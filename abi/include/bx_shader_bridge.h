#ifndef BETTERXEN_BX_SHADER_BRIDGE_H
#define BETTERXEN_BX_SHADER_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * C ABI contract between Track C (d3d12_xs.dll) and Track D (shader-transpiler).
 * Used at Create*PipelineState / CreateStateObject time to inspect shader blobs
 * (DXBC+DXIL vs DXBC+ACCC/FSPX RDNA2 ISA) and translate console-compiled shaders
 * into D3DMetal-consumable DXIL (or report unsupported forms cleanly).
 */

typedef enum bx_shader_kind {
    BX_SHADER_UNKNOWN       = 0,
    BX_SHADER_DXBC_DXIL     = 1, /* Standard DXIL inside DXBC container (fast path to D3DMetal) */
    BX_SHADER_DXBC_LEGACY   = 2, /* Legacy SM5 SHDR/SHEX inside DXBC */
    BX_SHADER_DXBC_ACCC     = 3, /* Console-compiled AMD RDNA2 (ACCC / FSPX) without DXIL */
    BX_SHADER_DXBC_DUAL     = 4  /* Contains both DXIL and ACCC chunks */
} bx_shader_kind_t;

typedef enum bx_shader_stage {
    BX_STAGE_UNKNOWN = 0,
    BX_STAGE_PIXEL   = 1,
    BX_STAGE_VERTEX  = 2,
    BX_STAGE_GEOM    = 3,
    BX_STAGE_HULL    = 4,
    BX_STAGE_DOMAIN  = 5,
    BX_STAGE_COMPUTE = 6,
    BX_STAGE_LIBRARY = 7,
    BX_STAGE_MESH    = 8,
    BX_STAGE_AMP     = 9
} bx_shader_stage_t;

typedef struct bx_shader_inspection {
    bx_shader_kind_t kind;
    bx_shader_stage_t stage;
    uint32_t shader_model_major;
    uint32_t shader_model_minor;
    uint32_t container_size;
    uint32_t dxil_chunk_size;
    uint32_t accc_chunk_size;
    uint32_t has_root_signature;
    uint8_t  digest_hash[16]; /* DXBC header digest or SHA-like key for caching */
} bx_shader_inspection_t;

typedef enum bx_shader_status {
    BX_SHADER_OK               = 0,
    BX_SHADER_ERR_INVALID_BLOB = 1,
    BX_SHADER_ERR_UNSUPPORTED  = 2,
    BX_SHADER_ERR_TRANSLATE    = 3,
    BX_SHADER_ERR_OOM          = 4
} bx_shader_status_t;

/* Inspect a raw shader bytecode blob without allocating. */
bx_shader_status_t bx_shader_inspect(
    const uint8_t* bytes,
    size_t len,
    bx_shader_inspection_t* out_info
);

/*
 * Convert an incoming console shader blob into a D3DMetal-compatible DXBC/DXIL blob.
 * On BX_SHADER_OK, caller must free `*out_bytes` with `bx_shader_free_buffer`.
 */
bx_shader_status_t bx_shader_prepare_for_d3dmetal(
    const uint8_t* in_bytes,
    size_t in_len,
    uint8_t** out_bytes,
    size_t* out_len
);

void bx_shader_free_buffer(uint8_t* ptr, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BETTERXEN_BX_SHADER_BRIDGE_H */
