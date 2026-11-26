#include <windows.h>

#include "bx_log.h"

/* Track B (M1-B3): DirectStorage factory & async I/O queue shim. */

extern "C" HRESULT WINAPI DStorageGetFactory(REFIID /*riid*/, void** ppv) {
    BX_STUB_LOG("XCRDAPI.dll", "DStorageGetFactory");
    if (ppv) {
        *ppv = nullptr;
    }
    return E_NOTIMPL;
}
