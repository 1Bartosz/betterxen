#include <windows.h>
#include <cstdint>

#include "bx_log.h"

/* Track E (M1-E2 / M5): chad.dll Xbox DSP command shim. */

extern "C" {

HRESULT WINAPI XDspConnectWithMaximumStreamLimit(uint32_t /*max_streams*/, void** out_handle) {
    BX_STUB_LOG("chad.dll", "XDspConnectWithMaximumStreamLimit");
    if (out_handle) *out_handle = reinterpret_cast<void*>(0x5001);
    return S_OK;
}

HRESULT WINAPI XDspDisconnect(void* /*handle*/) { return S_OK; }
HRESULT WINAPI XDspActivate(void* /*handle*/) { return S_OK; }
HRESULT WINAPI XDspDeactivate(void* /*handle*/) { return S_OK; }
HRESULT WINAPI XDspSubmitCommand(void* /*handle*/, const void* /*cmd*/, uint32_t /*size*/) { return S_OK; }

} // extern "C"
