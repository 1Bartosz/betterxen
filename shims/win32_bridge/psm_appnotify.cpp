#include <windows.h>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_set>

#include "bx_log.h"

namespace {

struct Registrations {
    std::mutex mutex;
    std::unordered_set<uintptr_t> active;
    uintptr_t next;
};

// Each DLL exposes one family. Disjoint token values also prevent an
// unregister call on the wrong DLL from removing another registration.
Registrations g_state{{}, {}, 1};
Registrations g_constrained{{}, {}, 2};

ULONG register_notification(Registrations& registrations, PVOID callback,
                            PVOID* out_reg, const char* event) {
    if (!out_reg) {
        return ERROR_INVALID_PARAMETER;
    }
    *out_reg = nullptr;
    if (!callback) {
        return ERROR_INVALID_PARAMETER;
    }

    uintptr_t token;
    {
        const std::lock_guard lock(registrations.mutex);
        // Never recycle a token that a caller may still retain.
        if (registrations.next > std::numeric_limits<uintptr_t>::max() - 2) {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        token = registrations.next;
        try {
            registrations.active.insert(token);
        } catch (const std::bad_alloc&) {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        registrations.next += 2;
    }

    *out_reg = reinterpret_cast<PVOID>(token);
    bx_log_emit(BX_LOG_INFO, "win32_bridge", event, "registered_offline");
    return ERROR_SUCCESS;
}

void unregister_notification(Registrations& registrations, PVOID reg, const char* event) {
    bool removed;
    {
        const std::lock_guard lock(registrations.mutex);
        removed = registrations.active.erase(reinterpret_cast<uintptr_t>(reg)) != 0;
    }
    bx_log_emit(BX_LOG_INFO, "win32_bridge", event, removed ? "unregistered" : "unknown_token");
}

} // namespace

extern "C" {

ULONG WINAPI RegisterAppStateChangeNotification(PVOID callback, PVOID /*context*/, PVOID* out_reg) {
    return register_notification(g_state, callback, out_reg, "RegisterAppStateChangeNotification");
}

VOID WINAPI UnregisterAppStateChangeNotification(PVOID reg) {
    unregister_notification(g_state, reg, "UnregisterAppStateChangeNotification");
}

ULONG WINAPI RegisterAppConstrainedChangeNotification(PVOID callback, PVOID /*context*/, PVOID* out_reg) {
    return register_notification(g_constrained, callback, out_reg, "RegisterAppConstrainedChangeNotification");
}

VOID WINAPI UnregisterAppConstrainedChangeNotification(PVOID reg) {
    unregister_notification(g_constrained, reg, "UnregisterAppConstrainedChangeNotification");
}

} // extern "C"
