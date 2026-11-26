#include <windows.h>
#include <cstdint>
#include <cstdlib>

#include "bx_log.h"

/* Track E (M1-E2 / M5): AcpHal.dll SHAPE Audio Coprocessor HAL shim. */

extern "C" {

HRESULT WINAPI AcpHalCreate(void** out_hal) {
    BX_STUB_LOG("AcpHal.dll", "AcpHalCreate");
    if (out_hal) *out_hal = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI AcpHalAllocateShapeContexts(void* /*desc*/) {
    BX_STUB_LOG("AcpHal.dll", "AcpHalAllocateShapeContexts");
    return S_OK;
}

HRESULT WINAPI AcpHalReleaseShapeContexts(void) {
    BX_STUB_LOG("AcpHal.dll", "AcpHalReleaseShapeContexts");
    return S_OK;
}

HRESULT WINAPI ApuCreateHeap(uint32_t /*cached_size*/, uint32_t /*non_cached_size*/) {
    BX_STUB_LOG("AcpHal.dll", "ApuCreateHeap");
    return S_OK;
}

HRESULT WINAPI ApuAlloc(void** out_va, uint32_t* out_apu_addr, uint32_t size, uint32_t alignment) {
    if (alignment < 64) alignment = 64;
    void* p = _aligned_malloc(size ? size : 64, alignment);
    if (out_va) *out_va = p;
    if (out_apu_addr) *out_apu_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p) & 0xFFFFFFFFu);
    return p ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI ApuFree(void* va) {
    if (va) _aligned_free(va);
    return S_OK;
}

BOOL WINAPI ApuIsVirtualAddressValid(const void* va, uint32_t /*size*/) {
    return va != nullptr;
}

void* WINAPI ApuMapApuAddress(uint32_t /*apu_addr*/) {
    BX_STUB_LOG("AcpHal.dll", "ApuMapApuAddress");
    return nullptr;
}

uint32_t WINAPI ApuMapVirtualAddress(const void* va) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(va) & 0xFFFFFFFFu);
}

} // extern "C"
