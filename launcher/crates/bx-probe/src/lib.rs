//! Track A / F: Clean-room PE32+ static import & compatibility classifier.
//!
//! Categorizes PE32+ imported DLLs by BetterXen ownership track (A-E) without
//! executing the binary or embedding any title-specific metadata.

use std::collections::BTreeMap;
use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum ShimTrack {
    WineBuiltin,
    TrackAWin32Bridge,
    TrackBMemoryStorage,
    TrackCGraphics,
    TrackEPlatformAudio,
    BundledOrUnknown,
}

impl fmt::Display for ShimTrack {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::WineBuiltin => write!(f, "Wine / CrossOver built-in"),
            Self::TrackAWin32Bridge => write!(f, "Track A (win32_bridge)"),
            Self::TrackBMemoryStorage => write!(f, "Track B (xmem / XCRDAPI)"),
            Self::TrackCGraphics => write!(f, "Track C (d3d12_xs / xg_xs)"),
            Self::TrackEPlatformAudio => write!(f, "Track E (xgameruntime / GameInput / audio)"),
            Self::BundledOrUnknown => write!(f, "Bundled / Third-party"),
        }
    }
}

pub fn classify_dll(dll_name: &str) -> ShimTrack {
    let lower = dll_name.to_ascii_lowercase();
    match lower.as_str() {
        "d3d12_xs.dll" | "d3d12_x.dll" | "xg_xs.dll" | "xg_x.dll" => ShimTrack::TrackCGraphics,
        "xmem.dll" | "xcrdapi.dll" | "dstorage.dll" | "dstoragecore.dll" => {
            ShimTrack::TrackBMemoryStorage
        }
        "xgameruntime.dll" | "gameinput.dll" | "acphal.dll" | "xapu.dll" | "chad.dll" => {
            ShimTrack::TrackEPlatformAudio
        }
        s if s.starts_with("api-ms-win-core-psm-appnotify-")
            || s.starts_with("ext-ms-win-rtcore-ntuser-") =>
        {
            ShimTrack::TrackAWin32Bridge
        }
        s if s.starts_with("api-ms-win-")
            || matches!(
                s,
                "kernel32.dll"
                    | "ntdll.dll"
                    | "user32.dll"
                    | "gdi32.dll"
                    | "advapi32.dll"
                    | "bcrypt.dll"
                    | "crypt32.dll"
                    | "ws2_32.dll"
                    | "winhttp.dll"
                    | "ole32.dll"
                    | "rpcrt4.dll"
                    | "mfplat.dll"
                    | "mfreadwrite.dll"
                    | "xaudio2_9.dll"
                    | "msvcp140.dll"
                    | "vcruntime140.dll"
                    | "vcruntime140_1.dll"
                    | "ucrtbase.dll"
            ) =>
        {
            ShimTrack::WineBuiltin
        }
        _ => ShimTrack::BundledOrUnknown,
    }
}

#[derive(Debug, Clone)]
pub struct PeSummary {
    pub image_base: u64,
    pub size_of_image: u32,
    pub entry_point_rva: u32,
    pub section_count: u16,
    pub imports: BTreeMap<String, Vec<String>>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProbeError {
    NotPe32Plus(&'static str),
}

impl fmt::Display for ProbeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NotPe32Plus(msg) => write!(f, "invalid PE32+ image: {msg}"),
        }
    }
}

impl std::error::Error for ProbeError {}

fn u16_le(b: &[u8], off: usize) -> Option<u16> {
    let s = b.get(off..off + 2)?;
    Some(u16::from_le_bytes([s[0], s[1]]))
}

fn u32_le(b: &[u8], off: usize) -> Option<u32> {
    let s = b.get(off..off + 4)?;
    Some(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}

fn u64_le(b: &[u8], off: usize) -> Option<u64> {
    let s = b.get(off..off + 8)?;
    Some(u64::from_le_bytes([
        s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
    ]))
}

fn read_cstr(b: &[u8], off: usize) -> Option<String> {
    let tail = b.get(off..)?;
    let end = tail.iter().position(|&c| c == 0)?;
    Some(String::from_utf8_lossy(&tail[..end]).into_owned())
}

struct Section {
    vaddr: u32,
    vsize: u32,
    raw_ptr: u32,
    raw_size: u32,
}

fn rva_to_off(sections: &[Section], rva: u32) -> Option<usize> {
    for s in sections {
        let span = s.vsize.max(s.raw_size);
        if rva >= s.vaddr && rva < s.vaddr.saturating_add(span) {
            let delta = rva - s.vaddr;
            if delta < s.raw_size {
                return Some((s.raw_ptr + delta) as usize);
            }
        }
    }
    None
}

/// Parse PE32+ headers and standard import directory from raw image bytes.
pub fn probe_pe32_plus(bytes: &[u8]) -> Result<PeSummary, ProbeError> {
    if bytes.len() < 0x40 || &bytes[0..2] != b"MZ" {
        return Err(ProbeError::NotPe32Plus("missing MZ header"));
    }
    let pe_off = u32_le(bytes, 0x3C).ok_or(ProbeError::NotPe32Plus("missing e_lfanew"))? as usize;
    if bytes.get(pe_off..pe_off + 4) != Some(b"PE\0\0") {
        return Err(ProbeError::NotPe32Plus("missing PE signature"));
    }
    let coff = pe_off + 4;
    let machine = u16_le(bytes, coff).ok_or(ProbeError::NotPe32Plus("truncated COFF"))?;
    if machine != 0x8664 {
        return Err(ProbeError::NotPe32Plus("not x86-64 machine"));
    }
    let section_count = u16_le(bytes, coff + 2).ok_or(ProbeError::NotPe32Plus("sections"))?;
    let opt_size = u16_le(bytes, coff + 16).ok_or(ProbeError::NotPe32Plus("opt_size"))? as usize;
    let opt = coff + 20;
    let magic = u16_le(bytes, opt).ok_or(ProbeError::NotPe32Plus("opt magic"))?;
    if magic != 0x020B {
        return Err(ProbeError::NotPe32Plus("not PE32+ (0x020B)"));
    }

    let entry_point_rva = u32_le(bytes, opt + 16).ok_or(ProbeError::NotPe32Plus("entry"))?;
    let image_base = u64_le(bytes, opt + 24).ok_or(ProbeError::NotPe32Plus("image_base"))?;
    let size_of_image = u32_le(bytes, opt + 56).ok_or(ProbeError::NotPe32Plus("size_of_image"))?;
    let num_rvas = u32_le(bytes, opt + 108).ok_or(ProbeError::NotPe32Plus("num_rvas"))?;

    let mut sections = Vec::with_capacity(section_count as usize);
    let sec_base = opt + opt_size;
    for i in 0..section_count as usize {
        let so = sec_base + i * 40;
        let vsize = u32_le(bytes, so + 8).ok_or(ProbeError::NotPe32Plus("sec vsize"))?;
        let vaddr = u32_le(bytes, so + 12).ok_or(ProbeError::NotPe32Plus("sec vaddr"))?;
        let raw_size = u32_le(bytes, so + 16).ok_or(ProbeError::NotPe32Plus("sec raw_size"))?;
        let raw_ptr = u32_le(bytes, so + 20).ok_or(ProbeError::NotPe32Plus("sec raw_ptr"))?;
        sections.push(Section {
            vaddr,
            vsize,
            raw_ptr,
            raw_size,
        });
    }

    let mut imports: BTreeMap<String, Vec<String>> = BTreeMap::new();
    if num_rvas > 1 {
        let imp_rva = u32_le(bytes, opt + 112 + 8).unwrap_or(0);
        if imp_rva != 0
            && let Some(mut desc_off) = rva_to_off(&sections, imp_rva)
        {
            while let (Some(orig_thunk), Some(name_rva), Some(first_thunk)) = (
                u32_le(bytes, desc_off),
                u32_le(bytes, desc_off + 12),
                u32_le(bytes, desc_off + 16),
            ) {
                if orig_thunk == 0 && name_rva == 0 && first_thunk == 0 {
                    break;
                }
                if let Some(name_off) = rva_to_off(&sections, name_rva)
                    && let Some(dll) = read_cstr(bytes, name_off)
                {
                    let thunk_rva = if orig_thunk != 0 {
                        orig_thunk
                    } else {
                        first_thunk
                    };
                    let mut syms = Vec::new();
                    if let Some(mut toff) = rva_to_off(&sections, thunk_rva) {
                        while let Some(entry) = u64_le(bytes, toff) {
                            if entry == 0 {
                                break;
                            }
                            if (entry & (1u64 << 63)) != 0 {
                                syms.push(format!("ORDINAL:{}", entry & 0xFFFF));
                            } else if let Some(hint_off) =
                                rva_to_off(&sections, (entry & 0x7FFF_FFFF) as u32)
                                && let Some(sym) = read_cstr(bytes, hint_off + 2)
                            {
                                syms.push(sym);
                            }
                            toff += 8;
                        }
                    }
                    imports.entry(dll).or_default().extend(syms);
                }
                desc_off += 20;
            }
        }
    }

    Ok(PeSummary {
        image_base,
        size_of_image,
        entry_point_rva,
        section_count,
        imports,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classifies_tracks_accurately() {
        assert_eq!(classify_dll("d3d12_xs.dll"), ShimTrack::TrackCGraphics);
        assert_eq!(classify_dll("xg_xs.dll"), ShimTrack::TrackCGraphics);
        assert_eq!(classify_dll("xmem.dll"), ShimTrack::TrackBMemoryStorage);
        assert_eq!(classify_dll("XCRDAPI.dll"), ShimTrack::TrackBMemoryStorage);
        assert_eq!(
            classify_dll("xgameruntime.dll"),
            ShimTrack::TrackEPlatformAudio
        );
        assert_eq!(classify_dll("AcpHal.dll"), ShimTrack::TrackEPlatformAudio);
        assert_eq!(
            classify_dll("api-ms-win-core-psm-appnotify-l1-1-0.dll"),
            ShimTrack::TrackAWin32Bridge
        );
        assert_eq!(
            classify_dll("ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll"),
            ShimTrack::TrackAWin32Bridge
        );
        assert_eq!(classify_dll("KERNEL32.dll"), ShimTrack::WineBuiltin);
    }
}
