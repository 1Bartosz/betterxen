#include "xg_texture.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "bx_log.h"

/* File-local helpers. Exported entry points stay at the bottom with C linkage. */

/*
 * Linear 2D layouts follow the public D3D12 copy footprint rules:
 *   mip size     = max(1, base >> mip)
 *   block count  = ceil(texels / block), at least 1
 *   row bytes    = block_columns * bytes_per_block
 *   row pitch    = align_up(row_bytes, 256)
 *   subresource  = row_pitch * block_rows
 *   next offset  = align_up(offset + subresource, 512)
 * Subresource index is mip + array_slice * mip_count.
 * BC1 and BC4 store 8 bytes per 4x4 block. BC2, BC3, BC5, BC6H, and BC7
 * store 16. R8G8B8A8 stores 4 bytes per texel.
 * SW_64KB_S, SW_64KB_D, and SW_64KB_R do not produce addresses.
 */

constexpr uint32_t kPitchAlignment = BX_XG_LINEAR_PITCH_ALIGNMENT;
constexpr uint32_t kPlacementAlignment = BX_XG_LINEAR_PLACEMENT_ALIGNMENT;

static_assert(static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_TYPELESS) == 27u, "DXGI R8G8B8A8_TYPELESS");
static_assert(static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM) == 28u, "DXGI R8G8B8A8_UNORM");
static_assert(static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_SINT) == 32u, "DXGI R8G8B8A8_SINT");
static_assert(static_cast<uint32_t>(DXGI_FORMAT_BC1_TYPELESS) == 70u, "DXGI BC1_TYPELESS");
static_assert(static_cast<uint32_t>(DXGI_FORMAT_BC1_UNORM) == 71u, "DXGI BC1_UNORM");
static_assert(static_cast<uint32_t>(DXGI_FORMAT_BC4_SNORM) == 81u, "DXGI BC4_SNORM");
static_assert(static_cast<uint32_t>(DXGI_FORMAT_BC5_TYPELESS) == 82u, "DXGI BC5_TYPELESS");
static_assert(static_cast<uint32_t>(DXGI_FORMAT_BC6H_TYPELESS) == 94u, "DXGI BC6H_TYPELESS");
static_assert(static_cast<uint32_t>(DXGI_FORMAT_BC6H_UF16) == 95u, "DXGI BC6H_UF16");
static_assert(static_cast<uint32_t>(DXGI_FORMAT_BC7_UNORM) == 98u, "DXGI BC7_UNORM");
static_assert(static_cast<uint32_t>(DXGI_FORMAT_BC7_UNORM_SRGB) == 99u, "DXGI BC7_UNORM_SRGB");

struct Resolved {
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;
    uint32_t array_size;
    uint32_t subresource_count;
    uint32_t format;
    uint32_t block_w;
    uint32_t block_h;
    uint32_t block_bytes;
    uint32_t layout;
};

struct TextureComputer {
    bx_xg_texture_computer iface;
    volatile LONG refcount;
    Resolved resolved;
};

struct Placement {
    uint64_t offset;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t row_pitch;
    uint32_t row_count;
    uint64_t row_bytes;
    uint32_t format;
};

struct Rdna2TileMode {
    uint32_t mode;
    uint32_t tile_bytes;
    const char* name;
};

struct Rdna2TileQuery {
    uint32_t mode;
    uint32_t mip_width;
    uint32_t mip_height;
    uint32_t block_bytes;
    uint32_t texel_x;
    uint32_t texel_y;
};

/* Named 64 KiB tiles. The address equation is intentionally absent. */
constexpr Rdna2TileMode kRdna2TileModes[] = {
    {BX_XG_LAYOUT_SW_64KB_S, 64u * 1024u, "SW_64KB_S"},
    {BX_XG_LAYOUT_SW_64KB_D, 64u * 1024u, "SW_64KB_D"},
    {BX_XG_LAYOUT_SW_64KB_R, 64u * 1024u, "SW_64KB_R"},
};

constexpr uint32_t kSlotGetResourceLayout = 3u;
constexpr uint32_t kSlotGetResourceSizeBytes = 4u;
constexpr uint32_t kSlotGetResourceBaseAlignmentBytes = 5u;
constexpr uint32_t kSlotGetMipLevelOffsetBytes = 6u;
constexpr uint32_t kSlotGetTexelElementOffsetBytes = 7u;
constexpr uint32_t kSlotCopyIntoSubresource = 8u;
constexpr uint32_t kSlotCopyFromSubresource = 9u;

static bool mul_u64(uint64_t a, uint64_t b, uint64_t* out) {
    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

static bool add_u64(uint64_t a, uint64_t b, uint64_t* out) {
    if (a > UINT64_MAX - b) {
        return false;
    }
    *out = a + b;
    return true;
}

static bool align_up_u64(uint64_t value, uint64_t alignment, uint64_t* out) {
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0) {
        return false;
    }
    const uint64_t mask = alignment - 1u;
    if (value > UINT64_MAX - mask) {
        return false;
    }
    *out = (value + mask) & ~mask;
    return true;
}

static uint32_t div_up_u32(uint32_t value, uint32_t block) {
    return (value + block - 1u) / block;
}

static uint32_t mip_size(uint32_t base, uint32_t mip) {
    if (mip >= 31u) {
        return 1u;
    }
    const uint32_t scaled = base >> mip;
    return scaled == 0u ? 1u : scaled;
}

static uint32_t full_mip_count(uint32_t width, uint32_t height) {
    uint32_t levels = 1u;
    while (width > 1u || height > 1u) {
        if (width > 1u) {
            width >>= 1u;
        }
        if (height > 1u) {
            height >>= 1u;
        }
        levels += 1u;
    }
    return levels;
}

static bool guid_eq(const GUID* a, const GUID* b) {
    return std::memcmp(a, b, sizeof(GUID)) == 0;
}

static bool lookup_format(uint32_t format, uint32_t* block_w, uint32_t* block_h, uint32_t* block_bytes) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
        *block_w = 1u;
        *block_h = 1u;
        *block_bytes = 4u;
        return true;
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        *block_w = 4u;
        *block_h = 4u;
        *block_bytes = 8u;
        return true;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        *block_w = 4u;
        *block_h = 4u;
        *block_bytes = 16u;
        return true;
    default:
        return false;
    }
}

static bool layout_is_tiled(uint32_t layout) {
    for (const Rdna2TileMode& mode : kRdna2TileModes) {
        if (mode.mode == layout) {
            return true;
        }
    }
    return false;
}

static HRESULT validate_desc(const bx_xg_resource_desc* desc, Resolved* out) {
    if (desc->dimension == BX_XG_DIMENSION_TEXTURE1D || desc->dimension == BX_XG_DIMENSION_TEXTURE3D) {
        return E_NOTIMPL;
    }
    if (desc->dimension != BX_XG_DIMENSION_TEXTURE2D) {
        return E_INVALIDARG;
    }
    if (desc->flags != 0u) {
        return E_INVALIDARG;
    }
    if (desc->alignment != 0u && desc->alignment != kPlacementAlignment) {
        return E_INVALIDARG;
    }
    if (desc->width == 0 || desc->width > BX_XG_MAX_TEXTURE_EDGE || desc->height == 0u ||
        desc->height > BX_XG_MAX_TEXTURE_EDGE) {
        return E_INVALIDARG;
    }
    if (desc->depth_or_array_size == 0u || desc->depth_or_array_size > BX_XG_MAX_ARRAY_SLICES) {
        return E_INVALIDARG;
    }
    if (desc->sample_desc.count == 0u || desc->sample_desc.quality != 0u) {
        return E_INVALIDARG;
    }
    if (desc->sample_desc.count != 1u) {
        return E_NOTIMPL;
    }
    if (desc->layout != BX_XG_LAYOUT_LINEAR && !layout_is_tiled(desc->layout)) {
        return E_INVALIDARG;
    }

    uint32_t block_w = 0;
    uint32_t block_h = 0;
    uint32_t block_bytes = 0;
    if (!lookup_format(desc->format, &block_w, &block_h, &block_bytes)) {
        return E_INVALIDARG;
    }

    const uint32_t width = static_cast<uint32_t>(desc->width);
    const uint32_t height = desc->height;
    const uint32_t max_mips = full_mip_count(width, height);
    const uint32_t mips = desc->mip_levels == 0u ? max_mips : desc->mip_levels;
    if (mips == 0u || mips > max_mips) {
        return E_INVALIDARG;
    }
    if (desc->depth_or_array_size > UINT32_MAX / mips) {
        return E_INVALIDARG;
    }

    out->width = width;
    out->height = height;
    out->mip_levels = mips;
    out->array_size = desc->depth_or_array_size;
    out->subresource_count = mips * desc->depth_or_array_size;
    out->format = desc->format;
    out->block_w = block_w;
    out->block_h = block_h;
    out->block_bytes = block_bytes;
    out->layout = desc->layout;
    return S_OK;
}

static TextureComputer* impl_from(bx_xg_texture_computer* self) {
    static_assert(offsetof(TextureComputer, iface) == 0, "COM object must start with its vtable pointer");
    return reinterpret_cast<TextureComputer*>(self);
}

/* M3 placeholder. Validates the tile request and returns no address. */
static HRESULT rdna2_sw64kb_prepare(const Rdna2TileQuery* query) {
    if (!query || query->mip_width == 0u || query->mip_height == 0u || query->block_bytes == 0u) {
        return E_INVALIDARG;
    }
    const Rdna2TileMode* found = nullptr;
    for (const Rdna2TileMode& mode : kRdna2TileModes) {
        if (mode.mode == query->mode) {
            found = &mode;
            break;
        }
    }
    if (!found || found->tile_bytes != (64u * 1024u) || found->name == nullptr) {
        return E_INVALIDARG;
    }
    return E_NOTIMPL;
}

static HRESULT reject_if_tiled(const TextureComputer* obj, uint32_t slot, const char* method) {
    if (obj->resolved.layout == BX_XG_LAYOUT_LINEAR) {
        return S_OK;
    }
    Rdna2TileQuery query{};
    query.mode = obj->resolved.layout;
    query.mip_width = obj->resolved.width;
    query.mip_height = obj->resolved.height;
    query.block_bytes = obj->resolved.block_bytes;
    query.texel_x = 0u;
    query.texel_y = 0u;
    const HRESULT hr = rdna2_sw64kb_prepare(&query);
    if (hr == E_NOTIMPL) {
        BX_VTABLE_STUB_LOG("IXGTextureAddressComputer", slot, method);
    }
    return hr;
}

static HRESULT measure_mip(const Resolved* resolved, uint32_t mip, Placement* place) {
    const uint32_t width = mip_size(resolved->width, mip);
    const uint32_t height = mip_size(resolved->height, mip);
    const uint32_t blocks_w = div_up_u32(width, resolved->block_w);
    const uint32_t blocks_h = div_up_u32(height, resolved->block_h);
    uint64_t row_bytes = 0;
    if (!mul_u64(blocks_w, resolved->block_bytes, &row_bytes) || row_bytes == 0u) {
        return E_INVALIDARG;
    }
    uint64_t row_pitch = 0;
    if (!align_up_u64(row_bytes, kPitchAlignment, &row_pitch) || row_pitch > UINT32_MAX) {
        return E_INVALIDARG;
    }
    place->offset = 0;
    place->width = width;
    place->height = height;
    place->depth = 1u;
    place->row_pitch = static_cast<uint32_t>(row_pitch);
    place->row_count = blocks_h;
    place->row_bytes = row_bytes;
    place->format = resolved->format;
    return S_OK;
}

static HRESULT advance_cursor(uint64_t cursor, const Placement* place, uint64_t* next) {
    uint64_t payload = 0;
    uint64_t raw_end = 0;
    if (!mul_u64(place->row_pitch, place->row_count, &payload) || !add_u64(cursor, payload, &raw_end)) {
        return E_INVALIDARG;
    }
    if (!align_up_u64(raw_end, kPlacementAlignment, next)) {
        return E_INVALIDARG;
    }
    return S_OK;
}

using PlacementSink = void (*)(void* ctx, uint32_t slot, const Placement* place);

static HRESULT walk_placements(
    const Resolved* resolved,
    uint64_t base_offset,
    uint32_t first,
    uint32_t last_exclusive,
    PlacementSink sink,
    void* sink_ctx,
    uint64_t* aligned_end
) {
    uint64_t cursor = base_offset;
    uint64_t end = base_offset;
    for (uint32_t index = 0; index < last_exclusive; ++index) {
        Placement place{};
        const HRESULT hr = measure_mip(resolved, index % resolved->mip_levels, &place);
        if (hr != S_OK) {
            return hr;
        }
        place.offset = cursor;
        uint64_t next = 0;
        const HRESULT advanced = advance_cursor(cursor, &place, &next);
        if (advanced != S_OK) {
            return advanced;
        }
        if (sink && index >= first) {
            sink(sink_ctx, index - first, &place);
        }
        end = next;
        cursor = next;
    }
    *aligned_end = end;
    return S_OK;
}

struct OnePlacement {
    Placement place;
    bool written;
};

static void sink_one(void* ctx, uint32_t /*slot*/, const Placement* place) {
    auto* out = static_cast<OnePlacement*>(ctx);
    out->place = *place;
    out->written = true;
}

static HRESULT placement_at(const Resolved* resolved, uint32_t index, Placement* out) {
    if (index >= resolved->subresource_count) {
        return E_INVALIDARG;
    }
    OnePlacement captured{};
    uint64_t end = 0;
    const HRESULT hr = walk_placements(resolved, 0, index, index + 1u, sink_one, &captured, &end);
    if (hr != S_OK) {
        return hr;
    }
    if (!captured.written) {
        return E_FAIL;
    }
    *out = captured.place;
    return S_OK;
}

struct LayoutOut {
    bx_xg_placed_footprint* layouts;
    uint32_t* row_counts;
    uint64_t* row_sizes;
};

static void sink_layout(void* ctx, uint32_t slot, const Placement* place) {
    auto* out = static_cast<LayoutOut*>(ctx);
    out->layouts[slot].offset = place->offset;
    out->layouts[slot].footprint.format = place->format;
    out->layouts[slot].footprint.width = place->width;
    out->layouts[slot].footprint.height = place->height;
    out->layouts[slot].footprint.depth = place->depth;
    out->layouts[slot].footprint.row_pitch = place->row_pitch;
    out->row_counts[slot] = place->row_count;
    out->row_sizes[slot] = place->row_bytes;
}

static bool row_bounds(
    uint64_t origin,
    uint64_t row_pitch,
    uint64_t depth_pitch,
    uint32_t z,
    uint32_t y,
    uint64_t row_bytes,
    uint64_t size,
    uint64_t* begin,
    uint64_t* end
) {
    uint64_t zoff = 0;
    uint64_t yoff = 0;
    uint64_t start = 0;
    uint64_t stop = 0;
    if (!mul_u64(z, depth_pitch, &zoff) || !mul_u64(y, row_pitch, &yoff)) {
        return false;
    }
    if (!add_u64(origin, zoff, &start) || !add_u64(start, yoff, &start) || !add_u64(start, row_bytes, &stop)) {
        return false;
    }
    if (stop < start || stop > size) {
        return false;
    }
    *begin = start;
    *end = stop;
    return true;
}

static HRESULT resolve_image_pitch(const Placement* place, uint32_t row_pitch_in, uint32_t depth_pitch_in, uint64_t* row_pitch, uint64_t* depth_pitch) {
    const uint64_t row = row_pitch_in == 0u ? place->row_bytes : row_pitch_in;
    if (row < place->row_bytes || row > UINT32_MAX) {
        return E_INVALIDARG;
    }
    uint64_t depth = 0;
    if (depth_pitch_in == 0u) {
        if (!mul_u64(row, place->row_count, &depth)) {
            return E_INVALIDARG;
        }
    } else {
        depth = depth_pitch_in;
        if (place->depth > 1u && place->row_count > 0u) {
            uint64_t min_span = 0;
            if (!mul_u64(place->row_count - 1u, row, &min_span) || !add_u64(min_span, place->row_bytes, &min_span)) {
                return E_INVALIDARG;
            }
            if (depth < min_span) {
                return E_INVALIDARG;
            }
        }
    }
    *row_pitch = row;
    *depth_pitch = depth;
    return S_OK;
}

static HRESULT copy_rows(
    const Placement* place,
    uint8_t* dst,
    uint64_t dst_size,
    uint64_t dst_origin,
    uint64_t dst_row_pitch,
    uint64_t dst_depth_pitch,
    const uint8_t* src,
    uint64_t src_size,
    uint64_t src_origin,
    uint64_t src_row_pitch,
    uint64_t src_depth_pitch
) {
    if (!dst || !src || place->row_count == 0u || place->depth == 0u || place->row_bytes == 0u) {
        return E_INVALIDARG;
    }
    if (dst_row_pitch < place->row_bytes || src_row_pitch < place->row_bytes) {
        return E_INVALIDARG;
    }
    if (place->row_bytes > SIZE_MAX) {
        return E_INVALIDARG;
    }

    for (uint32_t z = 0; z < place->depth; ++z) {
        for (uint32_t y = 0; y < place->row_count; ++y) {
            uint64_t src_begin = 0;
            uint64_t src_end = 0;
            uint64_t dst_begin = 0;
            uint64_t dst_end = 0;
            if (!row_bounds(src_origin, src_row_pitch, src_depth_pitch, z, y, place->row_bytes, src_size, &src_begin, &src_end) ||
                !row_bounds(dst_origin, dst_row_pitch, dst_depth_pitch, z, y, place->row_bytes, dst_size, &dst_begin, &dst_end)) {
                return E_INVALIDARG;
            }
        }
    }

    for (uint32_t z = 0; z < place->depth; ++z) {
        for (uint32_t y = 0; y < place->row_count; ++y) {
            uint64_t src_begin = 0;
            uint64_t src_end = 0;
            uint64_t dst_begin = 0;
            uint64_t dst_end = 0;
            if (!row_bounds(src_origin, src_row_pitch, src_depth_pitch, z, y, place->row_bytes, src_size, &src_begin, &src_end) ||
                !row_bounds(dst_origin, dst_row_pitch, dst_depth_pitch, z, y, place->row_bytes, dst_size, &dst_begin, &dst_end)) {
                return E_FAIL;
            }
            std::memcpy(dst + dst_begin, src + src_begin, static_cast<size_t>(place->row_bytes));
        }
    }
    return S_OK;
}

static HRESULT image_and_resource_pitch(
    const Placement* place,
    uint32_t image_row_pitch,
    uint32_t image_depth_pitch,
    uint64_t* image_row,
    uint64_t* image_depth,
    uint64_t* resource_depth
) {
    const HRESULT pitch = resolve_image_pitch(place, image_row_pitch, image_depth_pitch, image_row, image_depth);
    if (pitch != S_OK) {
        return pitch;
    }
    if (!mul_u64(place->row_pitch, place->row_count, resource_depth)) {
        return E_INVALIDARG;
    }
    return S_OK;
}

static ULONG WINAPI computer_add_ref(bx_xg_texture_computer* self) {
    if (!self) {
        return 0u;
    }
    const LONG count = InterlockedIncrement(&impl_from(self)->refcount);
    return static_cast<ULONG>(count);
}

static ULONG WINAPI computer_release(bx_xg_texture_computer* self) {
    if (!self) {
        return 0u;
    }
    TextureComputer* obj = impl_from(self);
    const LONG count = InterlockedDecrement(&obj->refcount);
    if (count == 0) {
        HeapFree(GetProcessHeap(), 0, obj);
        return 0u;
    }
    if (count < 0) {
        return 0u;
    }
    return static_cast<ULONG>(count);
}

static HRESULT WINAPI computer_query_interface(bx_xg_texture_computer* self, const GUID* riid, void** object) {
    if (!self || !riid || !object) {
        return E_POINTER;
    }
    *object = nullptr;
    if (!guid_eq(riid, &BX_IID_IUnknown) && !guid_eq(riid, &BX_IID_IXGTextureAddressComputer)) {
        return E_NOINTERFACE;
    }
    *object = self;
    computer_add_ref(self);
    return S_OK;
}

static HRESULT WINAPI computer_get_resource_layout(
    bx_xg_texture_computer* self,
    uint32_t first_subresource,
    uint32_t subresource_count,
    uint64_t base_offset,
    bx_xg_placed_footprint* layouts,
    uint32_t* row_counts,
    uint64_t* row_sizes_in_bytes,
    uint64_t* total_bytes
) {
    if (!self || !layouts || !row_counts || !row_sizes_in_bytes || !total_bytes) {
        return E_POINTER;
    }
    TextureComputer* obj = impl_from(self);
    const HRESULT tiled = reject_if_tiled(obj, kSlotGetResourceLayout, "GetResourceLayout");
    if (tiled != S_OK) {
        return tiled;
    }
    if (subresource_count == 0u || (base_offset % kPlacementAlignment) != 0u) {
        return E_INVALIDARG;
    }
    if (first_subresource > obj->resolved.subresource_count ||
        subresource_count > obj->resolved.subresource_count - first_subresource) {
        return E_INVALIDARG;
    }
    uint64_t end = 0;
    const HRESULT checked = walk_placements(
        &obj->resolved, base_offset, first_subresource, first_subresource + subresource_count, nullptr, nullptr, &end);
    if (checked != S_OK) {
        return checked;
    }
    if (end < base_offset) {
        return E_FAIL;
    }
    LayoutOut out{layouts, row_counts, row_sizes_in_bytes};
    uint64_t end_again = 0;
    const HRESULT written = walk_placements(
        &obj->resolved,
        base_offset,
        first_subresource,
        first_subresource + subresource_count,
        sink_layout,
        &out,
        &end_again);
    if (written != S_OK || end_again != end) {
        return written != S_OK ? written : E_FAIL;
    }
    *total_bytes = end - base_offset;
    return S_OK;
}

static HRESULT WINAPI computer_get_resource_size_bytes(bx_xg_texture_computer* self, uint64_t* size_bytes) {
    if (!self || !size_bytes) {
        return E_POINTER;
    }
    TextureComputer* obj = impl_from(self);
    const HRESULT tiled = reject_if_tiled(obj, kSlotGetResourceSizeBytes, "GetResourceSizeBytes");
    if (tiled != S_OK) {
        return tiled;
    }
    uint64_t end = 0;
    const HRESULT hr = walk_placements(&obj->resolved, 0, 0, obj->resolved.subresource_count, nullptr, nullptr, &end);
    if (hr != S_OK) {
        return hr;
    }
    *size_bytes = end;
    return S_OK;
}

static HRESULT WINAPI computer_get_resource_base_alignment_bytes(bx_xg_texture_computer* self, uint64_t* alignment_bytes) {
    if (!self || !alignment_bytes) {
        return E_POINTER;
    }
    TextureComputer* obj = impl_from(self);
    const HRESULT tiled = reject_if_tiled(obj, kSlotGetResourceBaseAlignmentBytes, "GetResourceBaseAlignmentBytes");
    if (tiled != S_OK) {
        return tiled;
    }
    *alignment_bytes = kPlacementAlignment;
    return S_OK;
}

static HRESULT WINAPI computer_get_mip_level_offset_bytes(
    bx_xg_texture_computer* self,
    uint32_t array_slice,
    uint32_t mip_level,
    uint64_t* offset_bytes
) {
    if (!self || !offset_bytes) {
        return E_POINTER;
    }
    TextureComputer* obj = impl_from(self);
    const HRESULT tiled = reject_if_tiled(obj, kSlotGetMipLevelOffsetBytes, "GetMipLevelOffsetBytes");
    if (tiled != S_OK) {
        return tiled;
    }
    if (array_slice >= obj->resolved.array_size || mip_level >= obj->resolved.mip_levels) {
        return E_INVALIDARG;
    }
    if (array_slice > (UINT32_MAX - mip_level) / obj->resolved.mip_levels) {
        return E_INVALIDARG;
    }
    const uint32_t index = mip_level + array_slice * obj->resolved.mip_levels;
    Placement place{};
    const HRESULT hr = placement_at(&obj->resolved, index, &place);
    if (hr != S_OK) {
        return hr;
    }
    *offset_bytes = place.offset;
    return S_OK;
}

static HRESULT WINAPI computer_get_texel_element_offset_bytes(
    bx_xg_texture_computer* self,
    uint32_t array_slice,
    uint32_t mip_level,
    uint32_t x,
    uint32_t y,
    uint32_t z,
    uint64_t* offset_bytes
) {
    if (!self || !offset_bytes) {
        return E_POINTER;
    }
    TextureComputer* obj = impl_from(self);
    const HRESULT tiled = reject_if_tiled(obj, kSlotGetTexelElementOffsetBytes, "GetTexelElementOffsetBytes");
    if (tiled != S_OK) {
        return tiled;
    }
    if (z != 0u || array_slice >= obj->resolved.array_size || mip_level >= obj->resolved.mip_levels) {
        return E_INVALIDARG;
    }
    const uint32_t index = mip_level + array_slice * obj->resolved.mip_levels;
    Placement place{};
    const HRESULT hr = placement_at(&obj->resolved, index, &place);
    if (hr != S_OK) {
        return hr;
    }
    if (x >= place.width || y >= place.height) {
        return E_INVALIDARG;
    }
    const uint32_t block_x = x / obj->resolved.block_w;
    const uint32_t block_y = y / obj->resolved.block_h;
    uint64_t row_off = 0;
    uint64_t element = 0;
    uint64_t sum = 0;
    if (!mul_u64(block_y, place.row_pitch, &row_off) || !mul_u64(block_x, obj->resolved.block_bytes, &element) ||
        !add_u64(place.offset, row_off, &sum) || !add_u64(sum, element, &sum)) {
        return E_INVALIDARG;
    }
    *offset_bytes = sum;
    return S_OK;
}

static HRESULT WINAPI computer_copy_into_subresource(
    bx_xg_texture_computer* self,
    void* destination,
    uint64_t destination_size,
    uint32_t subresource,
    const void* source,
    uint64_t source_size,
    uint32_t source_row_pitch,
    uint32_t source_depth_pitch
) {
    if (!self || !destination || !source) {
        return E_POINTER;
    }
    TextureComputer* obj = impl_from(self);
    const HRESULT tiled = reject_if_tiled(obj, kSlotCopyIntoSubresource, "CopyIntoSubresource");
    if (tiled != S_OK) {
        return tiled;
    }
    Placement place{};
    const HRESULT placed = placement_at(&obj->resolved, subresource, &place);
    if (placed != S_OK) {
        return placed;
    }
    uint64_t image_row = 0;
    uint64_t image_depth = 0;
    uint64_t resource_depth = 0;
    const HRESULT pitch = image_and_resource_pitch(
        &place, source_row_pitch, source_depth_pitch, &image_row, &image_depth, &resource_depth);
    if (pitch != S_OK) {
        return pitch;
    }
    return copy_rows(
        &place,
        static_cast<uint8_t*>(destination),
        destination_size,
        place.offset,
        place.row_pitch,
        resource_depth,
        static_cast<const uint8_t*>(source),
        source_size,
        0,
        image_row,
        image_depth);
}

static HRESULT WINAPI computer_copy_from_subresource(
    bx_xg_texture_computer* self,
    void* destination,
    uint64_t destination_size,
    uint32_t destination_row_pitch,
    uint32_t destination_depth_pitch,
    const void* source,
    uint64_t source_size,
    uint32_t subresource
) {
    if (!self || !destination || !source) {
        return E_POINTER;
    }
    TextureComputer* obj = impl_from(self);
    const HRESULT tiled = reject_if_tiled(obj, kSlotCopyFromSubresource, "CopyFromSubresource");
    if (tiled != S_OK) {
        return tiled;
    }
    Placement place{};
    const HRESULT placed = placement_at(&obj->resolved, subresource, &place);
    if (placed != S_OK) {
        return placed;
    }
    uint64_t image_row = 0;
    uint64_t image_depth = 0;
    uint64_t resource_depth = 0;
    const HRESULT pitch = image_and_resource_pitch(
        &place, destination_row_pitch, destination_depth_pitch, &image_row, &image_depth, &resource_depth);
    if (pitch != S_OK) {
        return pitch;
    }
    return copy_rows(
        &place,
        static_cast<uint8_t*>(destination),
        destination_size,
        0,
        image_row,
        image_depth,
        static_cast<const uint8_t*>(source),
        source_size,
        place.offset,
        place.row_pitch,
        resource_depth);
}

static const bx_xg_texture_computer_vtbl kVtbl = {
    computer_query_interface,
    computer_add_ref,
    computer_release,
    computer_get_resource_layout,
    computer_get_resource_size_bytes,
    computer_get_resource_base_alignment_bytes,
    computer_get_mip_level_offset_bytes,
    computer_get_texel_element_offset_bytes,
    computer_copy_into_subresource,
    computer_copy_from_subresource,
};

extern "C" {

HRESULT WINAPI XGCreateTextureComputer(const bx_xg_resource_desc* desc, void** computer) {
    if (!computer) {
        return E_POINTER;
    }
    *computer = nullptr;
    if (!desc) {
        return E_POINTER;
    }
    Resolved resolved{};
    const HRESULT hr = validate_desc(desc, &resolved);
    if (hr != S_OK) {
        return hr;
    }
    auto* obj = static_cast<TextureComputer*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TextureComputer)));
    if (!obj) {
        return E_OUTOFMEMORY;
    }
    obj->iface.lpVtbl = &kVtbl;
    obj->refcount = 1;
    obj->resolved = resolved;
    *computer = obj;
    return S_OK;
}

uint32_t WINAPI XGSuggestSwizzleMode(
    uint32_t resource_type,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    uint32_t mip_levels,
    uint32_t sample_count,
    uint32_t flags
) {
    bx_xg_resource_desc desc{};
    desc.dimension = resource_type;
    desc.alignment = 0u;
    desc.width = width;
    desc.height = height;
    desc.depth_or_array_size = depth > 0xFFFFu ? static_cast<uint16_t>(0) : static_cast<uint16_t>(depth);
    desc.mip_levels = mip_levels > 0xFFFFu ? static_cast<uint16_t>(0xFFFF) : static_cast<uint16_t>(mip_levels);
    desc.format = format;
    desc.sample_desc.count = sample_count;
    desc.sample_desc.quality = 0u;
    desc.layout = BX_XG_LAYOUT_LINEAR;
    desc.flags = flags;
    Resolved resolved{};
    if (validate_desc(&desc, &resolved) != S_OK || resolved.layout != BX_XG_LAYOUT_LINEAR) {
        return BX_XG_LAYOUT_NONE;
    }
    return BX_XG_LAYOUT_LINEAR;
}

} // extern "C"
