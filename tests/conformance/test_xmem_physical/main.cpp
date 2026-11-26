#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "bx_xmem_internal.h"

extern "C" {
BOOL WINAPI XMemAllocatePhysicalPages(SIZE_T*, PULONG_PTR, uint64_t);
BOOL WINAPI XMemFreePhysicalPages(SIZE_T*, PULONG_PTR, uint64_t);
PVOID WINAPI XMemMapPhysicalPages(PVOID, SIZE_T, uint64_t, PULONG_PTR);
PVOID WINAPI XMemMapPhysicalPagesScatter(PVOID, SIZE_T, uint64_t, PULONG_PTR);
}

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "xmem physical:%d: %s (Win32 error %lu)\n",      \
                         __LINE__, #condition, static_cast<unsigned long>(GetLastError())); \
            return false;                                                       \
        }                                                                       \
    } while (false)

bool check_aliases(SIZE_T page_size, uint64_t flags) {
    SIZE_T count = 2;
    ULONG_PTR pages[2]{};
    CHECK(XMemAllocatePhysicalPages(&count, pages, flags));
    CHECK(count == 2 && pages[0] && pages[1] && pages[0] != pages[1]);

    auto* linear = static_cast<BYTE*>(XMemMapPhysicalPages(nullptr, count, flags, pages));
    CHECK(linear && reinterpret_cast<uintptr_t>(linear) % page_size == 0);
    ULONG_PTR reversed[] = {pages[1], pages[0]};
    auto* scatter = static_cast<BYTE*>(
        XMemMapPhysicalPagesScatter(nullptr, count, flags, reversed));
    CHECK(scatter && scatter != linear && reinterpret_cast<uintptr_t>(scatter) % page_size == 0);

    linear[7] = 0x41;
    linear[page_size + 7] = 0x73;
    CHECK(scatter[7] == 0x73 && scatter[page_size + 7] == 0x41);
    scatter[page_size + 30] = 0xf2;
    CHECK(linear[30] == 0xf2);

    bx_xmem_region_info_t first{};
    bx_xmem_region_info_t alias{};
    CHECK(bx_xmem_query_region(reinterpret_cast<uintptr_t>(linear + 7), &first));
    CHECK(bx_xmem_query_region(reinterpret_cast<uintptr_t>(scatter + page_size + 7), &alias));
    CHECK(first.base_va == reinterpret_cast<uintptr_t>(linear));
    CHECK(alias.base_va == reinterpret_cast<uintptr_t>(scatter + page_size));
    CHECK(alias.physical_tag == first.physical_tag && first.physical_tag == pages[0]);
    CHECK(first.size_bytes == page_size && alias.page_size_bytes == page_size);
    CHECK(first.protect_flags == PAGE_READWRITE && first.mem_type_flags == MEM_MAPPED);

    ULONG_PTR stale[] = {pages[0], 0};
    CHECK(!XMemFreePhysicalPages(&count, stale, flags));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER && linear[7] == 0x41);
    CHECK(!XMemMapPhysicalPagesScatter(nullptr, count, flags, stale));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(!XMemMapPhysicalPages(linear, count, flags, pages));
    CHECK(linear[7] == 0x41 && scatter[7] == 0x73);

    CHECK(XMemMapPhysicalPagesScatter(scatter, count, flags, nullptr) == scatter);
    CHECK(!bx_xmem_query_region(reinterpret_cast<uintptr_t>(scatter), &alias));
    CHECK(XMemMapPhysicalPagesScatter(scatter, count, flags, reversed) == scatter);
    CHECK(scatter[7] == 0x73 && scatter[page_size + 7] == 0x41);
    CHECK(XMemFreePhysicalPages(&count, pages, flags));
    CHECK(!pages[0] && !pages[1]);
    CHECK(!bx_xmem_query_region(reinterpret_cast<uintptr_t>(linear), &first));
    CHECK(!bx_xmem_query_region(reinterpret_cast<uintptr_t>(scatter), &alias));
    CHECK(!XMemMapPhysicalPages(nullptr, count, flags, reversed));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
    return true;
}

int main() {
    if (!check_aliases(64 * 1024, 0) || !check_aliases(2 * 1024 * 1024, 2 * 1024 * 1024)) {
        return 1;
    }
    std::puts("test_xmem_physical: passed");
}
