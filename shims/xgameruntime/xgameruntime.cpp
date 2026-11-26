#include <windows.h>
#include <cstdint>
#include <cstring>

#include "bx_config.h"
#include "bx_log.h"

/* Track E (M1-E1): Offline GDK XGameRuntime shim (Lockhart/Anaconda aware). */

extern "C" {

HRESULT WINAPI XGameRuntimeInitialize(void) {
    bx_log_emit(BX_LOG_INFO, "xgameruntime", "XGameRuntimeInitialize", "ok");
    return S_OK;
}

HRESULT WINAPI XGameRuntimeInitializeWithOptions(const void* /*options*/) {
    bx_log_emit(BX_LOG_INFO, "xgameruntime", "XGameRuntimeInitializeWithOptions", "ok");
    return S_OK;
}

void WINAPI XGameRuntimeUninitialize(void) {
    bx_log_emit(BX_LOG_INFO, "xgameruntime", "XGameRuntimeUninitialize", "ok");
}

HRESULT WINAPI XTaskQueueCreate(uint32_t /*work_mode*/, uint32_t /*completion_mode*/, void** out_queue) {
    BX_STUB_LOG("xgameruntime.dll", "XTaskQueueCreate");
    if (out_queue) *out_queue = reinterpret_cast<void*>(0x1001);
    return S_OK;
}

void WINAPI XTaskQueueCloseHandle(void* /*queue*/) {}

BOOL WINAPI XTaskQueueDispatch(void* /*queue*/, uint32_t /*port*/, uint32_t /*timeout_ms*/) {
    return FALSE;
}

HRESULT WINAPI XTaskQueueSubmitCallback(void* /*queue*/, uint32_t /*port*/, void* /*ctx*/, void* /*cb*/) {
    BX_STUB_LOG("xgameruntime.dll", "XTaskQueueSubmitCallback");
    return S_OK;
}

BOOL WINAPI XTaskQueueGetCurrentProcessTaskQueue(void** out_queue) {
    if (out_queue) *out_queue = reinterpret_cast<void*>(0x1000);
    return TRUE;
}

HRESULT WINAPI XAsyncGetStatus(void* /*async*/, BOOL /*wait*/) { return S_OK; }
HRESULT WINAPI XAsyncGetResultSize(void* /*async*/, size_t* out_size) {
    if (out_size) *out_size = 0;
    return S_OK;
}
void WINAPI XAsyncCancel(void* /*async*/) {}

HRESULT WINAPI XUserAddAsync(uint32_t /*opts*/, void* /*async*/) { BX_STUB_LOG("xgameruntime.dll", "XUserAddAsync"); return S_OK; }
HRESULT WINAPI XUserAddResult(void* /*async*/, void** out_user) { if (out_user) *out_user = reinterpret_cast<void*>(0x2001); return S_OK; }
void WINAPI XUserCloseHandle(void* /*user*/) {}
HRESULT WINAPI XUserGetId(void* /*user*/, uint64_t* out_id) { if (out_id) *out_id = 0x0009000000000001ULL; return S_OK; }
HRESULT WINAPI XUserGetGamertag(void* /*user*/, uint32_t /*comp*/, size_t size, char* out, size_t* used) {
    static const char kTag[] = "BetterXenLocal";
    size_t len = sizeof(kTag);
    if (out && size > 0) {
        size_t copy = len < size ? len : size - 1;
        std::memcpy(out, kTag, copy);
        out[copy] = '\0';
    }
    if (used) *used = len;
    return S_OK;
}
HRESULT WINAPI XUserGetTokenAndSignatureAsync(void*, uint32_t, const char*, const char*, size_t, const void*, size_t, const void*, void*) { BX_STUB_LOG("xgameruntime.dll", "XUserGetTokenAndSignatureAsync"); return E_NOTIMPL; }
HRESULT WINAPI XUserGetTokenAndSignatureResultSize(void*, size_t* s) { if (s) *s = 0; return E_NOTIMPL; }
HRESULT WINAPI XUserGetTokenAndSignatureResult(void*, size_t, void*, void**, size_t*) { return E_NOTIMPL; }
HRESULT WINAPI XUserResolvePrivilegeWithUiAsync(void*, uint32_t, uint32_t, void*) { return S_OK; }
HRESULT WINAPI XUserResolvePrivilegeWithUiResult(void*) { return S_OK; }

HRESULT WINAPI XStoreCreateContext(void* /*user*/, void** out_ctx) { if (out_ctx) *out_ctx = reinterpret_cast<void*>(0x3001); return S_OK; }
void WINAPI XStoreCloseContextHandle(void* /*ctx*/) {}
HRESULT WINAPI XStoreQueryGameLicenseAsync(void*, void*) { return S_OK; }
HRESULT WINAPI XStoreQueryGameLicenseResult(void*, void* out_lic) { if (out_lic) std::memset(out_lic, 0, 64); return S_OK; }
HRESULT WINAPI XStoreAcquireLicenseForPackageAsync(void*, const char*, void*) { return S_OK; }
HRESULT WINAPI XStoreAcquireLicenseForPackageResult(void*, void** out_lic) { if (out_lic) *out_lic = reinterpret_cast<void*>(0x3002); return S_OK; }
HRESULT WINAPI XStoreAcquireLicenseForDurablesAsync(void*, const char*, void*) { return S_OK; }
HRESULT WINAPI XStoreAcquireLicenseForDurablesResult(void*, void** out_lic) { if (out_lic) *out_lic = reinterpret_cast<void*>(0x3003); return S_OK; }
HRESULT WINAPI XStoreQueryEntitledProductsAsync(void*, uint32_t, void*) { return S_OK; }
HRESULT WINAPI XStoreQueryEntitledProductsResult(void*, void** out_q) { if (out_q) *out_q = nullptr; return S_OK; }
HRESULT WINAPI XStoreQueryAssociatedProductsAsync(void*, uint32_t, void*) { return S_OK; }
HRESULT WINAPI XStoreQueryAssociatedProductsResult(void*, void** out_q) { if (out_q) *out_q = nullptr; return S_OK; }

HRESULT WINAPI XSystemGetConsoleId(size_t size, char* out, size_t* used) {
    static const char kId[] = "00000000.00000000.00000000.00000000.00";
    if (out && size > 0) {
        size_t copy = sizeof(kId) < size ? sizeof(kId) : size - 1;
        std::memcpy(out, kId, copy);
        out[copy] = '\0';
    }
    if (used) *used = sizeof(kId);
    return S_OK;
}

uint32_t WINAPI XSystemGetDeviceType(void) {
    const bx_runtime_config_t* cfg = bx_get_runtime_config();
    /* Public GDK XSystemDeviceType enum: XboxScarlettLockhart = 6, XboxScarlettAnaconda = 7. */
    return (cfg && cfg->profile == BX_PROFILE_ANACONDA) ? 7u : 6u;
}

HRESULT WINAPI XSystemGetXboxLiveSandboxId(size_t size, char* out, size_t* used) {
    static const char kSb[] = "RETAIL";
    if (out && size > 0) {
        size_t copy = sizeof(kSb) < size ? sizeof(kSb) : size - 1;
        std::memcpy(out, kSb, copy);
        out[copy] = '\0';
    }
    if (used) *used = sizeof(kSb);
    return S_OK;
}

HRESULT WINAPI XPersistentLocalStorageGetPathSize(size_t* out_size) {
    if (out_size) *out_size = 16;
    return S_OK;
}
HRESULT WINAPI XPersistentLocalStorageGetPath(size_t size, char* out, size_t* used) {
    static const char kPath[] = "C:\\bx_pls";
    if (out && size > 0) {
        size_t copy = sizeof(kPath) < size ? sizeof(kPath) : size - 1;
        std::memcpy(out, kPath, copy);
        out[copy] = '\0';
    }
    if (used) *used = sizeof(kPath);
    return S_OK;
}
HRESULT WINAPI XPersistentLocalStorageGetSpaceInfo(void* out_info) {
    if (out_info) std::memset(out_info, 0, 32);
    return S_OK;
}

HRESULT WINAPI XGameUiShowPlayerProfileCardAsync(void*, uint64_t, void*) { return E_NOTIMPL; }
HRESULT WINAPI XGameUiShowTextEntryAsync(void*, const char*, const char*, const char*, uint32_t, uint32_t, void*) { return E_NOTIMPL; }
HRESULT WINAPI XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(void*) { return E_NOTIMPL; }
HRESULT WINAPI XNetworkingQuerySecurityInformationForUrlAsync(const char*, void*) { return E_NOTIMPL; }

} // extern "C"
