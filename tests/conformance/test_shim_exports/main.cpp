#include <windows.h>
#include <cstdio>

struct ShimExpectation {
    const char* dll;
    const char* symbol; /* Or nullptr to check ordinal 1 */
};

static const ShimExpectation kChecks[] = {
    {"bx_runtime_veh.dll", "bx_log_emit"},
    {"bx_runtime_veh.dll", "bx_veh_install"},
    {"xmem.dll", "XMemAlloc"},
    {"xmem.dll", "XMemMapPhysicalPagesScatter"},
    {"xmem.dll", "bx_xmem_get_internal_stats"},
    {"XCRDAPI.dll", "DStorageGetFactory"},
    {"d3d12_xs.dll", "D3D12XboxCreateDevice"},
    {"d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX"},
    {"d3d12_xs.dll", "D3D12SerializeVersionedRootSignature"},
    {"d3d12_xs.dll", "D3D12CreateRootSignatureDeserializer"},
    {"xg_xs.dll", "XGCreateTextureComputer"},
    {"xg_xs.dll", "XGSuggestSwizzleMode"},
    {"xgameruntime.dll", "XGameRuntimeInitialize"},
    {"xgameruntime.dll", "XSystemGetDeviceType"},
    {"AcpHal.dll", "AcpHalCreate"},
    {"AcpHal.dll", "ApuAlloc"},
    {"xapu.dll", "XApuConnect"},
    {"chad.dll", "XDspActivate"},
    {"GameInput.dll", nullptr}, /* Ordinal 1 */
    {"api-ms-win-core-psm-appnotify-l1-1-0.dll", "RegisterAppStateChangeNotification"},
    {"api-ms-win-core-psm-appnotify-l1-1-1.dll", "RegisterAppConstrainedChangeNotification"},
    {"ext-ms-win-rtcore-ntuser-message-l1-1-0.dll", "TranslateMessage"},
    {"ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0.dll", "PeekMessageA"},
    {"ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "ShowWindow"},
    {"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "CreateWindowExA"},
};

int main() {
    int failures = 0;
    for (const auto& check : kChecks) {
        HMODULE mod = LoadLibraryA(check.dll);
        if (!mod) {
            std::fprintf(stderr, "FAIL: LoadLibraryA(%s) err=%lu\n", check.dll, GetLastError());
            failures++;
            continue;
        }
        FARPROC proc = check.symbol
            ? GetProcAddress(mod, check.symbol)
            : GetProcAddress(mod, MAKEINTRESOURCEA(1));
        if (!proc) {
            std::fprintf(stderr, "FAIL: GetProcAddress(%s, %s) err=%lu\n",
                         check.dll, check.symbol ? check.symbol : "@1", GetLastError());
            failures++;
        }
    }
    if (failures > 0) {
        std::fprintf(stderr, "FAIL: %d shim export check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS: test_shim_exports (%zu checks verified)\n", sizeof(kChecks) / sizeof(kChecks[0]));
    return 0;
}
