//! Track D (M1-D1): Clean-room DXBC / DXIL / ACCC (`FSPX`) shader container classifier.
//!
//! Parses standard DirectX `DXBC` containers to classify whether an incoming shader
//! blob contains standard `DXIL` (fast path to D3DMetal 3.0), legacy `SHDR`/`SHEX`,
//! or console-compiled `ACCC` (`FSPX`) AMD RDNA 2 machine code.

use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShaderKind {
    Unknown,
    Dxil,
    LegacyDxbc,
    AcccOnly,
    DualDxilAndAccc,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShaderStage {
    Unknown,
    Pixel,
    Vertex,
    Geometry,
    Hull,
    Domain,
    Compute,
    Library,
    Mesh,
    Amplification,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChunkInfo {
    pub fourcc: [u8; 4],
    pub offset: u32,
    pub size: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ShaderInspection {
    pub kind: ShaderKind,
    pub stage: ShaderStage,
    pub shader_model_major: u8,
    pub shader_model_minor: u8,
    pub container_size: u32,
    pub digest: [u8; 16],
    pub has_root_signature: bool,
    pub dxil_size: u32,
    pub accc_size: u32,
    pub accc_is_fspx: bool,
    pub chunks: Vec<ChunkInfo>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ParseError {
    TooShort,
    BadMagic,
    UnsupportedVersion(u32),
    TruncatedChunkTable,
    ChunkOutOfBounds { index: usize },
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::TooShort => write!(f, "buffer too short for DXBC header"),
            Self::BadMagic => write!(f, "missing DXBC fourcc magic"),
            Self::UnsupportedVersion(v) => write!(f, "unsupported DXBC version {v}"),
            Self::TruncatedChunkTable => write!(f, "chunk offset table exceeds buffer"),
            Self::ChunkOutOfBounds { index } => write!(f, "chunk {index} out of bounds"),
        }
    }
}

impl std::error::Error for ParseError {}

fn read_u32_le(bytes: &[u8], off: usize) -> Option<u32> {
    let slice = bytes.get(off..off + 4)?;
    Some(u32::from_le_bytes([slice[0], slice[1], slice[2], slice[3]]))
}

fn decode_stage(program_type: u16) -> ShaderStage {
    match program_type {
        0 => ShaderStage::Pixel,
        1 => ShaderStage::Vertex,
        2 => ShaderStage::Geometry,
        3 => ShaderStage::Hull,
        4 => ShaderStage::Domain,
        5 => ShaderStage::Compute,
        6 => ShaderStage::Library,
        7 => ShaderStage::Mesh,
        8 => ShaderStage::Amplification,
        _ => ShaderStage::Unknown,
    }
}

/// Inspect a `DXBC` shader container blob and classify its payload chunks.
pub fn inspect_dxbc(bytes: &[u8]) -> Result<ShaderInspection, ParseError> {
    if bytes.len() < 32 {
        return Err(ParseError::TooShort);
    }
    if &bytes[0..4] != b"DXBC" {
        return Err(ParseError::BadMagic);
    }
    let mut digest = [0u8; 16];
    digest.copy_from_slice(&bytes[4..20]);

    let version = read_u32_le(bytes, 20).ok_or(ParseError::TooShort)?;
    if version != 1 {
        return Err(ParseError::UnsupportedVersion(version));
    }
    let container_size = read_u32_le(bytes, 24).ok_or(ParseError::TooShort)?;
    let chunk_count = read_u32_le(bytes, 28).ok_or(ParseError::TooShort)? as usize;

    let table_end = 32usize
        .checked_add(
            chunk_count
                .checked_mul(4)
                .ok_or(ParseError::TruncatedChunkTable)?,
        )
        .ok_or(ParseError::TruncatedChunkTable)?;
    if table_end > bytes.len() {
        return Err(ParseError::TruncatedChunkTable);
    }

    let mut chunks = Vec::with_capacity(chunk_count);
    let mut has_dxil = false;
    let mut has_accc = false;
    let mut has_legacy = false;
    let mut has_root_signature = false;
    let mut dxil_size = 0u32;
    let mut accc_size = 0u32;
    let mut accc_is_fspx = false;
    let mut stage = ShaderStage::Unknown;
    let mut sm_major = 0u8;
    let mut sm_minor = 0u8;

    for i in 0..chunk_count {
        let off = read_u32_le(bytes, 32 + i * 4).ok_or(ParseError::TruncatedChunkTable)? as usize;
        let hdr = bytes
            .get(off..off + 8)
            .ok_or(ParseError::ChunkOutOfBounds { index: i })?;
        let fourcc = [hdr[0], hdr[1], hdr[2], hdr[3]];
        let size = u32::from_le_bytes([hdr[4], hdr[5], hdr[6], hdr[7]]);
        let payload_start = off + 8;
        let payload_end = payload_start
            .checked_add(size as usize)
            .ok_or(ParseError::ChunkOutOfBounds { index: i })?;
        if payload_end > bytes.len() {
            return Err(ParseError::ChunkOutOfBounds { index: i });
        }
        let payload = &bytes[payload_start..payload_end];

        match &fourcc {
            b"DXIL" => {
                has_dxil = true;
                dxil_size = size;
                if let Some(prog_ver) = read_u32_le(payload, 0) {
                    stage = decode_stage((prog_ver >> 16) as u16);
                    sm_major = ((prog_ver >> 4) & 0xF) as u8;
                    sm_minor = (prog_ver & 0xF) as u8;
                }
            }
            b"ACCC" => {
                has_accc = true;
                accc_size = size;
                if payload.get(0..4) == Some(b"FSPX") {
                    accc_is_fspx = true;
                }
            }
            b"SHDR" | b"SHEX" => {
                has_legacy = true;
                if let Some(tok) = read_u32_le(payload, 0) {
                    stage = decode_stage((tok >> 16) as u16);
                    sm_major = ((tok >> 4) & 0xF) as u8;
                    sm_minor = (tok & 0xF) as u8;
                }
            }
            b"RTS0" => has_root_signature = true,
            _ => {}
        }

        chunks.push(ChunkInfo {
            fourcc,
            offset: off as u32,
            size,
        });
    }

    let kind = match (has_dxil, has_accc, has_legacy) {
        (true, true, _) => ShaderKind::DualDxilAndAccc,
        (true, false, _) => ShaderKind::Dxil,
        (false, true, _) => ShaderKind::AcccOnly,
        (false, false, true) => ShaderKind::LegacyDxbc,
        (false, false, false) => ShaderKind::Unknown,
    };

    Ok(ShaderInspection {
        kind,
        stage,
        shader_model_major: sm_major,
        shader_model_minor: sm_minor,
        container_size,
        digest,
        has_root_signature,
        dxil_size,
        accc_size,
        accc_is_fspx,
        chunks,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn build_synthetic_dxbc(chunks: &[(&[u8; 4], &[u8])]) -> Vec<u8> {
        let header_size = 32 + chunks.len() * 4;
        let mut out = Vec::new();
        out.extend_from_slice(b"DXBC");
        out.extend_from_slice(&[0xAB; 16]);
        out.extend_from_slice(&1u32.to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes()); // total size patched below
        out.extend_from_slice(&(chunks.len() as u32).to_le_bytes());

        let mut offset = header_size as u32;
        for (_, payload) in chunks {
            out.extend_from_slice(&offset.to_le_bytes());
            offset += 8 + payload.len() as u32;
        }
        for (fourcc, payload) in chunks {
            out.extend_from_slice(*fourcc);
            out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
            out.extend_from_slice(payload);
        }
        let total = out.len() as u32;
        out[24..28].copy_from_slice(&total.to_le_bytes());
        out
    }

    #[test]
    fn classifies_synthetic_dxil_cs_6_6() {
        let mut dxil_payload = Vec::new();
        let prog_ver: u32 = (5u32 << 16) | (6u32 << 4) | 6u32; // cs_6_6
        dxil_payload.extend_from_slice(&prog_ver.to_le_bytes());
        dxil_payload.extend_from_slice(&4u32.to_le_bytes());
        dxil_payload.extend_from_slice(b"DXIL");
        dxil_payload.extend_from_slice(&[0u8; 12]);

        let blob = build_synthetic_dxbc(&[(b"RTS0", &[1, 2, 3, 4]), (b"DXIL", &dxil_payload)]);
        let info = inspect_dxbc(&blob).expect("valid synthetic DXBC");
        assert_eq!(info.kind, ShaderKind::Dxil);
        assert_eq!(info.stage, ShaderStage::Compute);
        assert_eq!((info.shader_model_major, info.shader_model_minor), (6, 6));
        assert!(info.has_root_signature);
        assert!(!info.accc_is_fspx);
    }

    #[test]
    fn classifies_synthetic_accc_fspx() {
        let mut accc_payload = Vec::new();
        accc_payload.extend_from_slice(b"FSPX");
        accc_payload.extend_from_slice(&[0u8; 28]);

        let blob = build_synthetic_dxbc(&[(b"ACCC", &accc_payload)]);
        let info = inspect_dxbc(&blob).expect("valid synthetic ACCC");
        assert_eq!(info.kind, ShaderKind::AcccOnly);
        assert!(info.accc_is_fspx);
        assert_eq!(info.accc_size, 32);
    }

    #[test]
    fn rejects_invalid_magic() {
        assert_eq!(inspect_dxbc(&[0u8; 64]), Err(ParseError::BadMagic));
    }
}
