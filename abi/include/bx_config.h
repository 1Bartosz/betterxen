#ifndef BETTERXEN_BX_CONFIG_H
#define BETTERXEN_BX_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BX_ABI_VERSION 1u

/* Environment variables configured by bx-cli when launching under CrossOver/Wine. */
#define BX_ENV_PROFILE          "BX_PROFILE"          /* "lockhart" (default) | "anaconda" */
#define BX_ENV_LOG_FILE         "BX_LOG_FILE"         /* Path to JSONL/text trace log inside guest */
#define BX_ENV_LOG_LEVEL        "BX_LOG_LEVEL"        /* "error" | "warn" | "info" | "trace" */
#define BX_ENV_TRACE_D3D        "BX_TRACE_D3D"        /* "1" to trace every COM call */
#define BX_ENV_SHADER_DUMP_DIR  "BX_SHADER_DUMP_DIR"  /* Optional local dump dir for shader blobs */
#define BX_ENV_MEM_LIMIT_MB     "BX_MEM_LIMIT_MB"     /* Override reported title working-set cap */
#define BX_ENV_RT_MODE          "BX_RT_MODE"          /* "rebuild" | "bypass" */

typedef enum bx_hardware_profile {
    BX_PROFILE_LOCKHART = 0, /* Series S memory budget (~8-10 GB; default on 16 GB hosts) */
    BX_PROFILE_ANACONDA = 1  /* Series X memory budget (~13.5 GB; opt-in on 32 GB+ hosts) */
} bx_hardware_profile_t;

typedef enum bx_log_level {
    BX_LOG_ERROR = 0,
    BX_LOG_WARN  = 1,
    BX_LOG_INFO  = 2,
    BX_LOG_TRACE = 3
} bx_log_level_t;

typedef struct bx_runtime_config {
    uint32_t abi_version;
    bx_hardware_profile_t profile;
    bx_log_level_t log_level;
    uint64_t title_memory_limit_bytes;
    uint32_t trace_d3d;
    uint32_t enable_clwb_veh;
    uint32_t enable_adx_tripwire;
    uint32_t reserved;
} bx_runtime_config_t;

#ifdef __cplusplus
}
#endif

#endif /* BETTERXEN_BX_CONFIG_H */
