#include <windows.h>
#include <cstdint>

#include "bx_log.h"

/* Track E (M1-E2 / M5): xapu.dll APU command queue shim. */

extern "C" {

HRESULT WINAPI XApuConnect(void** out_conn) {
    BX_STUB_LOG("xapu.dll", "XApuConnect");
    if (out_conn) *out_conn = reinterpret_cast<void*>(0x4001);
    return S_OK;
}

HRESULT WINAPI XApuDisconnect(void* /*conn*/) {
    return S_OK;
}

HRESULT WINAPI XApuEnqueueCommand(void* /*conn*/, const void* /*cmd*/, uint32_t /*size*/) {
    return S_OK;
}

HRESULT WINAPI XApuDequeueResult(void* /*conn*/, void* /*out_res*/, uint32_t /*size*/) {
    return S_FALSE;
}

uint32_t WINAPI XApuGetResultCount(void* /*conn*/) {
    return 0u;
}

} // extern "C"
