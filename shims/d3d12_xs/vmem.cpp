#include "proxy.h"
#include "vmem.h"

#include "bx_xmem_internal.h"

#include <cstdio>
#include <cstring>
#include <new>

/*
 * The configured region is backed by one CUSTOM WRITE_BACK / L0 heap and a buffer
 * that stays mapped. CreateHeap of that size returns that heap. atesahmet0 and
 * Arths17 measured this for #473: WRITE_BACK or WRITE_COMBINE on L0 is shared
 * memory, and both OpenExistingHeap methods return E_NOTIMPL. This file does not
 * call them. OpenExistingHeapFromFileMapping also needs a section handle that
 * bx_xmem_region_info does not carry. This file does not request L1 and does not
 * treat an xmem address as a GPU pointer.
 * Any other size is a normal host heap. Heap alignment values 0, 64 KiB and 4 MiB:
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_heap_desc
 * Placed offsets are a multiple of the resource alignment and stay inside the heap,
 * tightened here to 64 KiB:
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createplacedresource
 * Non-custom heap properties use UNKNOWN CPU page and memory pool:
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_heap_properties
 */

namespace {

constexpr uint64_t k64K = 64ull * 1024ull;
constexpr uint64_t k4M = 4ull * 1024ull * 1024ull;
constexpr uint32_t kConfigVersion = 1;

constexpr uint32_t kForwardedFlags =
    static_cast<uint32_t>(D3D12_HEAP_FLAG_DENY_BUFFERS) |
    static_cast<uint32_t>(D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES) |
    static_cast<uint32_t>(D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES) |
    static_cast<uint32_t>(D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS) |
    static_cast<uint32_t>(D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT) |
    static_cast<uint32_t>(D3D12_HEAP_FLAG_CREATE_NOT_ZEROED);

struct Record {
    bool configured = false;
    bx_hardware_profile_t profile = BX_PROFILE_LOCKHART;
    uint64_t title_limit = 0;
    bool have_region = false;
    bx_xmem_region_info_t region{};
    /* Bytes of host heaps still alive. D3DMetal keeps freed memory in its own
       pool, so this counts live heaps only: charge on create, credit on the
       final Release. It does not assume the OS reclaimed the pages. */
    uint64_t charged_private_bytes = 0;
    uint64_t cpu_pointer = 0;
    BxD3d12VirtualMemoryConfig* config_block = nullptr;
    class BxChargedHeap* backing = nullptr;
    ID3D12Resource* backing_buffer = nullptr;
};

struct Section {
    CRITICAL_SECTION cs;
    Section() { InitializeCriticalSection(&cs); }
    ~Section() { DeleteCriticalSection(&cs); }
};

Section g_section;
Record g_record;

struct Guard {
    explicit Guard(Section& section) : section(section) { EnterCriticalSection(&section.cs); }
    ~Guard() { LeaveCriticalSection(&section.cs); }
    Section& section;
};

uint64_t title_limit() {
    const bx_runtime_config_t* cfg = bx_get_runtime_config();
    if (cfg && cfg->title_memory_limit_bytes) {
        return cfg->title_memory_limit_bytes;
    }
    /* Same 8 GiB Lockhart default bx_runtime_veh stores when no profile is set. */
    return 8ull << 30;
}

uint64_t active_cap(const bx_xmem_stats_t& stats) {
    uint64_t cap = title_limit();
    if (stats.title_limit_bytes != 0 && stats.title_limit_bytes < cap) {
        cap = stats.title_limit_bytes;
    }
    return cap;
}

const char* profile_name(bx_hardware_profile_t profile) {
    return profile == BX_PROFILE_ANACONDA ? "anaconda" : "lockhart";
}

bool node_masks_ok(UINT creation, UINT visible) {
    const UINT creation_bit = creation == 0 ? 1u : creation;
    const UINT visible_bits = visible == 0 ? creation_bit : visible;
    if (creation != 0 && (creation & (creation - 1u)) != 0) {
        return false;
    }
    return (visible_bits & creation_bit) == creation_bit;
}

HRESULT check_heap_shape(const D3D12_HEAP_DESC* desc) {
    if (desc->Properties.MemoryPoolPreference == D3D12_MEMORY_POOL_L1) {
        return E_INVALIDARG;
    }
    const uint64_t align = desc->Alignment == 0 ? k64K : desc->Alignment;
    if (align != k64K && align != k4M) {
        return E_INVALIDARG;
    }
    if (desc->SizeInBytes == 0 || desc->SizeInBytes % align != 0 || desc->SizeInBytes % k64K != 0) {
        return E_INVALIDARG;
    }
    switch (desc->Properties.Type) {
    case D3D12_HEAP_TYPE_DEFAULT:
    case D3D12_HEAP_TYPE_UPLOAD:
    case D3D12_HEAP_TYPE_READBACK:
        if (desc->Properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN ||
            desc->Properties.MemoryPoolPreference != D3D12_MEMORY_POOL_UNKNOWN) {
            return E_INVALIDARG;
        }
        break;
    case D3D12_HEAP_TYPE_CUSTOM:
        if (desc->Properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_UNKNOWN ||
            desc->Properties.MemoryPoolPreference == D3D12_MEMORY_POOL_UNKNOWN) {
            return E_INVALIDARG;
        }
        break;
    default:
        return E_INVALIDARG;
    }
    if (!node_masks_ok(desc->Properties.CreationNodeMask, desc->Properties.VisibleNodeMask)) {
        return E_INVALIDARG;
    }
    return S_OK;
}

bool heap_riid(REFIID riid) {
    return riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
           riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Heap);
}

/* Private host heaps count against the same Lockhart/Anaconda cap as xmem. */
bool try_charge(uint64_t bytes) {
    Guard guard(g_section);
    bx_xmem_stats_t stats{};
    bx_xmem_get_internal_stats(&stats);
    const uint64_t cap = active_cap(stats);
    const uint64_t xmem_used = stats.committed_virtual_bytes + stats.allocated_physical_bytes;
    if (bytes > cap || xmem_used > cap || g_record.charged_private_bytes > cap - xmem_used) {
        return false;
    }
    const uint64_t room = cap - xmem_used - g_record.charged_private_bytes;
    if (bytes > room) {
        return false;
    }
    g_record.charged_private_bytes += bytes;
    return true;
}

void release_charge(uint64_t bytes) {
    Guard guard(g_section);
    if (bytes > g_record.charged_private_bytes) {
        bx_log_emit(BX_LOG_ERROR, "d3d12_xs", "heap_budget", "GPU heap charge underflow on release");
        g_record.charged_private_bytes = 0;
        return;
    }
    g_record.charged_private_bytes -= bytes;
}

/*
 * Holds the budget charge for one private host heap and drops it on the last Release.
 * QueryInterface for ID3D12Heap and its base interfaces returns this object, so
 * bx_unwrap sees IID_BxProxyHost and CreatePlacedResource receives the host heap.
 * A CreateHeap riid outside that set cannot be observed here: the host pointer is
 * returned and the charge stays until process exit. Callers in this tree request
 * ID3D12Heap.
 */
class BxChargedHeap final : public ID3D12Heap {
public:
    static BxChargedHeap* create(ID3D12Heap* host, uint64_t bytes) {
        return new (std::nothrow) BxChargedHeap(host, bytes);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (heap_riid(riid)) {
            *ppv = static_cast<ID3D12Heap*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_BxProxyHost) {
            *ppv = host_;
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = InterlockedDecrement(&refs_);
        if (left == 0) {
            /* Final Release of a live heap. D3DMetal may pool the pages after this,
               so the budget credit is the live-heap count, not a physical-memory drop. */
            release_charge(bytes_);
            if (host_) {
                host_->Release();
            }
            delete this;
        }
        return left;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        return host_->GetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        return host_->SetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override {
        return host_->SetPrivateDataInterface(guid, data);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return host_->SetName(name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppv) override { return host_->GetDevice(riid, ppv); }
    D3D12_HEAP_DESC* STDMETHODCALLTYPE GetDesc(D3D12_HEAP_DESC* ret) override { return host_->GetDesc(ret); }

private:
    BxChargedHeap(ID3D12Heap* host, uint64_t bytes) : host_(host), bytes_(bytes) {}
    ~BxChargedHeap() = default;

    ID3D12Heap* host_;
    uint64_t bytes_;
    LONG refs_ = 1;
};

bool region_matches(const D3D12_HEAP_DESC* desc, const Record& snap, bx_xmem_region_info_t* live) {
    if (!snap.configured || !snap.have_region || desc->SizeInBytes != snap.region.size_bytes) {
        return false;
    }
    if (!bx_xmem_query_region(snap.region.base_va, live) || live->base_va != snap.region.base_va ||
        live->size_bytes == 0 || snap.region.size_bytes % live->size_bytes != 0) {
        return false;
    }
    const uint64_t page = live->size_bytes;
    for (uint64_t offset = 0; offset < snap.region.size_bytes; offset += page) {
        bx_xmem_region_info_t part{};
        if (!bx_xmem_query_region(snap.region.base_va + offset, &part) ||
            part.base_va != snap.region.base_va + offset || part.size_bytes != page) {
            return false;
        }
    }
    live->size_bytes = snap.region.size_bytes;
    return true;
}

void publish_cpu_pointer(uint64_t pointer) {
    g_record.cpu_pointer = pointer;
    if (g_record.config_block) {
        g_record.config_block->cpu_pointer = pointer;
    }
}

HRESULT host_private_heap(BxDeviceProxy* self, const D3D12_HEAP_DESC* desc, REFIID riid, void** out);

HRESULT return_backing(REFIID riid, void** out) {
    if (!g_record.backing) {
        return E_FAIL;
    }
    g_record.backing->AddRef();
    const HRESULT hr = g_record.backing->QueryInterface(riid, out);
    g_record.backing->Release();
    return hr;
}

HRESULT ensure_backing(BxDeviceProxy* self, uint64_t bytes, REFIID riid, void** out) {
    {
        Guard guard(g_section);
        if (g_record.backing) {
            return return_backing(riid, out);
        }
    }

    D3D12_HEAP_DESC desc = {};
    desc.SizeInBytes = bytes;
    desc.Alignment = k64K;
    desc.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    desc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    void* created = nullptr;
    HRESULT hr = host_private_heap(self, &desc, __uuidof(ID3D12Heap), &created);
    if (FAILED(hr) || !created) {
        return FAILED(hr) ? hr : E_FAIL;
    }
    auto* wrap = static_cast<BxChargedHeap*>(created);
    ID3D12Heap* host_heap = bx_unwrap(wrap);

    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    hr = self->h_ID3D12Device->CreatePlacedResource(host_heap, 0, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    if (FAILED(hr) || !resource) {
        wrap->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }
    void* mapped = nullptr;
    hr = resource->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) {
        resource->Release();
        wrap->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }

    {
        Guard guard(g_section);
        if (g_record.backing) {
            resource->Unmap(0, nullptr);
            resource->Release();
            wrap->Release();
            return return_backing(riid, out);
        }
        g_record.backing = wrap;
        g_record.backing_buffer = resource;
        publish_cpu_pointer(reinterpret_cast<uint64_t>(mapped));
    }
    return return_backing(riid, out);
}

HRESULT host_private_heap(BxDeviceProxy* self, const D3D12_HEAP_DESC* desc, REFIID riid, void** out) {
    if (!try_charge(desc->SizeInBytes)) {
        bx_log_emit(BX_LOG_WARN, "d3d12_xs", "heap_budget",
                    "CreateHeap exceeds the Lockhart/Anaconda xmem title budget");
        return E_OUTOFMEMORY;
    }
    void* raw = nullptr;
    HRESULT hr = self->h_ID3D12Device->CreateHeap(desc, __uuidof(ID3D12Heap), &raw);
    if (FAILED(hr) || !raw) {
        release_charge(desc->SizeInBytes);
        return FAILED(hr) ? hr : E_FAIL;
    }
    auto* host = static_cast<ID3D12Heap*>(raw);
    if (!heap_riid(riid)) {
        /* Charge cannot follow an interface this wrapper does not implement.
           The first heap is dropped. If the caller's CreateHeap succeeds, the
           charge stays until process exit. See BxChargedHeap. */
        host->Release();
        bx_log_emit(BX_LOG_WARN, "d3d12_xs", "heap_budget",
                    "CreateHeap riid is not ID3D12Heap; the charge stays until process exit");
        hr = self->h_ID3D12Device->CreateHeap(desc, riid, out);
        if (FAILED(hr)) {
            release_charge(desc->SizeInBytes);
        }
        return hr;
    }
    BxChargedHeap* wrap = BxChargedHeap::create(host, desc->SizeInBytes);
    if (!wrap) {
        host->Release();
        release_charge(desc->SizeInBytes);
        return E_OUTOFMEMORY;
    }
    hr = wrap->QueryInterface(riid, out);
    wrap->Release();
    if (FAILED(hr) && out) {
        *out = nullptr;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE create_heap(BxDeviceProxy* self, const D3D12_HEAP_DESC* desc, REFIID riid, void** out) {
    BX_D3D_TRACE("ID3D12Device", "CreateHeap");
    if (!out) {
        return E_POINTER;
    }
    *out = nullptr;
    if (!self || !self->h_ID3D12Device) {
        BX_VTABLE_STUB_LOG("ID3D12Device", 28u, "CreateHeap");
        return E_NOTIMPL;
    }
    if (!desc) {
        return E_INVALIDARG;
    }
    if ((static_cast<uint32_t>(desc->Flags) & ~kForwardedFlags) != 0) {
        BX_VTABLE_STUB_LOG("ID3D12Device", 28u, "CreateHeap");
        return E_NOTIMPL;
    }
    HRESULT hr = check_heap_shape(desc);
    if (FAILED(hr)) {
        return hr;
    }

    Record snap;
    {
        Guard guard(g_section);
        snap = g_record;
    }
    bx_xmem_region_info_t live{};
    if (region_matches(desc, snap, &live)) {
        return ensure_backing(self, live.size_bytes, riid, out);
    }
    return host_private_heap(self, desc, riid, out);
}

HRESULT STDMETHODCALLTYPE create_placed(BxDeviceProxy* self, ID3D12Heap* heap, UINT64 offset,
                                       const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                       const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    BX_D3D_TRACE("ID3D12Device", "CreatePlacedResource");
    if (!out) {
        return E_POINTER;
    }
    *out = nullptr;
    if (!self || !self->h_ID3D12Device) {
        BX_VTABLE_STUB_LOG("ID3D12Device", 29u, "CreatePlacedResource");
        return E_NOTIMPL;
    }
    if (!heap || !desc || (offset % k64K) != 0) {
        return E_INVALIDARG;
    }
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && clear) {
        return E_INVALIDARG;
    }

    D3D12_HEAP_DESC heap_desc = heap->GetDesc();
    if (heap_desc.SizeInBytes % k64K != 0) {
        return E_INVALIDARG;
    }
    if (heap_desc.Properties.Type == D3D12_HEAP_TYPE_UPLOAD && state != D3D12_RESOURCE_STATE_GENERIC_READ) {
        return E_INVALIDARG;
    }
    if (heap_desc.Properties.Type == D3D12_HEAP_TYPE_READBACK && state != D3D12_RESOURCE_STATE_COPY_DEST) {
        return E_INVALIDARG;
    }

    D3D12_RESOURCE_ALLOCATION_INFO info = self->h_ID3D12Device->GetResourceAllocationInfo(0, 1, desc);
    if (info.SizeInBytes == 0 || info.SizeInBytes == ~0ull) {
        return E_INVALIDARG;
    }
    if (info.Alignment != 0 && (offset % info.Alignment) != 0) {
        return E_INVALIDARG;
    }
    if (offset > heap_desc.SizeInBytes || info.SizeInBytes > heap_desc.SizeInBytes - offset) {
        return E_INVALIDARG;
    }

    ID3D12Heap* host_heap = bx_unwrap(heap);
    HRESULT hr = self->h_ID3D12Device->CreatePlacedResource(host_heap, offset, desc, state, clear, riid, out);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "CreatePlacedResource offset=%llu bytes=%llu hr=0x%08lx",
                  static_cast<unsigned long long>(offset), static_cast<unsigned long long>(info.SizeInBytes),
                  static_cast<unsigned long>(hr));
    bx_log_emit(FAILED(hr) ? BX_LOG_WARN : BX_LOG_INFO, "d3d12_xs", "placed_resource", detail);
    return hr;
}

} // namespace

extern "C" HRESULT bx_d3d12_configure_virtual_memory(void* config) {
    if (!config) {
        /* Titles call this before reserving memory and require S_OK. */
        BX_STUB_LOG("d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX");
        return S_OK;
    }
    const auto* in = static_cast<const BxD3d12VirtualMemoryConfig*>(config);
    if (in->version != kConfigVersion || in->flags != 0 || in->reserved != 0) {
        /* A console parameter block is not this struct. Leave those bytes alone. */
        BX_STUB_LOG("d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX");
        return S_OK;
    }

    const bx_runtime_config_t* cfg = bx_get_runtime_config();
    const uint64_t cap = title_limit();
    const auto profile = (cfg && cfg->profile == BX_PROFILE_ANACONDA) ? BX_PROFILE_ANACONDA : BX_PROFILE_LOCKHART;
    if (in->size_bytes != 0 && (in->size_bytes % k64K != 0 || in->size_bytes > cap)) {
        return E_INVALIDARG;
    }

    bx_xmem_stats_t stats{};
    bx_xmem_get_internal_stats(&stats);
    if (in->size_bytes != 0 && stats.title_limit_bytes != 0 && in->size_bytes > stats.title_limit_bytes) {
        return E_INVALIDARG;
    }

    bx_xmem_region_info_t region{};
    bool have_region = false;
    if (in->base_va != 0) {
        bx_xmem_region_info_t first{};
        if (!bx_xmem_query_region(in->base_va, &first) || first.size_bytes == 0 || first.base_va != in->base_va ||
            first.base_va % k64K != 0 || first.size_bytes % k64K != 0) {
            return E_INVALIDARG;
        }
        const uint64_t span = in->size_bytes != 0 ? in->size_bytes : first.size_bytes;
        if (span < first.size_bytes || span % first.size_bytes != 0) {
            return E_INVALIDARG;
        }
        for (uint64_t offset = 0; offset < span; offset += first.size_bytes) {
            bx_xmem_region_info_t page{};
            if (!bx_xmem_query_region(first.base_va + offset, &page) || page.base_va != first.base_va + offset ||
                page.size_bytes != first.size_bytes) {
                return E_INVALIDARG;
            }
        }
        region = first;
        region.size_bytes = span;
        have_region = true;
    }

    auto* block = static_cast<BxD3d12VirtualMemoryConfig*>(config);
    BxChargedHeap* old_heap = nullptr;
    ID3D12Resource* old_buffer = nullptr;
    {
        Guard guard(g_section);
        const bool same = have_region && g_record.have_region && g_record.region.base_va == region.base_va &&
                          g_record.region.size_bytes == region.size_bytes;
        if (!same) {
            old_heap = g_record.backing;
            old_buffer = g_record.backing_buffer;
            g_record.backing = nullptr;
            g_record.backing_buffer = nullptr;
            g_record.cpu_pointer = 0;
        }
        g_record.configured = true;
        g_record.profile = profile;
        g_record.title_limit = cap;
        g_record.have_region = have_region;
        g_record.region = have_region ? region : bx_xmem_region_info_t{};
        g_record.config_block = block;
        block->cpu_pointer = g_record.cpu_pointer;
    }
    if (old_buffer) {
        old_buffer->Unmap(0, nullptr);
        old_buffer->Release();
    }
    if (old_heap) {
        old_heap->Release();
    }

    char detail[320];
    std::snprintf(detail, sizeof(detail),
                  "profile=%s title_limit=%llu xmem_used=%llu xmem_region=%d base=0x%llx region_bytes=%llu",
                  profile_name(profile), static_cast<unsigned long long>(cap),
                  static_cast<unsigned long long>(stats.committed_virtual_bytes + stats.allocated_physical_bytes),
                  have_region ? 1 : 0, static_cast<unsigned long long>(have_region ? region.base_va : 0),
                  static_cast<unsigned long long>(have_region ? region.size_bytes : 0));
    bx_log_emit(BX_LOG_INFO, "d3d12_xs", "D3D12ConfigureVirtualMemoryX", detail);
    return S_OK;
}

void bx_vmem_install_device_hooks(void* device) {
    /*
     * CreateHeap is slot 28 and CreatePlacedResource is slot 29 on ID3D12Device.
     * BxDeviceProxy does not override those methods, and proxy.cpp / proxy.h are left
     * untouched, so the slots cannot be replaced by a virtual override.
     * The proxy vtable is shared static data: every BxDeviceProxy instance points at
     * the same function table. Patching it once covers every device this DLL creates.
     * That table is in a read-only section, so VirtualProtect opens the two slots for
     * the store and then puts the previous protection back.
     * No other hook in this DLL replaces those methods without editing the proxy sources.
     */
    static LONG installed = 0;
    if (!device || InterlockedCompareExchange(&installed, 1, 0) != 0) {
        return;
    }
    auto* proxy = static_cast<BxDeviceProxy*>(device);
    void** vtable = *reinterpret_cast<void***>(proxy);
    DWORD old = 0;
    if (!VirtualProtect(vtable + 28, sizeof(void*) * 2, PAGE_READWRITE, &old)) {
        InterlockedExchange(&installed, 0);
        bx_log_emit(BX_LOG_ERROR, "d3d12_xs", "vmem_hook", "VirtualProtect on the device vtable failed");
        BX_VTABLE_STUB_LOG("ID3D12Device", 28u, "CreateHeap");
        return;
    }
    vtable[28] = reinterpret_cast<void*>(&create_heap);
    vtable[29] = reinterpret_cast<void*>(&create_placed);
    DWORD ignored = 0;
    VirtualProtect(vtable + 28, sizeof(void*) * 2, old, &ignored);
    bx_log_emit(BX_LOG_INFO, "d3d12_xs", "vmem_hook", "CreateHeap and CreatePlacedResource installed");
}
