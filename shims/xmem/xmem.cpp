#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "bx_config.h"
#include "bx_log.h"
#include "bx_xmem_internal.h"

/*
 * Track B (M1-B1 / M1-B2): Starter implementation of xmem.dll.
 * Provides working default heap & virtual allocations so dependent DLLs can link
 * and load immediately; Track B issues expand physical/scatter page mapping,
 * __XMemAllocationHooks__ dispatch, and Lockhart working-set enforcement.
 */

namespace {
std::atomic<uint64_t> g_committed_virtual{0};
std::atomic<uint64_t> g_allocated_physical{0};
std::atomic<uint64_t> g_peak_committed{0};

void record_alloc(uint64_t bytes) {
    uint64_t cur = g_committed_virtual.fetch_add(bytes) + bytes;
    uint64_t peak = g_peak_committed.load();
    while (cur > peak && !g_peak_committed.compare_exchange_weak(peak, cur)) {}
}
} // namespace

extern "C" {

PVOID WINAPI XMemAllocDefault(SIZE_T size, uint64_t /*attributes*/) {
    if (size == 0) size = 16;
    void* p = _aligned_malloc(size, 64);
    if (p) record_alloc(size);
    return p;
}

VOID WINAPI XMemFreeDefault(PVOID address, uint64_t /*attributes*/) {
    if (address) _aligned_free(address);
}

PVOID WINAPI XMemAlloc(SIZE_T size, uint64_t attributes) {
    return XMemAllocDefault(size, attributes);
}

VOID WINAPI XMemFree(PVOID address, uint64_t attributes) {
    XMemFreeDefault(address, attributes);
}

PVOID WINAPI XMemVirtualAlloc(PVOID address, SIZE_T size, DWORD alloc_type, DWORD protect) {
    /* Strip console-only high bits before forwarding to standard Win32 VirtualAlloc. */
    DWORD win_alloc = alloc_type & (MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN);
    if (win_alloc == 0) win_alloc = MEM_COMMIT | MEM_RESERVE;
    DWORD win_prot = protect & 0xFFu;
    if (win_prot == 0) win_prot = PAGE_READWRITE;
    void* p = VirtualAlloc(address, size, win_alloc, win_prot);
    if (p && (win_alloc & MEM_COMMIT)) record_alloc(size);
    return p;
}

SIZE_T WINAPI XMemVirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buf, SIZE_T len) {
    return VirtualQuery(address, buf, len);
}

BOOL WINAPI XMemTransferMemory(PVOID dst, LPCVOID src, SIZE_T bytes, uint64_t /*flags*/) {
    if (!dst || !src) return FALSE;
    std::memcpy(dst, src, bytes);
    return TRUE;
}

BOOL WINAPI XMemGetWorkingSetStatistics(PVOID out_stats, SIZE_T stats_size) {
    BX_STUB_LOG("xmem.dll", "XMemGetWorkingSetStatistics");
    if (out_stats && stats_size > 0) {
        std::memset(out_stats, 0, stats_size);
    }
    return TRUE;
}

BOOL WINAPI XMemIsToolsMemAvailable(void) {
    return FALSE;
}

void bx_xmem_get_internal_stats(bx_xmem_stats_t* out_stats) {
    if (!out_stats) return;
    const bx_runtime_config_t* cfg = bx_get_runtime_config();
    out_stats->title_limit_bytes = cfg ? cfg->title_memory_limit_bytes : (8ULL << 30);
    out_stats->committed_virtual_bytes = g_committed_virtual.load();
    out_stats->allocated_physical_bytes = g_allocated_physical.load();
    out_stats->peak_committed_bytes = g_peak_committed.load();
}

} // extern "C"
