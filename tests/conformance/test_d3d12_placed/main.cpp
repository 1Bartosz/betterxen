/*
 * M2-C1 (#464): D3D12ConfigureVirtualMemoryX and placed resources.
 *
 * The configuration block is BetterXen-defined and must match shims/d3d12_xs/vmem.h.
 * cpu_pointer is an output of the config block: the persistent map of the one
 * WRITE_BACK / L0 heap that backs the region. The device and that heap are created
 * once and kept for the whole run. Acceptance writes this file's pattern through
 * that pointer, with no Unmap, and checks a fence-separated GPU copy in both directions.
 */
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bx_log.h"

extern "C" {
__declspec(dllimport) HRESULT WINAPI D3D12ConfigureVirtualMemoryX(void* config);
__declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(void* params, REFIID riid, void** ppv);
__declspec(dllimport) BOOL WINAPI XMemAllocatePhysicalPages(SIZE_T* count, PULONG_PTR pages, uint64_t flags);
__declspec(dllimport) BOOL WINAPI XMemFreePhysicalPages(SIZE_T* count, PULONG_PTR pages, uint64_t flags);
__declspec(dllimport) PVOID WINAPI XMemMapPhysicalPages(PVOID base, SIZE_T count, uint64_t flags, PULONG_PTR pages);
}

struct BxD3d12VirtualMemoryConfig {
    uint32_t version;
    uint32_t flags;
    uint64_t base_va;
    uint64_t size_bytes;
    uint64_t reserved;
    uint64_t cpu_pointer;
};
static_assert(sizeof(BxD3d12VirtualMemoryConfig) == 40, "config block is 40 bytes");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, base_va) == 8, "base_va offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, size_bytes) == 16, "size_bytes offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, reserved) == 24, "reserved offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, cpu_pointer) == 32, "cpu_pointer offset");

namespace {

constexpr uint32_t kVersion = 1;
constexpr uint64_t kAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
constexpr uint32_t kBufBytes = 64;
constexpr uint32_t kTexW = 4;
constexpr uint32_t kTexH = 4;
constexpr uint32_t kChannels = 4;
constexpr uint32_t kRowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
constexpr uint32_t kPlacement = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
constexpr uint32_t kFootprint = kTexH * kRowPitch;
constexpr uint32_t kUploadBytes = kPlacement + kFootprint;
constexpr uint64_t kRegionBytes = kAlign * 2;

static_assert(kAlign == 65536, "placement alignment is 64 KiB");
static_assert(kRowPitch == 256, "texture row pitch is 256 bytes");
static_assert(kPlacement == 512, "texture placement alignment is 512 bytes");
static_assert(kBufBytes <= kRowPitch, "buffer pattern fits in the first texture row");
static_assert(kFootprint <= kAlign, "texture footprint fits in one 64 KiB page");

int g_checks = 0;
int g_failures = 0;

__attribute__((format(gnu_printf, 2, 3)))
bool check(bool ok, const char* fmt, ...) {
    g_checks++;
    if (!ok) {
        g_failures++;
        char msg[384];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::fflush(stderr);
    }
    return ok;
}

unsigned long ul(HRESULT hr) { return static_cast<unsigned long>(hr); }

bool file_contains(const char* path, const char* needle) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64ll << 20)) {
        auto* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(size.QuadPart) + 1));
        DWORD got = 0;
        if (buf && ReadFile(f, buf, static_cast<DWORD>(size.QuadPart), &got, nullptr)) {
            buf[got] = '\0';
            found = std::strstr(buf, needle) != nullptr;
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(f);
    return found;
}

uint8_t texel_byte(uint32_t x, uint32_t y, uint32_t channel) {
    return static_cast<uint8_t>(0x3Cu ^ ((y * kTexW + x) * kChannels + channel + 1u));
}

void write_texture(uint8_t* dst) {
    std::memset(dst, 0, kFootprint);
    for (uint32_t y = 0; y < kTexH; ++y) {
        for (uint32_t x = 0; x < kTexW; ++x) {
            for (uint32_t c = 0; c < kChannels; ++c) {
                dst[y * kRowPitch + x * kChannels + c] = texel_byte(x, y, c);
            }
        }
    }
}

uint8_t reverse_byte(uint32_t i) { return static_cast<uint8_t>(0xA5u ^ (i * 3u + 1u)); }

void write_reverse_pattern(uint8_t* dst) {
    for (uint32_t i = 0; i < kBufBytes; ++i) {
        dst[i] = reverse_byte(i);
    }
}

uint32_t count_reverse(const uint8_t* got) {
    uint32_t match = 0;
    for (uint32_t i = 0; i < kBufBytes; ++i) {
        if (got[i] == reverse_byte(i)) {
            ++match;
        }
    }
    return match;
}

void write_buffer_pattern(uint8_t* dst) {
    std::memset(dst, 0, kBufBytes);
    for (uint32_t i = 0; i < kBufBytes; ++i) {
        const uint32_t x = i / kChannels;
        const uint32_t c = i % kChannels;
        if (x < kTexW) {
            dst[i] = texel_byte(x, 0, c);
        }
    }
}

uint32_t count_buffer(const uint8_t* got) {
    uint8_t want[kBufBytes];
    write_buffer_pattern(want);
    uint32_t match = 0;
    for (uint32_t i = 0; i < kBufBytes; ++i) {
        if (got[i] == want[i]) {
            ++match;
        }
    }
    return match;
}

uint32_t count_texture(const uint8_t* got) {
    uint8_t want[kFootprint];
    write_texture(want);
    uint32_t match = 0;
    for (uint32_t i = 0; i < kFootprint; ++i) {
        if (got[i] == want[i]) {
            ++match;
        }
    }
    return match;
}

D3D12_HEAP_DESC heap_desc(uint64_t bytes, D3D12_HEAP_FLAGS flags) {
    D3D12_HEAP_DESC desc = {};
    desc.SizeInBytes = bytes;
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Alignment = kAlign;
    desc.Flags = flags;
    return desc;
}

D3D12_RESOURCE_DESC buffer_desc(uint64_t bytes) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

D3D12_RESOURCE_DESC texture_desc() {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kTexW;
    desc.Height = kTexH;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return desc;
}

bool wait_fence(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    HRESULT hr = queue->Signal(fence, value);
    if (hr != S_OK) {
        return check(false, "queue Signal hr=0x%08lx", ul(hr));
    }
    if (fence->GetCompletedValue() >= value) {
        return true;
    }
    HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    hr = fence->SetEventOnCompletion(value, done);
    if (hr != S_OK || !done) {
        if (done) CloseHandle(done);
        return check(false, "fence SetEventOnCompletion hr=0x%08lx", ul(hr));
    }
    DWORD waited = WaitForSingleObject(done, 10000);
    CloseHandle(done);
    return check(waited == WAIT_OBJECT_0 && fence->GetCompletedValue() >= value,
                 "fence must reach %llu (wait=%lu completed=%llu)", static_cast<unsigned long long>(value), waited,
                 static_cast<unsigned long long>(fence->GetCompletedValue()));
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    const bx_runtime_config_t* cfg = bx_get_runtime_config();
    if (!check(cfg && cfg->title_memory_limit_bytes >= kAlign, "runtime config must publish a title memory limit")) {
        std::fprintf(stderr, "FAIL: test_d3d12_placed (no runtime config)\n");
        return 1;
    }
    const char* profile = cfg->profile == BX_PROFILE_ANACONDA ? "anaconda" : "lockhart";

    check(D3D12ConfigureVirtualMemoryX(nullptr) == S_OK, "null configuration is a logged stub that returns S_OK");
    uint8_t foreign[sizeof(BxD3d12VirtualMemoryConfig)];
    std::memset(foreign, 0, sizeof(foreign));
    const uint32_t foreign_version = 0x12345678u;
    std::memcpy(foreign, &foreign_version, sizeof(foreign_version));
    std::memset(foreign + 32, 0xA5, 8);
    uint8_t foreign_before[sizeof(foreign)];
    std::memcpy(foreign_before, foreign, sizeof(foreign));
    check(D3D12ConfigureVirtualMemoryX(foreign) == S_OK, "an unrecognised block returns S_OK");
    check(std::memcmp(foreign, foreign_before, sizeof(foreign)) == 0,
          "an unrecognised block, including the sentinel at bytes 32..39, is left unchanged");
    BxD3d12VirtualMemoryConfig bad = {2u, 0u, 0u, 0u, 0u, 0u};
    const BxD3d12VirtualMemoryConfig bad_before = bad;
    check(D3D12ConfigureVirtualMemoryX(&bad) == S_OK, "version 2 is an unrecognised block and returns S_OK");
    check(std::memcmp(&bad, &bad_before, sizeof(bad)) == 0, "version 2 block bytes stay unchanged");
    bad = {kVersion, 1u, 0u, 0u, 0u, 0u};
    const BxD3d12VirtualMemoryConfig flags_before = bad;
    check(D3D12ConfigureVirtualMemoryX(&bad) == S_OK, "non-zero flags are an unrecognised block and return S_OK");
    check(std::memcmp(&bad, &flags_before, sizeof(bad)) == 0, "non-zero flags block bytes stay unchanged");
    bad = {kVersion, 0u, 0u, 1000u, 0u, 0u};
    check(D3D12ConfigureVirtualMemoryX(&bad) == E_INVALIDARG, "size that is not a multiple of 64 KiB must be rejected");
    bad = {kVersion, 0u, 0u, cfg->title_memory_limit_bytes + kAlign, 0u, 0u};
    check(D3D12ConfigureVirtualMemoryX(&bad) == E_INVALIDARG, "size above the title limit must be rejected");
    bad = {kVersion, 0u, 0u, 0u, 1u, 0u};
    const BxD3d12VirtualMemoryConfig reserved_before = bad;
    check(D3D12ConfigureVirtualMemoryX(&bad) == S_OK, "a non-zero reserved word is an unrecognised block and returns S_OK");
    check(std::memcmp(&bad, &reserved_before, sizeof(bad)) == 0, "non-zero reserved block bytes stay unchanged");
    bad = {kVersion, 0u, 0x1000u, kAlign, 0u, 0u};
    check(D3D12ConfigureVirtualMemoryX(&bad) == E_INVALIDARG, "a VA that is not an xmem region must be rejected");

    SIZE_T page_count = 2;
    ULONG_PTR pages[2] = {};
    uint8_t* view = nullptr;
    bool mapped = XMemAllocatePhysicalPages(&page_count, pages, 0) && page_count == 2 && pages[0] != 0 && pages[1] != 0;
    if (mapped) {
        view = static_cast<uint8_t*>(XMemMapPhysicalPages(nullptr, page_count, 0, pages));
        mapped = view != nullptr && (reinterpret_cast<uintptr_t>(view) % kAlign) == 0;
        if (mapped) {
            view[kAlign] = 0x5A;
            mapped = view[kAlign] == 0x5A;
        }
    }
    check(mapped, "xmem must map two contiguous 64 KiB pages");

    BxD3d12VirtualMemoryConfig config = {};
    config.version = kVersion;
    config.base_va = reinterpret_cast<uint64_t>(view);
    config.size_bytes = kRegionBytes;
    const BxD3d12VirtualMemoryConfig sent = config;
    HRESULT hr = mapped ? D3D12ConfigureVirtualMemoryX(&config) : E_FAIL;
    check(hr == S_OK, "D3D12ConfigureVirtualMemoryX(valid) hr=0x%08lx", ul(hr));
    check(config.version == sent.version && config.flags == sent.flags && config.base_va == sent.base_va &&
              config.size_bytes == sent.size_bytes && config.reserved == sent.reserved,
          "configuration input fields must be left unchanged");
    check(config.cpu_pointer == 0, "cpu_pointer stays 0 until the backing heap exists");

    ID3D12Device* device = nullptr;
    hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    if (!check(hr == S_OK && device, "D3D12XboxCreateDevice hr=0x%08lx", ul(hr))) {
        std::fprintf(stderr, "FAIL: test_d3d12_placed (no device; %d of %d checks failed)\n", g_failures, g_checks);
        return 1;
    }

    ID3D12Heap* rejected = nullptr;
    D3D12_HEAP_DESC bad_align = heap_desc(kAlign, D3D12_HEAP_FLAG_NONE);
    bad_align.Alignment = 4096;
    hr = device->CreateHeap(&bad_align, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&rejected));
    check(hr == E_INVALIDARG && rejected == nullptr, "heap alignment 4096 must be rejected without a heap hr=0x%08lx",
          ul(hr));

    ID3D12Heap* heap = nullptr;
    D3D12_HEAP_DESC region_desc = heap_desc(kRegionBytes, D3D12_HEAP_FLAG_NONE);
    hr = device->CreateHeap(&region_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap));
    check(hr == S_OK && heap, "configured-region heap hr=0x%08lx", ul(hr));
    if (heap) {
        D3D12_HEAP_DESC got = heap->GetDesc();
        check(got.Properties.Type == D3D12_HEAP_TYPE_CUSTOM, "backing heap type is CUSTOM");
        check(got.Properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, "backing heap is WRITE_BACK");
        check(got.Properties.MemoryPoolPreference == D3D12_MEMORY_POOL_L0, "backing heap is L0");
        check(got.SizeInBytes == kRegionBytes && (got.Alignment == 0 || got.Alignment == kAlign),
              "backing heap is the 128 KiB region on a 64 KiB alignment");
    }
    check(config.cpu_pointer != 0, "cpu_pointer is the persistent map of the backing heap");

    ID3D12Resource* placed_buf = nullptr;
    ID3D12Resource* placed_tex = nullptr;
    if (heap) {
        D3D12_RESOURCE_DESC tiny = buffer_desc(kBufBytes);
        ID3D12Resource* misaligned = nullptr;
        hr = device->CreatePlacedResource(heap, 4096, &tiny, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          __uuidof(ID3D12Resource), reinterpret_cast<void**>(&misaligned));
        check(hr == E_INVALIDARG && misaligned == nullptr, "placed offset 4096 must be rejected hr=0x%08lx", ul(hr));
        if (misaligned) misaligned->Release();

        D3D12_RESOURCE_DESC buf_desc = buffer_desc(kUploadBytes);
        hr = device->CreatePlacedResource(heap, 0, &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          __uuidof(ID3D12Resource), reinterpret_cast<void**>(&placed_buf));
        check(hr == S_OK && placed_buf, "placed buffer at offset 0 hr=0x%08lx", ul(hr));

        D3D12_RESOURCE_DESC tex_desc = texture_desc();
        hr = device->CreatePlacedResource(heap, kAlign, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          __uuidof(ID3D12Resource), reinterpret_cast<void**>(&placed_tex));
        check(hr == S_OK && placed_tex, "placed RGBA8 texture at offset 64 KiB hr=0x%08lx", ul(hr));
    }

    ID3D12Resource* readback = nullptr;
    D3D12_HEAP_PROPERTIES readback_props = {};
    readback_props.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC staging_desc = buffer_desc(kUploadBytes);
    if (placed_buf && placed_tex) {
        hr = device->CreateCommittedResource(&readback_props, D3D12_HEAP_FLAG_NONE, &staging_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
                                             reinterpret_cast<void**>(&readback));
        check(hr == S_OK && readback, "readback buffer hr=0x%08lx", ul(hr));
    }

    uint32_t buf_match = 0;
    uint32_t tex_match = 0;
    uint32_t reverse_match = 0;
    UINT64 buf_gpu = 0;
    int flush_ok = 0;
    const char* texture_path = "not_run";
    HRESULT texture_hr = E_FAIL;
    const UINT64 cpu_va = reinterpret_cast<UINT64>(view);
    uint8_t* cpu = config.cpu_pointer != 0 ? reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(config.cpu_pointer))
                                          : nullptr;
    if (readback && placed_buf && placed_tex && cpu) {
        std::memset(cpu, 0, kUploadBytes);
        write_buffer_pattern(cpu);
        write_texture(cpu + kPlacement);
        uint8_t tex_src[kFootprint];
        write_texture(tex_src);
        texture_hr = placed_tex->WriteToSubresource(0, nullptr, tex_src, kRowPitch, kFootprint);
        texture_path = texture_hr == S_OK ? "WriteToSubresource" : "copy_from_buffer";
        std::printf("texture_path=%s hr=0x%08lx\n", texture_path, ul(texture_hr));
        std::fflush(stdout);

        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12Fence* fence = nullptr;
        hr = device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
        check(hr == S_OK && queue, "CreateCommandQueue hr=0x%08lx", ul(hr));
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                           reinterpret_cast<void**>(&allocator));
        check(hr == S_OK && allocator, "CreateCommandAllocator hr=0x%08lx", ul(hr));
        if (allocator) {
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                           __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list));
            check(hr == S_OK && list, "CreateCommandList hr=0x%08lx", ul(hr));
        }
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
        check(hr == S_OK && fence, "CreateFence hr=0x%08lx", ul(hr));

        ID3D12Resource* upload = nullptr;
        if (list && queue && fence) {
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = placed_buf;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = kPlacement;
            src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            src.PlacedFootprint.Footprint.Width = kTexW;
            src.PlacedFootprint.Footprint.Height = kTexH;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = kRowPitch;
            if (texture_hr != S_OK) {
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = placed_tex;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            list->CopyBufferRegion(readback, 0, placed_buf, 0, kBufBytes);

            D3D12_RESOURCE_BARRIER to_src = {};
            to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_src.Transition.pResource = placed_tex;
            to_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_src.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            to_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &to_src);

            D3D12_TEXTURE_COPY_LOCATION back_src = {};
            back_src.pResource = placed_tex;
            back_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION back_dst = {};
            back_dst.pResource = readback;
            back_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            back_dst.PlacedFootprint = src.PlacedFootprint;
            list->CopyTextureRegion(&back_dst, 0, 0, 0, &back_src, nullptr);

            hr = list->Close();
            check(hr == S_OK, "command list Close hr=0x%08lx", ul(hr));
            ID3D12CommandList* lists[] = {list};
            queue->ExecuteCommandLists(1, lists);
            check(wait_fence(queue, fence, 1), "GPU queue must retire the placed-resource copy");

            D3D12_HEAP_PROPERTIES upload_props = {};
            upload_props.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC upload_desc = buffer_desc(kBufBytes);
            hr = device->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
                                                 reinterpret_cast<void**>(&upload));
            check(hr == S_OK && upload, "reverse upload buffer hr=0x%08lx", ul(hr));
            void* upload_cpu = nullptr;
            if (upload && upload->Map(0, nullptr, &upload_cpu) == S_OK && upload_cpu) {
                write_reverse_pattern(static_cast<uint8_t*>(upload_cpu));
                upload->Unmap(0, nullptr);
            }
            if (upload && allocator->Reset() == S_OK && list->Reset(allocator, nullptr) == S_OK) {
                D3D12_RESOURCE_BARRIER to_dest = {};
                to_dest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                to_dest.Transition.pResource = placed_buf;
                to_dest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                to_dest.Transition.StateBefore = D3D12_RESOURCE_STATE_GENERIC_READ;
                to_dest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                list->ResourceBarrier(1, &to_dest);
                list->CopyBufferRegion(placed_buf, 0, upload, 0, kBufBytes);
                hr = list->Close();
                check(hr == S_OK, "reverse command list Close hr=0x%08lx", ul(hr));
                ID3D12CommandList* reverse_lists[] = {list};
                queue->ExecuteCommandLists(1, reverse_lists);
                check(wait_fence(queue, fence, 2), "GPU queue must retire the reverse copy");
                reverse_match = count_reverse(cpu);
            }
        }
        if (upload) upload->Release();
        if (queue) queue->Release();
        if (allocator) allocator->Release();
        if (list) list->Release();
        if (fence) fence->Release();

        void* back = nullptr;
        D3D12_RANGE read_range = {0, kUploadBytes};
        hr = readback->Map(0, &read_range, &back);
        check(hr == S_OK && back, "Map readback hr=0x%08lx", ul(hr));
        if (back) {
            auto* bytes = static_cast<const uint8_t*>(back);
            buf_match = count_buffer(bytes);
            tex_match = count_texture(bytes + kPlacement);
            D3D12_RANGE empty = {0, 0};
            readback->Unmap(0, &empty);
        }
        check(buf_match == kBufBytes, "readback buffer bytes %u/%u", buf_match, kBufBytes);
        check(tex_match == kFootprint, "readback texture bytes %u/%u", tex_match, kFootprint);
        check(reverse_match == kBufBytes, "CPU readback through the persistent pointer %u/%u", reverse_match, kBufBytes);

        buf_gpu = placed_buf->GetGPUVirtualAddress();
        if (view) {
            const BOOL first = FlushViewOfFile(view, static_cast<SIZE_T>(kAlign));
            const BOOL second = FlushViewOfFile(view + kAlign, static_cast<SIZE_T>(kAlign));
            flush_ok = first && second ? 1 : 0;
            check(flush_ok == 1, "FlushViewOfFile on each xmem page must succeed (err=%lu)",
                  static_cast<unsigned long>(GetLastError()));
        }
    }

    char log_path[MAX_PATH];
    DWORD log_len = GetEnvironmentVariableA("BX_LOG_FILE", log_path, sizeof(log_path));
    const bool have_log = log_len > 0 && log_len < sizeof(log_path);
    std::printf("RESULT: cpu_va=0x%llx buffer_gpu_va=0x%llx flush=%d copy_buffer=%u/%u copy_texture=%u/%u "
                "texture_path=%s cpu_after_gpu=%u/%u\n",
                static_cast<unsigned long long>(cpu_va), static_cast<unsigned long long>(buf_gpu), flush_ok, buf_match,
                kBufBytes, tex_match, kFootprint, texture_path, reverse_match, kBufBytes);
    std::fflush(stdout);

    if (have_log) {
        char base_needle[64];
        char limit_needle[80];
        std::snprintf(base_needle, sizeof(base_needle), "base=0x%llx", static_cast<unsigned long long>(cpu_va));
        std::snprintf(limit_needle, sizeof(limit_needle), "title_limit=%llu",
                      static_cast<unsigned long long>(cfg->title_memory_limit_bytes));
        check(file_contains(log_path, "\"event\":\"D3D12ConfigureVirtualMemoryX\""),
              "log must record D3D12ConfigureVirtualMemoryX");
        check(file_contains(log_path, profile), "log must name the %s profile", profile);
        check(file_contains(log_path, limit_needle), "log must record the active title limit %s", limit_needle);
        check(mapped && file_contains(log_path, base_needle), "log must record the queried xmem base %s", base_needle);
        check(file_contains(log_path, "d3d12_xs.dll!D3D12ConfigureVirtualMemoryX"),
              "an unrecognised configuration must log the unimplemented-API stub");
    } else {
        check(false, "BX_LOG_FILE must be set");
    }

    if (readback) readback->Release();
    if (placed_tex) placed_tex->Release();
    if (placed_buf) placed_buf->Release();
    if (heap) heap->Release();
    check(device->Release() == 0, "device proxy leaked references");

    if (view) {
        XMemMapPhysicalPages(view, page_count, 0, nullptr);
    }
    if (pages[0] || pages[1]) {
        SIZE_T free_count = 2;
        XMemFreePhysicalPages(&free_count, pages, 0);
    }

    if (g_failures > 0) {
        std::fprintf(stderr, "FAIL: test_d3d12_placed (%d of %d checks failed)\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: test_d3d12_placed (%d checks passed, buffer %u/%u bytes, texture %u/%u bytes, "
                "texture_path=%s, cpu_after_gpu %u/%u)\n",
                g_checks, buf_match, kBufBytes, tex_match, kFootprint, texture_path, reverse_match, kBufBytes);
    return 0;
}
