/*
 * #457: clean-room synthetic test for system32 shim staging by `bx-cli run`.
 *
 * GDK titles load the runtime with LoadLibraryExW(L"xgameruntime.dll", NULL,
 * LOAD_LIBRARY_SEARCH_SYSTEM32), which does not search WINEPATH or the executable's folder.
 * This test makes that load and requires that BetterXen's shim answers it:
 *   - the module loads, from a path under system32;
 *   - it exports QueryApiImpl (a BetterXen xgameruntime export, absent from Wine builtins);
 *   - LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR gives the same module.
 *
 * `--expect-missing` is the negative control for an unstaged prefix: the load must fail.
 * Exit 0 on pass, 1 on failure.
 */
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

int g_checks = 0;
int g_failures = 0;

bool check(bool ok, const char* what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::printf("PASS: %s\n", what);
    }
    return ok;
}

bool path_in_system32(HMODULE m) {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(m, path, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return false;
    for (DWORD i = 0; i < n; ++i) path[i] = static_cast<wchar_t>(towlower(path[i]));
    std::printf("INFO: loaded from %ls\n", path);
    return std::wcsstr(path, L"\\windows\\system32\\xgameruntime.dll") != nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    const bool expect_missing = argc > 1 && std::strcmp(argv[1], "--expect-missing") == 0;

    HMODULE sys = LoadLibraryExW(L"xgameruntime.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (expect_missing) {
        check(sys == nullptr, "unstaged: LOAD_LIBRARY_SEARCH_SYSTEM32 load fails");
        if (sys) FreeLibrary(sys);
    } else if (check(sys != nullptr, "LOAD_LIBRARY_SEARCH_SYSTEM32 loads xgameruntime.dll")) {
        check(path_in_system32(sys), "module path is under system32");
        check(GetProcAddress(sys, "QueryApiImpl") != nullptr, "module exports QueryApiImpl (BetterXen shim)");
        HMODULE both = LoadLibraryExW(L"xgameruntime.dll", nullptr,
                                      LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
        check(both == sys, "SYSTEM32 | APPLICATION_DIR search returns the same module");
        if (both) FreeLibrary(both);
        FreeLibrary(sys);
    }
    std::printf("SUMMARY: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
