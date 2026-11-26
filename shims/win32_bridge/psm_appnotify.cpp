#include <windows.h>
#include <atomic>
#include <cstdint>

#include "bx_log.h"

namespace {
std::atomic<uintptr_t> g_next_token{1};
}

extern "C" {

ULONG WINAPI RegisterAppStateChangeNotification(PVOID /*callback*/, PVOID /*context*/, PVOID* out_reg) {
    bx_log_emit(BX_LOG_INFO, "win32_bridge", "RegisterAppStateChangeNotification", "noop_ok");
    if (out_reg) {
        *out_reg = reinterpret_cast<PVOID>(g_next_token.fetch_add(1));
    }
    return ERROR_SUCCESS;
}

VOID WINAPI UnregisterAppStateChangeNotification(PVOID /*reg*/) {
    bx_log_emit(BX_LOG_INFO, "win32_bridge", "UnregisterAppStateChangeNotification", "noop_ok");
}

ULONG WINAPI RegisterAppConstrainedChangeNotification(PVOID /*callback*/, PVOID /*context*/, PVOID* out_reg) {
    bx_log_emit(BX_LOG_INFO, "win32_bridge", "RegisterAppConstrainedChangeNotification", "noop_ok");
    if (out_reg) {
        *out_reg = reinterpret_cast<PVOID>(g_next_token.fetch_add(1));
    }
    return ERROR_SUCCESS;
}

VOID WINAPI UnregisterAppConstrainedChangeNotification(PVOID /*reg*/) {
    bx_log_emit(BX_LOG_INFO, "win32_bridge", "UnregisterAppConstrainedChangeNotification", "noop_ok");
}

} // extern "C"
