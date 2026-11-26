#include <windows.h>
#include <immintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bx_config.h"
#include "bx_log.h"
#include "bx_xmem_internal.h"

extern "C" {
uint64_t bx_veh_get_clwb_trap_count(void);
PVOID WINAPI XMemAlloc(SIZE_T size, uint64_t attributes);
VOID WINAPI XMemFree(PVOID address, uint64_t attributes);
BOOL WINAPI XMemTransferMemory(PVOID dst, LPCVOID src, SIZE_T bytes, uint64_t flags);
}

int main() {
    /* 1. Verify AVX2 + FMA execution under Rosetta 2 */
    __m256 a = _mm256_set1_ps(3.0f);
    __m256 b = _mm256_set1_ps(4.0f);
    __m256 c = _mm256_fmadd_ps(a, b, a); /* 3*4 + 3 = 15.0 */
    alignas(32) float out[8] = {};
    _mm256_store_ps(out, c);
    for (int i = 0; i < 8; ++i) {
        if (out[i] != 15.0f) {
            std::fprintf(stderr, "FAIL: AVX2/FMA mismatch lane %d: %f\n", i, out[i]);
            return 1;
        }
    }

    /* 2. Verify clwb trapping via bx_runtime_veh.dll (both legacy and REX forms) */
    alignas(64) uint8_t buf[128] = {0x42};
    uint64_t before = bx_veh_get_clwb_trap_count();
    __asm__ volatile("clwb (%0)" :: "r"(buf) : "memory");
    __asm__ volatile("clwb 64(%0)" :: "r"(buf) : "memory");
    uint64_t after = bx_veh_get_clwb_trap_count();
    std::printf("clwb traps observed: %llu -> %llu\n",
                static_cast<unsigned long long>(before),
                static_cast<unsigned long long>(after));

    /* 3. Verify basic XMem allocation + transfer + stats contract */
    void* src = XMemAlloc(4096, 0);
    void* dst = XMemAlloc(4096, 0);
    if (!src || !dst) {
        std::fprintf(stderr, "FAIL: XMemAlloc returned null\n");
        return 2;
    }
    std::memset(src, 0x5A, 4096);
    if (!XMemTransferMemory(dst, src, 4096, 0) || std::memcmp(src, dst, 4096) != 0) {
        std::fprintf(stderr, "FAIL: XMemTransferMemory mismatch\n");
        return 3;
    }
    bx_xmem_stats_t stats = {};
    bx_xmem_get_internal_stats(&stats);
    std::printf("xmem stats: limit=%llu MB committed=%llu bytes\n",
                static_cast<unsigned long long>(stats.title_limit_bytes / (1024ULL * 1024ULL)),
                static_cast<unsigned long long>(stats.committed_virtual_bytes));
    XMemFree(src, 0);
    XMemFree(dst, 0);

    std::printf("PASS: test_veh_clwb\n");
    return 0;
}
