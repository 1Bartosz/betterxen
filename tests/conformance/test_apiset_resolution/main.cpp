/*
 * #437: API-set import resolution fixture.
 *
 * Statically imports one export from each of the six win32_bridge shim DLLs
 * (api-ms-win-core-psm-appnotify-l1-1-{0,1}, ext-ms-win-rtcore-ntuser-*) and
 * reports, per name, which module actually served it:
 *
 *   shim   - the BetterXen file of that name next to this EXE is the loaded module.
 *   host   - a module that is not that file answers the name (for example Wine's
 *            own API-set handling).
 *   absent - no module is loaded under that name.
 *
 * `bound_to` is the module that owns the address the loader wrote into the first
 * import address table slot for that name, which is the module that serves a call
 * made through the static import. The check is made through the PE import directory, not LoadLibrary,
 * which is how real titles reach these names (see #437).
 *
 * Usage: test_apiset_resolution [--expect host|shim]
 *   --expect host (default): pass when all six names are served by the host.
 *   --expect shim:           pass when all six names are served by BetterXen shim files.
 * Exit 0 when every name matches the expectation, 1 otherwise, 2 on bad arguments.
 */
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#define BX_IMPORT extern "C" __declspec(dllimport)
BX_IMPORT ULONG WINAPI RegisterAppStateChangeNotification(PVOID callback, PVOID context, PVOID* out_reg);
BX_IMPORT ULONG WINAPI RegisterAppConstrainedChangeNotification(PVOID callback, PVOID context, PVOID* out_reg);

namespace {

struct ApiSetName {
    const char* dll;
    const char* export_name;
};

/* One export per shim DLL. The calls in keep_imports() make the linker emit each import. */
const ApiSetName kNames[] = {
    {"api-ms-win-core-psm-appnotify-l1-1-0.dll", "RegisterAppStateChangeNotification"},
    {"api-ms-win-core-psm-appnotify-l1-1-1.dll", "RegisterAppConstrainedChangeNotification"},
    {"ext-ms-win-rtcore-ntuser-message-l1-1-0.dll", "TranslateMessage"},
    {"ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0.dll", "PeekMessageA"},
    {"ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "ShowWindow"},
    {"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "CreateWindowExA"},
};
constexpr size_t kNameCount = sizeof(kNames) / sizeof(kNames[0]);

enum class Served { Shim, Host, Absent };

const char* served_name(Served s) {
    switch (s) {
        case Served::Shim: return "shim";
        case Served::Host: return "host";
        default: return "absent";
    }
}

const IMAGE_IMPORT_DESCRIPTOR* find_import(const char* dll) {
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) {
        return nullptr;
    }
    auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) == 0) {
            return desc;
        }
    }
    return nullptr;
}

/* The address the loader wrote into the first import address table slot for `dll`. */
const void* first_bound_address(const IMAGE_IMPORT_DESCRIPTOR* desc) {
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
    auto* iat = reinterpret_cast<const uintptr_t*>(base + desc->FirstThunk);
    return reinterpret_cast<const void*>(iat[0]);
}

/* Referencing each export makes the linker keep the import. main() calls this only for an impossible argc. */
__attribute__((noinline)) void keep_imports() {
    const void* const sink[] = {
        reinterpret_cast<const void*>(&RegisterAppStateChangeNotification),
        reinterpret_cast<const void*>(&RegisterAppConstrainedChangeNotification),
        reinterpret_cast<const void*>(&TranslateMessage),
        reinterpret_cast<const void*>(&PeekMessageA),
        reinterpret_cast<const void*>(&ShowWindow),
        reinterpret_cast<const void*>(&CreateWindowExA),
    };
    for (const void* p : sink) {
        std::printf("%p\n", p);
    }
}

/* Lower-cased full path of a module; empty when it cannot be read. */
bool path_of(HMODULE mod, char* out, DWORD out_len) {
    out[0] = '\0';
    if (!mod) {
        return false;
    }
    DWORD n = GetModuleFileNameA(mod, out, out_len);
    if (n == 0 || n >= out_len) {
        out[0] = '\0';
        return false;
    }
    CharLowerBuffA(out, n);
    return true;
}

/* Directory part of `path` (up to the last backslash), or false when there is none. */
bool dir_len_of(const char* path, size_t* len) {
    const char* slash = std::strrchr(path, '\\');
    if (!slash) {
        return false;
    }
    *len = static_cast<size_t>(slash - path);
    return true;
}

/* Paths contain backslashes; escape them (and quotes) so each line stays valid JSON. */
void json_escape(const char* in, char* out, size_t out_len) {
    size_t o = 0;
    for (; *in && o + 2 < out_len; ++in) {
        if (*in == '\\' || *in == '"') {
            out[o++] = '\\';
        }
        out[o++] = *in;
    }
    out[o] = '\0';
}

int g_fail = 0;
int g_seq = 0;

void checkpoint(const char* stage, bool ok, const char* served, bool imported, const char* name_path,
                const char* bound_path, const char* detail) {
    char np[1024];
    char bp[1024];
    json_escape(name_path, np, sizeof(np));
    json_escape(bound_path, bp, sizeof(bp));
    g_seq++;
    if (!ok) {
        g_fail++;
    }
    std::printf("{\"bx_checkpoint\":\"test_apiset_resolution\",\"seq\":%d,\"stage\":\"%s\",\"status\":\"%s\","
                "\"served_by\":\"%s\",\"statically_imported\":%s,\"name_module\":\"%s\",\"bound_to\":\"%s\",\"detail\":\"%s\"}\n",
                g_seq, stage, ok ? "pass" : "fail", served, imported ? "true" : "false", np, bp, detail);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1000) {
        keep_imports();
    }
    Served expect = Served::Host;
    if (argc == 3 && std::strcmp(argv[1], "--expect") == 0) {
        if (std::strcmp(argv[2], "shim") == 0) {
            expect = Served::Shim;
        } else if (std::strcmp(argv[2], "host") != 0) {
            std::fprintf(stderr, "usage: %s [--expect host|shim]\n", argv[0]);
            return 2;
        }
    } else if (argc != 1) {
        std::fprintf(stderr, "usage: %s [--expect host|shim]\n", argv[0]);
        return 2;
    }

    char exe[MAX_PATH] = "";
    const bool exe_ok = path_of(GetModuleHandleA(nullptr), exe, sizeof(exe));
    size_t exe_dir = 0;
    const bool have_exe_dir = exe_ok && dir_len_of(exe, &exe_dir);

    int matched = 0;
    for (const ApiSetName& n : kNames) {
        const IMAGE_IMPORT_DESCRIPTOR* import = find_import(n.dll);
        const bool imported = import != nullptr;
        HMODULE name_mod = GetModuleHandleA(n.dll);
        char name_path[MAX_PATH] = "";
        const bool name_ok = path_of(name_mod, name_path, sizeof(name_path));

        HMODULE bound_mod = nullptr;
        char bound_path[MAX_PATH] = "";
        bool bound_ok = false;
        if (import
            && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCSTR>(first_bound_address(import)), &bound_mod)) {
            bound_ok = path_of(bound_mod, bound_path, sizeof(bound_path));
        }

        /* The shim serves the name only when the loaded module is that file next to this EXE. */
        Served served = Served::Absent;
        if (name_mod && name_ok && have_exe_dir) {
            size_t dir = 0;
            const char* slash = std::strrchr(name_path, '\\');
            const bool same_dir = dir_len_of(name_path, &dir) && dir == exe_dir
                               && std::memcmp(name_path, exe, exe_dir) == 0;
            served = same_dir && slash && _stricmp(slash + 1, n.dll) == 0 ? Served::Shim : Served::Host;
        }

        const bool lookup_ok = have_exe_dir && name_mod && name_ok && bound_ok;
        const bool ok = imported && lookup_ok && served == expect;
        if (ok) {
            matched++;
        }
        char detail[160];
        if (!imported) {
            std::snprintf(detail, sizeof(detail), "name is not in the static import directory");
        } else if (!have_exe_dir) {
            std::snprintf(detail, sizeof(detail), "%s: failed to resolve executable directory", n.export_name);
        } else if (!name_mod) {
            std::snprintf(detail, sizeof(detail), "%s: GetModuleHandleA returned null (expected %s)", n.export_name,
                          served_name(expect));
        } else if (!name_ok) {
            std::snprintf(detail, sizeof(detail), "%s: failed to resolve path for name module", n.export_name);
        } else if (!bound_ok) {
            std::snprintf(detail, sizeof(detail), "%s: failed to resolve owning module for bound IAT entry",
                          n.export_name);
        } else {
            std::snprintf(detail, sizeof(detail), "%s: served by %s, expected %s", n.export_name, served_name(served),
                          served_name(expect));
        }
        checkpoint(n.dll, ok, served_name(served), imported, name_ok ? name_path : "", bound_ok ? bound_path : "",
                   detail);
    }

    const bool ok = g_fail == 0;
    std::printf("{\"bx_summary\":\"test_apiset_resolution\",\"names\":%zu,\"matched\":%d,\"expected\":\"%s\",\"result\":\"%s\"}\n",
                kNameCount, matched, served_name(expect), ok ? "pass" : "fail");
    std::printf("%s: test_apiset_resolution (%d/%zu names served by %s)\n", ok ? "PASS" : "FAIL", matched, kNameCount,
                served_name(expect));
    std::fflush(stdout);
    return ok ? 0 : 1;
}
