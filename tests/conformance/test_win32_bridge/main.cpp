#include <windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

constexpr UINT kMessage = WM_APP + 1;
constexpr WPARAM kPayload = 42;
decltype(&DefWindowProcA) g_def_window_proc;
bool g_received_message = false;
std::atomic<bool> g_callback_called{false};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kMessage) {
        g_received_message = wparam == kPayload;
        return 0;
    }
    return g_def_window_proc(window, message, wparam, lparam);
}

void CALLBACK notification_callback(BOOLEAN, PVOID) {
    g_callback_called.store(true);
}

HMODULE load_bridge(const wchar_t* dll) {
    wchar_t executable[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!length || length == MAX_PATH) {
        return nullptr;
    }
    const std::wstring path(executable, length);
    const auto expected = path.substr(0, path.find_last_of(L"\\/") + 1) + dll;
    HMODULE module = LoadLibraryW(expected.c_str());
    wchar_t actual[MAX_PATH];
    const DWORD actual_length = module ? GetModuleFileNameW(module, actual, MAX_PATH) : 0;
    if (!actual_length || actual_length == MAX_PATH ||
        CompareStringOrdinal(expected.c_str(), -1, actual, -1, TRUE) != CSTR_EQUAL) {
        std::fwprintf(stderr, L"FAIL: could not load built bridge %ls (error %lu)\n",
                      expected.c_str(), GetLastError());
        return nullptr;
    }
    return module;
}

template<typename Function>
Function lookup(HMODULE module, const char* name) {
    return std::bit_cast<Function>(GetProcAddress(module, name));
}

bool test_window() {
    const auto window = load_bridge(L"ext-ms-win-rtcore-ntuser-window-l1-1-0.dll");
    const auto window_ansi = load_bridge(L"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll");
    const auto message = load_bridge(L"ext-ms-win-rtcore-ntuser-message-l1-1-0.dll");
    const auto message_ansi = load_bridge(L"ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0.dll");
    if (!window || !window_ansi || !message || !message_ansi) {
        return false;
    }

    const auto show = lookup<decltype(&ShowWindow)>(window, "ShowWindow");
    const auto create = lookup<decltype(&CreateWindowExA)>(window_ansi, "CreateWindowExA");
    const auto register_class = lookup<decltype(&RegisterClassExA)>(window_ansi, "RegisterClassExA");
    g_def_window_proc = lookup<decltype(&DefWindowProcA)>(window_ansi, "DefWindowProcA");
    const auto translate = lookup<decltype(&TranslateMessage)>(message, "TranslateMessage");
    const auto peek = lookup<decltype(&PeekMessageA)>(message_ansi, "PeekMessageA");
    const auto dispatch = lookup<decltype(&DispatchMessageA)>(message_ansi, "DispatchMessageA");
    if (!show || !create || !register_class || !g_def_window_proc || !translate || !peek || !dispatch) {
        return false;
    }

    WNDCLASSEXA cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = window_proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = "BetterXenBridgeConformance";
    if (!register_class(&cls)) {
        return false;
    }
    const HWND hwnd = create(0, cls.lpszClassName, "BetterXen Win32 Bridge", WS_OVERLAPPEDWINDOW,
                             0, 0, 100, 100, nullptr, nullptr, cls.hInstance, nullptr);
    if (hwnd) {
        show(hwnd, SW_HIDE);
        if (!IsWindowVisible(hwnd) && PostMessageA(hwnd, kMessage, kPayload, 0)) {
            MSG msg{};
            while (!g_received_message && peek(&msg, hwnd, 0, 0, PM_REMOVE)) {
                translate(&msg);
                dispatch(&msg);
            }
        }
    }
    const bool destroyed = hwnd && DestroyWindow(hwnd);
    const bool unregistered = UnregisterClassA(cls.lpszClassName, cls.hInstance);
    return g_received_message && destroyed && unregistered;
}

using RegisterNotification = ULONG (WINAPI*)(decltype(&notification_callback), PVOID, PVOID*);
using UnregisterNotification = VOID (WINAPI*)(PVOID);

bool test_psm() {
    const HMODULE state_dll = load_bridge(L"api-ms-win-core-psm-appnotify-l1-1-0.dll");
    const HMODULE constrained_dll = load_bridge(L"api-ms-win-core-psm-appnotify-l1-1-1.dll");
    if (!state_dll || !constrained_dll) {
        return false;
    }
    const auto register_state = lookup<RegisterNotification>(state_dll, "RegisterAppStateChangeNotification");
    const auto unregister_state = lookup<UnregisterNotification>(state_dll, "UnregisterAppStateChangeNotification");
    const auto register_constrained = lookup<RegisterNotification>(
        constrained_dll, "RegisterAppConstrainedChangeNotification");
    const auto unregister_constrained = lookup<UnregisterNotification>(
        constrained_dll, "UnregisterAppConstrainedChangeNotification");
    if (!register_state || !unregister_state || !register_constrained || !unregister_constrained) {
        return false;
    }

    PVOID token = reinterpret_cast<PVOID>(1);
    if (register_state(nullptr, nullptr, &token) != ERROR_INVALID_PARAMETER || token ||
        register_constrained(notification_callback, nullptr, nullptr) != ERROR_INVALID_PARAMETER) {
        return false;
    }

    constexpr size_t kRegistrationsPerThread = 16;
    struct Tokens {
        PVOID state{};
        PVOID constrained{};
    };
    std::array<std::array<Tokens, kRegistrationsPerThread>, 4> tokens{};
    std::array<std::thread, tokens.size()> workers;
    std::atomic<bool> success{true};
    for (size_t i = 0; i < workers.size(); ++i) {
        workers[i] = std::thread([&, i] {
            for (auto& entry : tokens[i]) {
                const auto state_status = register_state(notification_callback, &entry, &entry.state);
                const auto constrained_status = register_constrained(
                    notification_callback, &entry, &entry.constrained);
                if (state_status != ERROR_SUCCESS || constrained_status != ERROR_SUCCESS ||
                    !entry.state || !entry.constrained) {
                    success.store(false);
                }
                unregister_constrained(entry.state);
                unregister_state(entry.state);
                unregister_state(entry.state);
                unregister_constrained(entry.constrained);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    std::unordered_set<PVOID> seen;
    for (const auto& entries : tokens) {
        for (const auto& entry : entries) {
            if (!seen.insert(entry.state).second || !seen.insert(entry.constrained).second) {
                return false;
            }
        }
    }
    return success.load() && !g_callback_called.load();
}

} // namespace

int main() {
    if (!test_window()) {
        std::fprintf(stderr, "FAIL: Win32 bridge window or message pump (error %lu)\n", GetLastError());
        return 1;
    }
    if (!test_psm()) {
        std::fprintf(stderr, "FAIL: PSM bridge registration lifecycle\n");
        return 2;
    }
    std::puts("PASS: test_win32_bridge (hidden window, message pump, 128 PSM registrations)");
    return 0;
}
