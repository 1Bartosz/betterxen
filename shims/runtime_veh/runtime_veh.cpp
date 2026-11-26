#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bx_config.h"
#include "bx_log.h"

namespace {

CRITICAL_SECTION g_log_lock;
bool g_log_lock_ready = false;
HANDLE g_log_handle = INVALID_HANDLE_VALUE;
PVOID g_veh_handle = nullptr;
std::atomic<uint64_t> g_clwb_traps{0};
bx_runtime_config_t g_config = {
    BX_ABI_VERSION,
    BX_PROFILE_LOCKHART,
    BX_LOG_INFO,
    8ULL * 1024ULL * 1024ULL * 1024ULL, /* Default 8 GiB Lockhart title budget */
    0u,
    1u,
    1u,
    0u
};

void write_log_line(const char* line, size_t len) {
    if (g_log_lock_ready) {
        EnterCriticalSection(&g_log_lock);
    }
    if (g_log_handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_log_handle, line, static_cast<DWORD>(len), &written, nullptr);
    } else {
        DWORD written = 0;
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (err && err != INVALID_HANDLE_VALUE) {
            WriteFile(err, line, static_cast<DWORD>(len), &written, nullptr);
        }
    }
    OutputDebugStringA(line);
    if (g_log_lock_ready) {
        LeaveCriticalSection(&g_log_lock);
    }
}

/*
 * Decode an x86-64 `clwb m8` instruction at `ip`.
 * Encoding: [legacy prefixes] 0x66 [REX 0x40..0x4F] 0x0F 0xAE /6 (mod != 3).
 * Returns total instruction length in bytes, or 0 if `ip` is not `clwb`.
 */
size_t decode_clwb_length(const uint8_t* ip) {
    size_t idx = 0;
    bool has_66 = false;
    bool has_addr_override = false;

    while (idx < 6) {
        uint8_t b = ip[idx];
        if (b == 0x66) {
            has_66 = true;
            idx++;
        } else if (b == 0x67) {
            has_addr_override = true;
            idx++;
        } else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65) {
            idx++;
        } else {
            break;
        }
    }
    (void)has_addr_override;
    if (!has_66) {
        return 0;
    }

    if ((ip[idx] & 0xF0) == 0x40) {
        idx++; /* Optional REX prefix */
    }

    if (ip[idx] != 0x0F || ip[idx + 1] != 0xAE) {
        return 0;
    }
    idx += 2;

    uint8_t modrm = ip[idx++];
    uint8_t mod = (modrm >> 6) & 0x3;
    uint8_t reg = (modrm >> 3) & 0x7;
    uint8_t rm  = modrm & 0x7;

    /* `clwb` requires memory operand (mod != 3) and opcode extension /6. */
    if (mod == 3 || reg != 6) {
        return 0;
    }

    if (rm == 4) {
        uint8_t sib = ip[idx++];
        if (mod == 0 && (sib & 0x7) == 5) {
            idx += 4; /* disp32 with no base */
        }
    } else if (mod == 0 && rm == 5) {
        idx += 4; /* RIP-relative disp32 */
    }

    if (mod == 1) {
        idx += 1; /* disp8 */
    } else if (mod == 2) {
        idx += 4; /* disp32 */
    }

    return idx;
}

bool is_adx_opcode(const uint8_t* ip) {
    size_t idx = 0;
    bool has_66_or_f3 = false;
    while (idx < 4) {
        if (ip[idx] == 0x66 || ip[idx] == 0xF3) {
            has_66_or_f3 = true;
            idx++;
        } else {
            break;
        }
    }
    if (!has_66_or_f3) {
        return false;
    }
    if ((ip[idx] & 0xF0) == 0x40) {
        idx++;
    }
    return ip[idx] == 0x0F && ip[idx + 1] == 0x38 && ip[idx + 2] == 0xF6;
}

LONG CALLBACK bx_veh_handler(PEXCEPTION_POINTERS ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto* ip = reinterpret_cast<const uint8_t*>(ep->ContextRecord->Rip);
    if (g_config.enable_clwb_veh) {
        size_t len = decode_clwb_length(ip);
        if (len > 0) {
            uint64_t count = g_clwb_traps.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || (count & (count - 1)) == 0) {
                char msg[160];
                std::snprintf(msg, sizeof(msg),
                              "rip=0x%016llx len=%zu total_traps=%llu",
                              static_cast<unsigned long long>(ep->ContextRecord->Rip),
                              len,
                              static_cast<unsigned long long>(count));
                bx_log_emit(BX_LOG_INFO, "runtime_veh", "clwb_trapped", msg);
            }
            ep->ContextRecord->Rip += len;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    if (g_config.enable_adx_tripwire && is_adx_opcode(ip)) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "rip=0x%016llx bytes=%02x %02x %02x %02x %02x",
                      static_cast<unsigned long long>(ep->ContextRecord->Rip),
                      ip[0], ip[1], ip[2], ip[3], ip[4]);
        bx_log_emit(BX_LOG_ERROR, "runtime_veh", "adx_illegal_instruction", msg);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

void init_config_and_logging() {
    InitializeCriticalSection(&g_log_lock);
    g_log_lock_ready = true;

    char buf[512];
    if (GetEnvironmentVariableA(BX_ENV_PROFILE, buf, sizeof(buf)) > 0) {
        if (_stricmp(buf, "anaconda") == 0) {
            g_config.profile = BX_PROFILE_ANACONDA;
            g_config.title_memory_limit_bytes = 13500ULL * 1024ULL * 1024ULL;
        }
    }
    if (GetEnvironmentVariableA(BX_ENV_MEM_LIMIT_MB, buf, sizeof(buf)) > 0) {
        unsigned long long mb = std::strtoull(buf, nullptr, 10);
        if (mb >= 512) {
            g_config.title_memory_limit_bytes = mb * 1024ULL * 1024ULL;
        }
    }
    if (GetEnvironmentVariableA(BX_ENV_LOG_LEVEL, buf, sizeof(buf)) > 0) {
        if (_stricmp(buf, "error") == 0) g_config.log_level = BX_LOG_ERROR;
        else if (_stricmp(buf, "warn") == 0) g_config.log_level = BX_LOG_WARN;
        else if (_stricmp(buf, "trace") == 0) g_config.log_level = BX_LOG_TRACE;
    }
    if (GetEnvironmentVariableA(BX_ENV_TRACE_D3D, buf, sizeof(buf)) > 0 && buf[0] == '1') {
        g_config.trace_d3d = 1u;
    }
    if (GetEnvironmentVariableA(BX_ENV_LOG_FILE, buf, sizeof(buf)) > 0) {
        g_log_handle = CreateFileA(buf, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
}

} // namespace

extern "C" {

void bx_log_emit(bx_log_level_t level, const char* subsystem, const char* event, const char* detail) {
    if (level > g_config.log_level) {
        return;
    }
    static const char* kLevels[] = {"ERROR", "WARN", "INFO", "TRACE"};
    const char* lvl = (level <= BX_LOG_TRACE) ? kLevels[level] : "INFO";
    char line[768];
    int n = std::snprintf(
        line, sizeof(line),
        "{\"ts_ms\":%llu,\"tid\":%lu,\"level\":\"%s\",\"sub\":\"%s\",\"event\":\"%s\",\"detail\":\"%s\"}\n",
        static_cast<unsigned long long>(GetTickCount64()),
        static_cast<unsigned long>(GetCurrentThreadId()),
        lvl,
        subsystem ? subsystem : "unknown",
        event ? event : "event",
        detail ? detail : "");
    if (n > 0) {
        write_log_line(line, static_cast<size_t>(n < static_cast<int>(sizeof(line)) ? n : sizeof(line) - 1));
    }
}

void bx_log_unimpl_api(const char* dll_name, const char* symbol_name) {
    char detail[256];
    std::snprintf(detail, sizeof(detail), "%s!%s",
                  dll_name ? dll_name : "?",
                  symbol_name ? symbol_name : "?");
    bx_log_emit(BX_LOG_WARN, "stub", "unimpl_api", detail);
}

void bx_log_unimpl_vtable(const char* iface_name, uint32_t slot_index, const char* method_name) {
    char detail[256];
    std::snprintf(detail, sizeof(detail), "%s[%u]::%s",
                  iface_name ? iface_name : "?",
                  slot_index,
                  method_name ? method_name : "?");
    bx_log_emit(BX_LOG_WARN, "com_vtable", "unimpl_vtable", detail);
}

const bx_runtime_config_t* bx_get_runtime_config(void) {
    return &g_config;
}

int bx_veh_install(void) {
    if (!g_veh_handle) {
        g_veh_handle = AddVectoredExceptionHandler(1, bx_veh_handler);
    }
    return g_veh_handle != nullptr ? 1 : 0;
}

uint64_t bx_veh_get_clwb_trap_count(void) {
    return g_clwb_traps.load(std::memory_order_relaxed);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        init_config_and_logging();
        bx_veh_install();
        bx_log_emit(BX_LOG_INFO, "runtime_veh", "attached",
                    g_config.profile == BX_PROFILE_ANACONDA ? "profile=anaconda" : "profile=lockhart");
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_veh_handle) {
            RemoveVectoredExceptionHandler(g_veh_handle);
            g_veh_handle = nullptr;
        }
        if (g_log_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(g_log_handle);
            g_log_handle = INVALID_HANDLE_VALUE;
        }
        if (g_log_lock_ready) {
            DeleteCriticalSection(&g_log_lock);
            g_log_lock_ready = false;
        }
    }
    return TRUE;
}

} // extern "C"
