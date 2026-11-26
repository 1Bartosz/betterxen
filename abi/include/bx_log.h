#ifndef BETTERXEN_BX_LOG_H
#define BETTERXEN_BX_LOG_H

#include <stdint.h>
#include "bx_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Structured runtime diagnostics exported by bx_runtime_veh.dll and consumed
 * by all other BetterXen shim DLLs (via dynamic lookup or import library).
 */
void bx_log_emit(bx_log_level_t level, const char* subsystem, const char* event, const char* detail);
void bx_log_unimpl_api(const char* dll_name, const char* symbol_name);
void bx_log_unimpl_vtable(const char* iface_name, uint32_t slot_index, const char* method_name);
const bx_runtime_config_t* bx_get_runtime_config(void);

#define BX_STUB_LOG(dll, sym) bx_log_unimpl_api((dll), (sym))
#define BX_VTABLE_STUB_LOG(iface, slot, method) bx_log_unimpl_vtable((iface), (slot), (method))

#ifdef __cplusplus
}
#endif

#endif /* BETTERXEN_BX_LOG_H */
