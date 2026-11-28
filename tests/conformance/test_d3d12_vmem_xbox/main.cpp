/*
 * Synthetic conformance test for the inferred 32-byte Xbox configuration
 * decoder in d3d12_xs.dll!D3D12ConfigureVirtualMemoryX.
 *
 * Verifies:
 *  - valid Xbox configurations (3 and 4 virtual segments) are recorded
 *    without mutating caller bytes, report applied=0 recorded_only=1, and
 *    still emit the unimplemented-API stub, because nothing consumes the
 *    segment count yet;
 *  - repeated identical and updated Xbox calls log repeat=1 and repeat=0;
 *  - an Xbox configuration leaves a cached BXVM backing heap and its
 *    cpu_pointer untouched, while ordinary D3D12 heaps and XMemVirtualAlloc
 *    reservations keep working;
 *  - malformed, non-zero trailing, null, and page-edge
 *    unreadable configurations fall back safely without faulting or mutating
 *    caller memory and log the exact decoder and reason.
 *
 * Several fixtures share one BX_LOG_FILE, so every log check reads only the
 * bytes appended since the step it checks began.
 */
#include <d3d12.h>
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "bx_config.h"
#include "bx_log.h"
#include "bx_xmem_internal.h"

extern "C" {
__declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(void* params, REFIID riid, void** ppv);
__declspec(dllimport) HRESULT WINAPI D3D12ConfigureVirtualMemoryX(void* config);
__declspec(dllimport) PVOID WINAPI XMemVirtualAlloc(PVOID address, SIZE_T size, DWORD alloc_type, DWORD protect);
__declspec(dllimport) BOOL WINAPI XMemAllocatePhysicalPages(SIZE_T* page_count, PULONG_PTR pages, uint64_t flags);
__declspec(dllimport) BOOL WINAPI XMemFreePhysicalPages(SIZE_T* page_count, PULONG_PTR pages, uint64_t flags);
__declspec(dllimport) PVOID WINAPI XMemMapPhysicalPages(PVOID address, SIZE_T page_count, uint64_t flags,
                                                        PULONG_PTR pages);
}

namespace {

constexpr uint32_t kBxvmMagic = 0x4D565842u;
constexpr uint32_t kBxvmFlagBacking = 0x1u;
constexpr uint64_t k64K = 64ull * 1024ull;

struct BxvmConfig {
    uint32_t magic;
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint64_t base_va;
    uint64_t size_bytes;
    uint64_t reserved;
    uint64_t cpu_pointer;
};

struct XboxConfig {
    uint32_t reserved0;
    uint32_t virtual_segment_count;
    uint32_t reserved[6];
};
static_assert(sizeof(XboxConfig) == 32, "XboxConfig must be 32 bytes");

int g_failed = 0;

void check(bool cond, const char* fmt, ...) {
    if (cond) {
        return;
    }
    ++g_failed;
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "FAIL: ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

unsigned long ul(HRESULT hr) {
    return static_cast<unsigned long>(hr);
}

bool read_file_from(const char* path, long start, std::string& out) {
    out.clear();
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (start > 0 && std::fseek(f, start, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return true;
}

/* Current size of the log, used as the start offset of the next step. */
long log_size(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return 0;
    }
    long size = 0;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        size = std::ftell(f);
    }
    std::fclose(f);
    return size < 0 ? 0 : size;
}

bool log_contains_since(const char* path, long start, const char* needle) {
    std::string text;
    return read_file_from(path, start, text) && text.find(needle) != std::string::npos;
}

constexpr const char* kStubNeedle = "d3d12_xs.dll!D3D12ConfigureVirtualMemoryX";

BxvmConfig make_bxvm(uint64_t base_va, uint64_t size_bytes, uint32_t flags) {
    BxvmConfig cfg{};
    cfg.magic = kBxvmMagic;
    cfg.struct_size = sizeof(BxvmConfig);
    cfg.version = 1;
    cfg.flags = flags;
    cfg.base_va = base_va;
    cfg.size_bytes = size_bytes;
    cfg.reserved = 0;
    cfg.cpu_pointer = 0xA5A5A5A5A5A5A5A5ull;
    return cfg;
}

XboxConfig make_xbox(uint32_t segments) {
    XboxConfig cfg{};
    cfg.reserved0 = 0;
    cfg.virtual_segment_count = segments;
    return cfg;
}

} // namespace

int main() {
    const bx_runtime_config_t* cfg = bx_get_runtime_config();
    if (!cfg) {
        std::fprintf(stderr, "FAIL: test_d3d12_vmem_xbox (no runtime config)\n");
        return 1;
    }

    char log_path[MAX_PATH] = {};
    const DWORD log_len = GetEnvironmentVariableA("BX_LOG_FILE", log_path, sizeof(log_path));
    const bool have_log = log_len > 0 && log_len < sizeof(log_path);
    check(have_log, "BX_LOG_FILE must be set");

    long mark = have_log ? log_size(log_path) : 0;
    const long test_start = mark;

    /* 1. Valid Xbox configuration (4 virtual segments, Anaconda/24 GiB shape). */
    XboxConfig cfg4 = make_xbox(4);
    const XboxConfig cfg4_before = cfg4;
    HRESULT hr = D3D12ConfigureVirtualMemoryX(&cfg4);
    check(hr == S_OK, "valid Xbox config (4 segments) must return S_OK (hr=0x%08lx)", ul(hr));
    check(std::memcmp(&cfg4, &cfg4_before, sizeof(cfg4)) == 0,
          "Xbox decoder must not mutate caller memory");

    if (have_log) {
        check(log_contains_since(log_path, mark, "decoder=xbox applied=0 recorded_only=1 virtual_segments=4 repeat=0"),
              "log must record the 4-segment Xbox config as recorded only");
        check(log_contains_since(log_path, mark, kStubNeedle),
              "a recorded-only Xbox config must still emit the unimplemented-API stub");
        check(!log_contains_since(log_path, mark, "applied=1"),
              "a recorded-only Xbox config must not report applied=1");
        mark = log_size(log_path);
    }

    /* 2. Repeated identical Xbox call (repeat=1). */
    hr = D3D12ConfigureVirtualMemoryX(&cfg4);
    check(hr == S_OK, "repeated identical Xbox config must return S_OK (hr=0x%08lx)", ul(hr));
    if (have_log) {
        check(log_contains_since(log_path, mark, "decoder=xbox applied=0 recorded_only=1 virtual_segments=4 repeat=1"),
              "repeated identical Xbox config must log repeat=1");
        mark = log_size(log_path);
    }

    /* 3. Updated Xbox call (3 segments, Lockhart/16 GiB shape). */
    XboxConfig cfg3 = make_xbox(3);
    const XboxConfig cfg3_before = cfg3;
    hr = D3D12ConfigureVirtualMemoryX(&cfg3);
    check(hr == S_OK, "updated Xbox config (3 segments) must return S_OK (hr=0x%08lx)", ul(hr));
    check(std::memcmp(&cfg3, &cfg3_before, sizeof(cfg3)) == 0,
          "updated Xbox config must not mutate caller memory");
    if (have_log) {
        check(log_contains_since(log_path, mark, "decoder=xbox applied=0 recorded_only=1 virtual_segments=3 repeat=0"),
              "updated Xbox config must log virtual_segments=3 repeat=0");
        mark = log_size(log_path);
    }

    /* No upper bound is inferred: the count is only recorded and never consumed. */
    XboxConfig large_count = make_xbox(UINT32_MAX);
    const XboxConfig large_count_before = large_count;
    hr = D3D12ConfigureVirtualMemoryX(&large_count);
    check(hr == S_OK, "unbounded recorded count must return S_OK (hr=0x%08lx)", ul(hr));
    check(std::memcmp(&large_count, &large_count_before, sizeof(large_count)) == 0,
          "large recorded count must not mutate caller memory");
    if (have_log) {
        check(log_contains_since(log_path, mark,
                                 "decoder=xbox applied=0 recorded_only=1 virtual_segments=4294967295 repeat=0"),
              "large count must be recorded without imposing an inferred upper bound");
        mark = log_size(log_path);
    }

    /*
     * 4. Verify XMemVirtualAlloc reservation works after Xbox configuration
     *    (matching the caller's MEM_RESERVE + console high-bit pattern).
     */
    PVOID reserved_va = XMemVirtualAlloc(nullptr, 2ull * 1024ull * 1024ull, 0x20402000u, 0x21u);
    check(reserved_va != nullptr, "XMemVirtualAlloc reservation after Xbox config must succeed");
    if (reserved_va) {
        VirtualFree(reserved_va, 0, MEM_RELEASE);
    }

    /*
     * 5. Configure a BXVM backing heap, create and cache it, then apply an Xbox
     *    configuration. The Xbox path only records its segment count, so the
     *    cached BXVM backing heap, its cpu_pointer and its device link must
     *    survive.
     */
    ID3D12Device* device = nullptr;
    hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    check(hr == S_OK && device != nullptr, "D3D12XboxCreateDevice must succeed (hr=0x%08lx)", ul(hr));

    SIZE_T page_count = 2;
    ULONG_PTR pages[2] = {};
    const BOOL phys_ok = XMemAllocatePhysicalPages(&page_count, pages, 0);
    check(phys_ok && page_count == 2, "XMemAllocatePhysicalPages(2) must succeed");
    uint8_t* mapped_region = phys_ok ? static_cast<uint8_t*>(XMemMapPhysicalPages(nullptr, 2, 0, pages)) : nullptr;
    check(mapped_region != nullptr, "XMemMapPhysicalPages(2) must succeed");

    if (device && mapped_region) {
        const uint64_t region_va = reinterpret_cast<uint64_t>(mapped_region);
        const uint64_t region_bytes = 2ull * k64K;

        BxvmConfig bxvm = make_bxvm(region_va, region_bytes, kBxvmFlagBacking);
        hr = D3D12ConfigureVirtualMemoryX(&bxvm);
        check(hr == S_OK, "BXVM backing config must succeed (hr=0x%08lx)", ul(hr));
        check(bxvm.cpu_pointer == 0, "BXVM cpu_pointer must start at 0 before CreateHeap");

        D3D12_HEAP_DESC backing_desc{};
        backing_desc.SizeInBytes = region_bytes;
        backing_desc.Alignment = k64K;
        backing_desc.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        backing_desc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        backing_desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        backing_desc.Flags = D3D12_HEAP_FLAG_NONE;

        ID3D12Heap* backing_heap = nullptr;
        hr = device->CreateHeap(&backing_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&backing_heap));
        check(hr == S_OK && backing_heap != nullptr, "CreateHeap for BXVM backing must succeed (hr=0x%08lx)", ul(hr));

        BxvmConfig query = make_bxvm(region_va, region_bytes, kBxvmFlagBacking);
        hr = D3D12ConfigureVirtualMemoryX(&query);
        check(hr == S_OK && query.cpu_pointer != 0,
              "BXVM query after CreateHeap must return non-zero cached cpu_pointer (hr=0x%08lx)", ul(hr));

        /* An Xbox config between two BXVM calls must not evict the cached backing heap. */
        XboxConfig switch_cfg = make_xbox(4);
        hr = D3D12ConfigureVirtualMemoryX(&switch_cfg);
        check(hr == S_OK, "Xbox config after BXVM must return S_OK (hr=0x%08lx)", ul(hr));

        if (backing_heap) {
            ID3D12Device* still_dev = nullptr;
            const HRESULT dev_hr = backing_heap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&still_dev));
            check(SUCCEEDED(dev_hr) && still_dev != nullptr,
                  "cached BXVM backing heap must keep its device after an Xbox config (hr=0x%08lx)", ul(dev_hr));
            if (still_dev) {
                still_dev->Release();
            }
        }

        BxvmConfig after_xbox = make_bxvm(region_va, region_bytes, kBxvmFlagBacking);
        hr = D3D12ConfigureVirtualMemoryX(&after_xbox);
        check(hr == S_OK && after_xbox.cpu_pointer == query.cpu_pointer,
              "BXVM cpu_pointer must be unchanged after an Xbox config (hr=0x%08lx)", ul(hr));

        if (backing_heap) {
            backing_heap->Release();
            backing_heap = nullptr;
        }

        /* Restore Xbox config and verify ordinary D3D12 heap creation still works. */
        hr = D3D12ConfigureVirtualMemoryX(&switch_cfg);
        check(hr == S_OK, "restoring Xbox config must return S_OK (hr=0x%08lx)", ul(hr));

        D3D12_HEAP_DESC ordinary_desc{};
        ordinary_desc.SizeInBytes = k64K;
        ordinary_desc.Alignment = k64K;
        ordinary_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        ordinary_desc.Flags = D3D12_HEAP_FLAG_NONE;
        ID3D12Heap* ordinary_heap = nullptr;
        hr = device->CreateHeap(&ordinary_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&ordinary_heap));
        check(hr == S_OK && ordinary_heap != nullptr,
              "ordinary CreateHeap must succeed after Xbox config (hr=0x%08lx)", ul(hr));
        if (ordinary_heap) {
            ordinary_heap->Release();
        }
    }

    if (mapped_region) {
        XMemMapPhysicalPages(mapped_region, 2, 0, nullptr);
    }
    if (phys_ok) {
        SIZE_T free_count = 2;
        XMemFreePhysicalPages(&free_count, pages, 0);
    }
    if (device) {
        device->Release();
    }

    /* 6. Malformed, unsupported, and page-edge unreadable inputs. */
    if (have_log) {
        mark = log_size(log_path);
    }
    check(D3D12ConfigureVirtualMemoryX(nullptr) == S_OK, "null config must fall back with S_OK");

    uint32_t unknown_hdr[8] = {0x11223344u, 4u, 0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t unknown_hdr_before[8];
    std::memcpy(unknown_hdr_before, unknown_hdr, sizeof(unknown_hdr));
    check(D3D12ConfigureVirtualMemoryX(unknown_hdr) == S_OK, "unrecognized header must fall back with S_OK");
    check(std::memcmp(unknown_hdr, unknown_hdr_before, sizeof(unknown_hdr)) == 0,
          "unrecognized header bytes must stay unchanged");

    BxvmConfig readonly_bxvm = make_bxvm(0, 0, 0);
    SYSTEM_INFO readonly_sys{};
    GetSystemInfo(&readonly_sys);
    const SIZE_T readonly_page_bytes = readonly_sys.dwPageSize ? readonly_sys.dwPageSize : 4096;
    void* readonly_page =
        VirtualAlloc(nullptr, readonly_page_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(readonly_page != nullptr, "VirtualAlloc for read-only BXVM probe must succeed");
    if (readonly_page) {
        std::memcpy(readonly_page, &readonly_bxvm, sizeof(readonly_bxvm));
        DWORD old_protect = 0;
        const bool protected_readonly =
            VirtualProtect(readonly_page, readonly_page_bytes, PAGE_READONLY, &old_protect) != 0;
        check(protected_readonly, "BXVM config page must become read-only");
        if (protected_readonly) {
            check(D3D12ConfigureVirtualMemoryX(readonly_page) == S_OK,
                  "read-only BXVM output must fall back with S_OK");
            check(std::memcmp(readonly_page, &readonly_bxvm, sizeof(readonly_bxvm)) == 0,
                  "read-only BXVM config bytes must stay unchanged");
            DWORD ignored_protect = 0;
            VirtualProtect(readonly_page, readonly_page_bytes, old_protect, &ignored_protect);
        }
        VirtualFree(readonly_page, 0, MEM_RELEASE);
    }

    XboxConfig zero_seg = make_xbox(0);
    const XboxConfig zero_seg_before = zero_seg;
    check(D3D12ConfigureVirtualMemoryX(&zero_seg) == S_OK, "zero segment count must fall back with S_OK");
    check(std::memcmp(&zero_seg, &zero_seg_before, sizeof(zero_seg)) == 0,
          "zero segment count bytes must stay unchanged");

    XboxConfig bad_fields = make_xbox(4);
    bad_fields.reserved[2] = 1u;
    const XboxConfig bad_fields_before = bad_fields;
    check(D3D12ConfigureVirtualMemoryX(&bad_fields) == S_OK, "non-zero trailing Xbox field must fall back with S_OK");
    check(std::memcmp(&bad_fields, &bad_fields_before, sizeof(bad_fields)) == 0,
          "non-zero trailing Xbox field bytes must stay unchanged");

    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    const SIZE_T page_bytes = sys.dwPageSize ? sys.dwPageSize : 4096;
    void* guard_mem = VirtualAlloc(nullptr, page_bytes * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(guard_mem != nullptr, "VirtualAlloc for page-edge probe must succeed");
    if (guard_mem) {
        DWORD old_prot = 0;
        auto* base = static_cast<uint8_t*>(guard_mem);
        check(VirtualProtect(base + page_bytes, page_bytes, PAGE_NOACCESS, &old_prot) != 0,
              "second page must be PAGE_NOACCESS");

        /* Unreadable header directly on NOACCESS page. */
        check(D3D12ConfigureVirtualMemoryX(base + page_bytes) == S_OK,
              "unreadable header must not fault and must return S_OK");

        /* 8 readable bytes {0, 4} followed immediately by NOACCESS page. */
        uint8_t* edge = base + page_bytes - 8;
        const uint32_t zero = 0u;
        const uint32_t four = 4u;
        std::memcpy(edge, &zero, 4);
        std::memcpy(edge + 4, &four, 4);
        check(D3D12ConfigureVirtualMemoryX(edge) == S_OK,
              "Xbox header at page edge with unreadable tail must not fault and must return S_OK");

        VirtualProtect(base + page_bytes, page_bytes, PAGE_READWRITE, &old_prot);
        VirtualFree(guard_mem, 0, MEM_RELEASE);
    }

    if (have_log) {
        constexpr const char* expected_fallbacks[] = {
            "\"event\":\"vmem_config_fallback\",\"detail\":\"decoder=none applied=0 reason=null_config\"",
            "\"event\":\"vmem_config_fallback\",\"detail\":\"decoder=none applied=0 reason=unrecognized_header\"",
            "\"event\":\"vmem_config_fallback\",\"detail\":\"decoder=none applied=0 reason=unreadable_header\"",
            "\"event\":\"vmem_config_fallback\",\"detail\":\"decoder=bxvm applied=0 reason=unwritable_output\"",
            "\"event\":\"vmem_config_fallback\",\"detail\":\"decoder=xbox applied=0 reason=zero_segment_count\"",
            "\"event\":\"vmem_config_fallback\",\"detail\":\"decoder=xbox applied=0 reason=unsupported_fields\"",
            "\"event\":\"vmem_config_fallback\",\"detail\":\"decoder=xbox applied=0 reason=unreadable_tail\"",
        };
        for (const char* needle : expected_fallbacks) {
            check(log_contains_since(log_path, mark, needle), "missing expected fallback log: %s", needle);
        }
        check(log_contains_since(log_path, test_start, "\"event\":\"vmem_config_recorded\""),
              "the Xbox path must log vmem_config_recorded");
    }

    if (g_failed != 0) {
        std::fprintf(stderr, "FAIL: test_d3d12_vmem_xbox (%d failed)\n", g_failed);
        return 1;
    }
    std::printf("PASS: test_d3d12_vmem_xbox\n");
    return 0;
}
