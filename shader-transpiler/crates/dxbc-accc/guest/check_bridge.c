/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Guest-side check for bx_shader_bridge.dll, the x86-64 Windows build of this crate (#445).
 *
 * Runs as a PE32+ guest under CrossOver/Wine, loads the DLL by full path from its own directory,
 * and drives the three entry points from abi/include/bx_shader_bridge.h with synthetic
 * containers plus one real DXC SM 6.6 compute blob. It counts the classifications and checks
 * them, and the prepare/free behaviour, against expectations written here from the DXBC
 * layout and the frozen header, not from the Rust implementation.
 *
 * Output: "BXSHADER" lines with each blob's classification, one "BXSHADER counts" line, then
 * PASS or FAIL. Exit code 0 on success, 1 on a failed check, 2 if the DLL cannot be loaded.
 */
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "bx_shader_bridge.h"
#include "fill_cs66.h" /* g_fill_cs66: DXC 1.9.2609 output, see d3dmetal-harness */

/* The Rust side asserts the same layout; agreeing here proves both sides read one struct. */
_Static_assert(sizeof(bx_shader_kind_t) == 4, "C enum must be 4 bytes");
_Static_assert(sizeof(bx_shader_inspection_t) == 48, "bx_shader_inspection_t size");
_Static_assert(offsetof(bx_shader_inspection_t, container_size) == 16, "container_size offset");
_Static_assert(offsetof(bx_shader_inspection_t, digest_hash) == 32, "digest_hash offset");

typedef bx_shader_status_t (*inspect_fn)(const uint8_t*, size_t, bx_shader_inspection_t*);
typedef bx_shader_status_t (*prepare_fn)(const uint8_t*, size_t, uint8_t**, size_t*);
typedef void (*free_fn)(uint8_t*, size_t);

static int failures;

#define CHECK(expr)                                                                \
    do {                                                                           \
        if (!(expr)) {                                                             \
            fprintf(stderr, "FAIL: check_bridge line %d: %s\n", __LINE__, #expr); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

/* --- Synthetic DXBC containers --- */

static void put32(uint8_t* at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

typedef struct {
    const char* fourcc;
    const uint8_t* data;
    uint32_t size;
} chunk_t;

/* DXBC layout: "DXBC", 16-byte digest, u32 version 1, u32 total size, u32 chunk count, then
 * one u32 offset per chunk; each chunk is fourcc, u32 size, payload. */
static uint32_t build_dxbc(uint8_t* out, size_t cap, const chunk_t* chunks, uint32_t count) {
    uint32_t at = 32 + 4 * count;
    for (uint32_t i = 0; i < count; ++i) at += 8 + chunks[i].size;
    if (at > cap) abort();
    memcpy(out, "DXBC", 4);
    memset(out + 4, 0x5A, 16);
    put32(out + 20, 1);
    put32(out + 24, at);
    put32(out + 28, count);
    uint32_t offset = 32 + 4 * count;
    for (uint32_t i = 0; i < count; ++i) {
        put32(out + 32 + 4 * i, offset);
        memcpy(out + offset, chunks[i].fourcc, 4);
        put32(out + offset + 4, chunks[i].size);
        memcpy(out + offset + 8, chunks[i].data, chunks[i].size);
        offset += 8 + chunks[i].size;
    }
    return at;
}

/* A 28-byte DxilProgramHeader: program version word (kind << 16 | major << 4 | minor), size
 * in dwords (7), "DXIL", DXIL version 1.0, bitcode offset 16, bitcode size 4, 4 bitcode bytes. */
static void dxil_program(uint8_t out[28], uint32_t kind, uint32_t major, uint32_t minor) {
    memset(out, 0, 28);
    put32(out, kind << 16 | major << 4 | minor);
    put32(out + 4, 7);
    memcpy(out + 8, "DXIL", 4);
    put32(out + 12, 0x0100);
    put32(out + 16, 16);
    put32(out + 20, 4);
    put32(out + 24, 0xDEC011D1u);
}

/* Size of the first chunk named fourcc in a well-formed container, 0 if absent. Written from
 * the DXBC layout, independently of the Rust parser. */
static uint32_t get32(const uint8_t* at) {
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static int chunk(const uint8_t* bytes, size_t len, const char* fourcc, uint32_t* size) {
    if (len < 32 || memcmp(bytes, "DXBC", 4) != 0) return 0;
    const uint32_t count = get32(bytes + 28);
    for (uint32_t i = 0; i < count && 36 + 4 * (size_t)i <= len; ++i) {
        const uint32_t offset = get32(bytes + 32 + 4 * i);
        if (offset + 8 <= len && memcmp(bytes + offset, fourcc, 4) == 0) {
            *size = get32(bytes + offset + 4);
            return 1;
        }
    }
    return 0;
}

/* --- Cases --- */

typedef struct {
    const char* name;
    const uint8_t* bytes;
    size_t len;
    bx_shader_status_t inspect_status;
    bx_shader_kind_t kind;
    bx_shader_stage_t stage;
    uint32_t sm_major, sm_minor;
    uint32_t container_size; /* the container's declared size, which may be less than len */
    bx_shader_status_t prepare_status;
} case_t;

static const char* kind_name(bx_shader_kind_t kind) {
    switch (kind) {
        case BX_SHADER_DXBC_DXIL: return "dxil";
        case BX_SHADER_DXBC_LEGACY: return "legacy";
        case BX_SHADER_DXBC_ACCC: return "accc_only";
        case BX_SHADER_DXBC_DUAL: return "dual";
        default: return "unknown";
    }
}

int main(void) {
    /* By full path beside this EXE, as a caller should (see the crate README). */
    wchar_t path[MAX_PATH];
    const DWORD path_len = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* slash = path_len && path_len < MAX_PATH ? wcsrchr(path, L'\\') : NULL;
    if (!slash || (size_t)(slash + 1 - path) + 21 > MAX_PATH) {
        fprintf(stderr, "FAIL: check_bridge: executable path too long\n");
        return 2;
    }
    wcscpy(slash + 1, L"bx_shader_bridge.dll");
    HMODULE dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        fprintf(stderr, "FAIL: check_bridge: bx_shader_bridge.dll not loaded (error %lu)\n", GetLastError());
        return 2;
    }
    inspect_fn inspect = (inspect_fn)(void (*)(void))GetProcAddress(dll, "bx_shader_inspect");
    prepare_fn prepare = (prepare_fn)(void (*)(void))GetProcAddress(dll, "bx_shader_prepare_for_d3dmetal");
    free_fn release = (free_fn)(void (*)(void))GetProcAddress(dll, "bx_shader_free_buffer");
    if (!inspect || !prepare || !release) {
        fprintf(stderr, "FAIL: check_bridge: missing export\n");
        return 2;
    }

    uint8_t dxil_cs66[28], dxil_ps60[28];
    dxil_program(dxil_cs66, 5, 6, 6); /* DXIL program kind 5 = compute */
    dxil_program(dxil_ps60, 0, 6, 0); /* kind 0 = pixel */
    const uint8_t rts0[4] = {1, 2, 3, 4};

    /* FSPX v1 (BetterXen's synthetic layout): header "FSPX", version 1, 1 section, flags 0;
     * descriptor {id 1 (stage config), offset 28, size 12}; body {stage 6 compute, SM 6.0}. */
    uint8_t fspx_v1[40] = {0};
    memcpy(fspx_v1, "FSPX", 4);
    put32(fspx_v1 + 4, 1);
    put32(fspx_v1 + 8, 1);
    put32(fspx_v1 + 16, 1);
    put32(fspx_v1 + 20, 28);
    put32(fspx_v1 + 24, 12);
    put32(fspx_v1 + 28, BX_STAGE_COMPUTE);
    put32(fspx_v1 + 32, 6);
    put32(fspx_v1 + 36, 0);
    /* FSPX version 0: magic-only stub, no sections. */
    uint8_t fspx_v0[16] = {'F', 'S', 'P', 'X'};
    /* SM 5.0 compute SHEX token (program type 5 << 16 | 5 << 4 | 0) and a length dword. */
    uint8_t shex[8];
    put32(shex, 5u << 16 | 5u << 4);
    put32(shex + 4, 2);

    static uint8_t ps60[256], accc_v1[256], accc_v0[256], dual[256], legacy[256];
    const chunk_t ps60_chunks[] = {{"RTS0", rts0, 4}, {"DXIL", dxil_ps60, 28}};
    const chunk_t accc_v1_chunks[] = {{"ACCC", fspx_v1, sizeof(fspx_v1)}};
    const chunk_t accc_v0_chunks[] = {{"ACCC", fspx_v0, sizeof(fspx_v0)}};
    const chunk_t dual_chunks[] = {{"DXIL", dxil_cs66, 28}, {"ACCC", fspx_v0, sizeof(fspx_v0)}};
    const chunk_t legacy_chunks[] = {{"SHEX", shex, sizeof(shex)}};
    const uint32_t ps60_len = build_dxbc(ps60, sizeof(ps60), ps60_chunks, 2);
    const uint32_t accc_v1_len = build_dxbc(accc_v1, sizeof(accc_v1), accc_v1_chunks, 1);
    const uint32_t accc_v0_len = build_dxbc(accc_v0, sizeof(accc_v0), accc_v0_chunks, 1);
    const uint32_t dual_len = build_dxbc(dual, sizeof(dual), dual_chunks, 2);
    const uint32_t legacy_len = build_dxbc(legacy, sizeof(legacy), legacy_chunks, 1);
    /* The pixel shader again, followed by 16 bytes that are not part of the container. */
    static uint8_t padded[256 + 16];
    memcpy(padded, ps60, ps60_len);
    memset(padded + ps60_len, 0xEE, 16);
    static const uint8_t truncated[20] = {'D', 'X', 'B', 'C'};
    static const uint8_t not_dxbc[64] = {'X', 'X', 'X', 'X'};

    const case_t cases[] = {
        {"dxc_cs_6_6", g_fill_cs66, sizeof(g_fill_cs66), BX_SHADER_OK, BX_SHADER_DXBC_DXIL, BX_STAGE_COMPUTE, 6, 6,
         sizeof(g_fill_cs66), BX_SHADER_OK},
        {"synthetic_ps_6_0", ps60, ps60_len, BX_SHADER_OK, BX_SHADER_DXBC_DXIL, BX_STAGE_PIXEL, 6, 0, ps60_len,
         BX_SHADER_OK},
        {"trailing_bytes", padded, ps60_len + 16, BX_SHADER_OK, BX_SHADER_DXBC_DXIL, BX_STAGE_PIXEL, 6, 0, ps60_len,
         BX_SHADER_OK},
        {"accc_fspx_v1", accc_v1, accc_v1_len, BX_SHADER_OK, BX_SHADER_DXBC_ACCC, BX_STAGE_COMPUTE, 6, 0, accc_v1_len,
         BX_SHADER_ERR_UNSUPPORTED},
        {"accc_fspx_v0", accc_v0, accc_v0_len, BX_SHADER_OK, BX_SHADER_DXBC_ACCC, BX_STAGE_UNKNOWN, 0, 0, accc_v0_len,
         BX_SHADER_ERR_UNSUPPORTED},
        {"dual_dxil_accc", dual, dual_len, BX_SHADER_OK, BX_SHADER_DXBC_DUAL, BX_STAGE_COMPUTE, 6, 6, dual_len,
         BX_SHADER_OK},
        {"legacy_shex_5_0", legacy, legacy_len, BX_SHADER_OK, BX_SHADER_DXBC_LEGACY, BX_STAGE_COMPUTE, 5, 0,
         legacy_len, BX_SHADER_ERR_UNSUPPORTED},
        {"truncated", truncated, sizeof(truncated), BX_SHADER_ERR_INVALID_BLOB, BX_SHADER_UNKNOWN, BX_STAGE_UNKNOWN,
         0, 0, 0, BX_SHADER_ERR_INVALID_BLOB},
        {"not_dxbc", not_dxbc, sizeof(not_dxbc), BX_SHADER_ERR_INVALID_BLOB, BX_SHADER_UNKNOWN, BX_STAGE_UNKNOWN, 0,
         0, 0, BX_SHADER_ERR_INVALID_BLOB},
    };
    const size_t count = sizeof(cases) / sizeof(cases[0]);

    unsigned tally[5] = {0};
    unsigned invalid = 0;
    for (size_t i = 0; i < count; ++i) {
        const case_t* c = &cases[i];
        bx_shader_inspection_t info;
        memset(&info, 0xEE, sizeof(info));
        const bx_shader_status_t status = inspect(c->bytes, c->len, &info);
        printf("BXSHADER %s status=%d kind=%s stage=%d sm=%u.%u container_size=%u dxil=%u accc=%u rts0=%u\n",
               c->name, (int)status, kind_name(info.kind), (int)info.stage, info.shader_model_major,
               info.shader_model_minor, info.container_size, info.dxil_chunk_size, info.accc_chunk_size,
               info.has_root_signature);
        CHECK(status == c->inspect_status);
        if (status == BX_SHADER_OK) {
            if ((unsigned)info.kind < 5) ++tally[info.kind];
            CHECK(info.kind == c->kind);
            CHECK(info.stage == c->stage);
            CHECK(info.shader_model_major == c->sm_major && info.shader_model_minor == c->sm_minor);
            CHECK(info.container_size == c->container_size);
            /* Exact chunk sizes and the root signature flag, from this file's own chunk walk. */
            uint32_t want_dxil = 0, want_accc = 0, rts0_size = 0;
            chunk(c->bytes, c->len, "DXIL", &want_dxil);
            chunk(c->bytes, c->len, "ACCC", &want_accc);
            const uint32_t want_rts0 = (uint32_t)chunk(c->bytes, c->len, "RTS0", &rts0_size);
            CHECK(info.dxil_chunk_size == want_dxil);
            CHECK(info.accc_chunk_size == want_accc);
            CHECK(info.has_root_signature == want_rts0);
            CHECK((want_dxil != 0) == (c->kind == BX_SHADER_DXBC_DXIL || c->kind == BX_SHADER_DXBC_DUAL));
            CHECK((want_accc != 0) == (c->kind == BX_SHADER_DXBC_ACCC || c->kind == BX_SHADER_DXBC_DUAL));
            /* The digest is the container header's bytes 4..20, for every case. */
            CHECK(memcmp(info.digest_hash, c->bytes + 4, 16) == 0);
        } else {
            ++invalid;
            const bx_shader_inspection_t zero = {0};
            CHECK(memcmp(&info, &zero, sizeof(info)) == 0); /* cleared on failure */
        }

        uint8_t* copy = (uint8_t*)(uintptr_t)1;
        size_t copy_len = 1;
        const bx_shader_status_t prepared = prepare(c->bytes, c->len, &copy, &copy_len);
        CHECK(prepared == c->prepare_status);
        if (prepared == BX_SHADER_OK) {
            /* Only the declared container is copied, never bytes past it. */
            CHECK(copy && copy_len == c->container_size && memcmp(copy, c->bytes, c->container_size) == 0);
            release(copy, copy_len);
        } else {
            CHECK(copy == NULL && copy_len == 0);
        }
    }

    /* Argument checks at the DLL boundary. */
    bx_shader_inspection_t info;
    CHECK(inspect(g_fill_cs66, sizeof(g_fill_cs66), NULL) == BX_SHADER_ERR_INVALID_BLOB);
    CHECK(inspect(NULL, 16, &info) == BX_SHADER_ERR_INVALID_BLOB);
    uint8_t* copy = NULL;
    CHECK(prepare(g_fill_cs66, sizeof(g_fill_cs66), NULL, NULL) == BX_SHADER_ERR_INVALID_BLOB);
    CHECK(prepare(NULL, 16, &copy, &(size_t){0}) == BX_SHADER_ERR_INVALID_BLOB && copy == NULL);
    release(NULL, 0);

    printf("BXSHADER counts dxil=%u accc_only=%u dual=%u legacy=%u unknown=%u invalid=%u\n",
           tally[BX_SHADER_DXBC_DXIL], tally[BX_SHADER_DXBC_ACCC], tally[BX_SHADER_DXBC_DUAL],
           tally[BX_SHADER_DXBC_LEGACY], tally[BX_SHADER_UNKNOWN], invalid);
    CHECK(tally[BX_SHADER_DXBC_DXIL] == 3 && tally[BX_SHADER_DXBC_ACCC] == 2 && tally[BX_SHADER_DXBC_DUAL] == 1);
    CHECK(tally[BX_SHADER_DXBC_LEGACY] == 1 && tally[BX_SHADER_UNKNOWN] == 0 && invalid == 2);

    if (failures) {
        fprintf(stderr, "FAIL: check_bridge (%d failed checks)\n", failures);
        return 1;
    }
    printf("PASS: check_bridge (%u containers through bx_shader_bridge.dll)\n", (unsigned)count);
    return 0;
}
