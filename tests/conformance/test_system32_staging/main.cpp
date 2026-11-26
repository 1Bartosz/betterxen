/*
 * #457 / #482: clean-room synthetic test for system32 shim staging by `bx-cli run`.
 *
 * GDK titles load `xgameruntime.dll` and `GameInput.dll` with LoadLibraryExW(...,
 * LOAD_LIBRARY_SEARCH_SYSTEM32), which does not search WINEPATH or the executable's folder.
 * In addition, Wine pre-populates `system32\gameinput.dll` with a builtin stub whose
 * `GameInputCreate` returns E_NOTIMPL.
 *
 * This test makes both loads and requires that BetterXen's shims answer them:
 *   - xgameruntime.dll loads from system32, exports QueryApiImpl, and matches
 *     LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR;
 *   - GameInput.dll loads from system32, exports GameInputCreate (@1 and by name), and
 *     GameInputCreate(&input) returns S_OK with a valid IUnknown pointer.
 *
 * `--expect-missing` is the negative control for an unstaged prefix: xgameruntime.dll must
 * fail to load, and GameInput.dll must either fail to load or fail GameInputCreate (Wine stub).
 * Exit 0 on pass, 1 on failure.
 */
#include <windows.h>
#include <unknwn.h>

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

bool path_in_system32(HMODULE m, const wchar_t* needle) {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(m, path, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return false;
    for (DWORD i = 0; i < n; ++i) path[i] = static_cast<wchar_t>(towlower(path[i]));
    std::printf("INFO: loaded from %ls\n", path);
    return std::wcsstr(path, needle) != nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    const bool expect_missing = argc > 1 && std::strcmp(argv[1], "--expect-missing") == 0;

    HMODULE sys = LoadLibraryExW(L"xgameruntime.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (expect_missing) {
        check(sys == nullptr, "unstaged: LOAD_LIBRARY_SEARCH_SYSTEM32 xgameruntime.dll load fails");
        if (sys) FreeLibrary(sys);

        HMODULE gi = LoadLibraryExW(L"GameInput.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        bool gi_unusable = (gi == nullptr);
        if (gi) {
            using CreateFn = HRESULT (WINAPI*)(void**);
            auto fn = reinterpret_cast<CreateFn>(
                reinterpret_cast<void*>(GetProcAddress(gi, "GameInputCreate")));
            void* obj = nullptr;
            HRESULT hr = fn ? fn(&obj) : E_FAIL;
            if (SUCCEEDED(hr) && obj) {
                static_cast<IUnknown*>(obj)->Release();
            } else {
                gi_unusable = true;
            }
            FreeLibrary(gi);
        }
        check(gi_unusable, "unstaged: GameInput.dll is absent or Wine stub returns failure");
    } else {
        if (check(sys != nullptr, "LOAD_LIBRARY_SEARCH_SYSTEM32 loads xgameruntime.dll")) {
            check(path_in_system32(sys, L"\\windows\\system32\\xgameruntime.dll"),
                  "xgameruntime.dll module path is under system32");
            check(GetProcAddress(sys, "QueryApiImpl") != nullptr,
                  "module exports QueryApiImpl (BetterXen shim)");
            HMODULE both = LoadLibraryExW(
                L"xgameruntime.dll", nullptr,
                LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
            check(both == sys, "SYSTEM32 | APPLICATION_DIR search returns the same module");
            if (both) FreeLibrary(both);
            FreeLibrary(sys);
        }

        HMODULE gi = LoadLibraryExW(L"GameInput.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (check(gi != nullptr, "LOAD_LIBRARY_SEARCH_SYSTEM32 loads GameInput.dll")) {
            check(path_in_system32(gi, L"\\windows\\system32\\gameinput.dll"),
                  "GameInput.dll module path is under system32");
            FARPROC ord1 = GetProcAddress(gi, MAKEINTRESOURCEA(1));
            FARPROC named = GetProcAddress(gi, "GameInputCreate");
            check(ord1 != nullptr && ord1 == named,
                  "GameInput.dll exports GameInputCreate at ordinal 1");
            using CreateFn = HRESULT (WINAPI*)(void**);
            auto create = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(named));
            void* input = nullptr;
            HRESULT hr = create ? create(&input) : E_FAIL;
            if (check(hr == S_OK && input != nullptr,
                      "GameInputCreate returns S_OK (BetterXen shim, not Wine builtin stub)")) {
                static_cast<IUnknown*>(input)->Release();
            }
            FreeLibrary(gi);
        }
    }
    std::printf("SUMMARY: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
