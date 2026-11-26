#include <windows.h>
#include <cstdint>

#include "bx_log.h"

/* Track C (M1-C2): Scarlett RDNA 2 texture tiling / swizzle address computer. */

extern "C" {

HRESULT WINAPI XGCreateTextureComputer(const void* /*desc*/, void** out_computer) {
    BX_STUB_LOG("xg_xs.dll", "XGCreateTextureComputer");
    if (out_computer) {
        *out_computer = nullptr;
    }
    return E_NOTIMPL;
}

uint32_t WINAPI XGSuggestSwizzleMode(
    uint32_t /*resource_type*/,
    uint32_t /*format*/,
    uint32_t /*width*/,
    uint32_t /*height*/,
    uint32_t /*depth*/,
    uint32_t /*mip_levels*/,
    uint32_t /*sample_count*/,
    uint32_t /*flags*/
) {
    BX_STUB_LOG("xg_xs.dll", "XGSuggestSwizzleMode");
    return 0u; /* Linear fallback during M0/M1 bring-up */
}

} // extern "C"
