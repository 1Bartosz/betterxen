/*
 * M2-C2 (#465): clean-room synthetic conformance test for d3d12_xs.dll's presentation bridge.
 *
 * Renders FRAMES frames into the bridge's back buffers (a clear, then a shader draw limited by a
 * scissor rectangle), presents each one through the bridge, and compares every pixel of every
 * frame, read back from the back buffer before it was presented, with values computed on the CPU
 * from the pixel coordinates and the frame number alone.
 *
 * What this shows: the content handed to Present. It does not capture the window, so it does not
 * show what reached the screen. The bridge interfaces are BetterXen's own (presentx.h); nothing
 * here is evidence about the console's PresentX or about commercial software.
 *
 * Exit codes: 0 pass; otherwise the code of the first stage that failed (see Stage).
 * Negative controls, each of which must fail in the named stage. Pass --inject <name>, or, because
 * `bx-cli run` forwards no guest arguments, set BX_PRESENTX_INJECT=<name> and add --inherit-env:
 *   --inject bad-window         the main swapchain gets a destroyed window    -> create  (12)
 *   --inject skip-draw          the shader draw is left out of one frame      -> pixels  (15)
 *   --inject wrong-expectation  the CPU expects a blue value that is off by 1 -> pixels  (15)
 */
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bx_log.h"
#include "presentx.h"

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(PVOID params, REFIID riid, void** ppv);

namespace {

constexpr UINT kWidth = 100;  /* 400-byte rows, so the 256-byte copy pitch has padding */
constexpr UINT kHeight = 60;
constexpr UINT kBuffers = 3;
constexpr UINT kFrames = 12;  /* every back buffer is reused several times */
constexpr UINT kSkippedFrame = 5;
constexpr D3D12_RECT kRect = {7, 5, 71, 43}; /* asymmetric, so flips and transposes show */
constexpr UINT kRectPixels = (71 - 7) * (43 - 5);
constexpr HRESULT kTimedOut = static_cast<HRESULT>(0x800705B4u);

enum Stage { kSetup = 10, kContract = 11, kCreate = 12, kPipeline = 13, kRender = 14, kPixels = 15, kSync = 16, kLifetime = 17 };

const char* stage_name(Stage s) {
    switch (s) {
    case kSetup: return "setup";
    case kContract: return "contract";
    case kCreate: return "create";
    case kPipeline: return "pipeline";
    case kRender: return "render";
    case kPixels: return "pixels";
    case kSync: return "sync";
    case kLifetime: return "lifetime";
    }
    return "?";
}

Stage g_stage = kSetup;
int g_first_failed_stage = 0;
int g_checks = 0;
int g_failures = 0;
int g_skipped = 0;
LONGLONG g_log_start = 0; /* log records before this offset belong to earlier runs */

__attribute__((format(gnu_printf, 2, 3)))
bool check(bool ok, const char* fmt, ...) {
    g_checks++;
    if (!ok) {
        g_failures++;
        if (!g_first_failed_stage) g_first_failed_stage = g_stage;
        char msg[320];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        std::printf("FAIL [%s]: %s\n", stage_name(g_stage), msg);
    }
    return ok;
}

/* For failures that make later stages meaningless. The process exits; nothing is unwound. */
#define REQUIRE(...)                  \
    do {                              \
        if (!check(__VA_ARGS__)) {    \
            finish();                 \
        }                             \
    } while (0)

[[noreturn]] void finish() {
    if (g_failures) {
        std::printf("FAIL: test_d3d12_presentx (%d of %d checks failed; first failing stage: %s)\n", g_failures, g_checks,
                    stage_name(static_cast<Stage>(g_first_failed_stage)));
    } else {
        std::printf("PASS: test_d3d12_presentx (%d checks passed, %d skipped)\n", g_checks, g_skipped);
    }
    std::fflush(stdout);
    ExitProcess(static_cast<UINT>(g_first_failed_stage));
}

unsigned long ul(HRESULT hr) { return static_cast<unsigned long>(hr); }

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

/* The reference count an object reports to its holder, without changing it. */
ULONG ref_count(IUnknown* obj) {
    obj->AddRef();
    return obj->Release();
}

bool vtable_in_shim(IUnknown* obj) {
    HMODULE owner = nullptr;
    void* vtable = *reinterpret_cast<void**>(obj);
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(vtable), &owner)) {
        return false;
    }
    return owner != nullptr && owner == GetModuleHandleA("d3d12_xs.dll");
}

void pump(DWORD ms) {
    const ULONGLONG start = GetTickCount64();
    do {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(5);
    } while (GetTickCount64() - start < ms);
}

HWND make_window() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "BxPresentXTest";
        registered = RegisterClassA(&wc) != 0;
    }
    HWND w = CreateWindowA("BxPresentXTest", "BetterXen PresentX test", WS_POPUP, 40, 40, kWidth * 2, kHeight * 2, nullptr,
                           nullptr, GetModuleHandleA(nullptr), nullptr);
    if (w) {
        ShowWindow(w, SW_SHOWNOACTIVATE);
        pump(10);
    }
    return w;
}

template <typename Fn>
Fn proc(const char* dll, const char* name) {
    HMODULE m = LoadLibraryA(dll);
    return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

bool wait_fence(ID3D12Fence* fence, UINT64 value, HANDLE event) {
    if (fence->GetCompletedValue() < value) {
        if (FAILED(fence->SetEventOnCompletion(value, event))) return false;
        if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) return false;
    }
    const UINT64 done = fence->GetCompletedValue();
    return done >= value && done != UINT64_MAX;
}

const BxPresentDesc kDesc = {kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, kBuffers};

/* ---- Expected image: from the coordinates and the frame number only ------------------------- */

struct Rgba {
    uint8_t r, g, b, a;
};

Rgba clear_colour(UINT frame) { return {static_cast<uint8_t>(10 + frame), 200, 30, 255}; }
uint32_t pattern_blue(UINT frame) { return 100 + 8 * frame; }

Rgba expected_pixel(UINT x, UINT y, UINT frame, int blue_bias) {
    const bool inside = static_cast<LONG>(x) >= kRect.left && static_cast<LONG>(x) < kRect.right &&
                        static_cast<LONG>(y) >= kRect.top && static_cast<LONG>(y) < kRect.bottom;
    if (!inside) return clear_colour(frame);
    return {static_cast<uint8_t>(x), static_cast<uint8_t>(y),
            static_cast<uint8_t>(static_cast<int>(pattern_blue(frame)) + blue_bias), 255};
}

UINT count_mismatches(const uint8_t* data, UINT row_pitch, UINT frame, int blue_bias, bool report) {
    UINT bad = 0;
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const uint8_t* p = data + static_cast<size_t>(y) * row_pitch + x * 4;
            const Rgba e = expected_pixel(x, y, frame, blue_bias);
            if (p[0] != e.r || p[1] != e.g || p[2] != e.b || p[3] != e.a) {
                if (report && bad == 0) {
                    std::printf("  frame %u first mismatch at (%u,%u): got %u,%u,%u,%u expected %u,%u,%u,%u\n", frame, x, y,
                                p[0], p[1], p[2], p[3], e.r, e.g, e.b, e.a);
                }
                ++bad;
            }
        }
    }
    return bad;
}

const char kShader[] =
    "cbuffer C : register(b0) { uint blue; };\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "  uint2 xy = (uint2)pos.xy;\n"
    "  return float4((xy.x & 255) / 255.0, (xy.y & 255) / 255.0, blue / 255.0, 1.0);\n"
    "}\n";

ID3DBlob* compile(const char* entry, const char* target) {
    auto fn = proc<pD3DCompile>("d3dcompiler_47.dll", "D3DCompile");
    if (!check(fn != nullptr, "d3dcompiler_47.dll!D3DCompile not available")) return nullptr;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = fn(kShader, sizeof(kShader) - 1, "presentx", nullptr, nullptr, entry, target, 0, 0, &code, &errors);
    check(SUCCEEDED(hr) && code, "D3DCompile(%s) hr=0x%08lx %s", entry, ul(hr),
          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    release(errors);
    return code;
}

/* ---- Log evidence --------------------------------------------------------------------------- */

bool log_path(char (&path)[MAX_PATH]) {
    DWORD len = GetEnvironmentVariableA("BX_LOG_FILE", path, sizeof(path));
    return len > 0 && len < sizeof(path);
}

LONGLONG log_size() {
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!log_path(path) || !GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return 0;
    return (static_cast<LONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

bool log_has(const char* event, const char* needle) {
    char path[MAX_PATH];
    if (!log_path(path)) return false;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(f, &size) && size.QuadPart > g_log_start && size.QuadPart < (64ll << 20)) {
        auto* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(size.QuadPart) + 1));
        DWORD got = 0;
        if (buf && ReadFile(f, buf, static_cast<DWORD>(size.QuadPart), &got, nullptr)) {
            buf[got] = '\0';
            char event_key[96];
            std::snprintf(event_key, sizeof(event_key), "\"event\":\"%s\"", event);
            for (char* line = buf + g_log_start; line && *line && !found;) {
                char* end = std::strchr(line, '\n');
                if (end) *end = '\0';
                found = std::strstr(line, event_key) && std::strstr(line, needle);
                line = end ? end + 1 : nullptr;
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(f);
    return found;
}

/* ---- Lifetime: the bridge against a swapchain made with host DXGI alone ---------------------- */

/* Attached to a swapchain as private data; it stays referenced for as long as that data lives. */
struct Witness final : public IUnknown {
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!IsEqualGUID(riid, __uuidof(IUnknown))) return E_NOINTERFACE;
        *ppv = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(InterlockedDecrement(&refs)); }
};
const GUID kWitnessKey = {0xb3b13f0b, 0xe98a, 0x4a4f, {0x82, 0xc2, 0xc1, 0x30, 0xc3, 0x71, 0xcf, 0xc8}};

struct HostDxgi {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGIFactory4* factory = nullptr;
};

HRESULT host_swapchain(const HostDxgi& host, HWND window, IDXGISwapChain1** out) {
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = kWidth;
    sd.Height = kHeight;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    return host.factory->CreateSwapChainForHwnd(host.queue, window, &sd, nullptr, nullptr, out);
}

bool open_host(HostDxgi& host) {
    using CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto create_device = proc<CreateDevice>("d3d12.dll", "D3D12CreateDevice");
    auto create_factory = proc<CreateFactory>("dxgi.dll", "CreateDXGIFactory1");
    if (!create_device || !create_factory) return false;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    return SUCCEEDED(create_device(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&host.device))) &&
           SUCCEEDED(host.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&host.queue))) &&
           SUCCEEDED(create_factory(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&host.factory)));
}

struct Retention {
    ULONG holder_refs = 0; /* swapchain references seen while its one owner (app or bridge) holds it */
    LONG retained = 0;     /* witnesses still referenced after every create/release cycle */
};

constexpr int kProbeCycles = 4;

Retention probe_host(const HostDxgi& host, Witness (&witness)[kProbeCycles]) {
    Retention result;
    for (int i = 0; i < kProbeCycles; ++i) {
        HWND window = make_window();
        IDXGISwapChain1* swap = nullptr;
        HRESULT hr = window ? host_swapchain(host, window, &swap) : E_FAIL;
        if (!check(SUCCEEDED(hr), "control: host swapchain %d hr=0x%08lx", i, ul(hr))) return result;
        result.holder_refs = ref_count(swap);
        check(SUCCEEDED(swap->SetPrivateDataInterface(kWitnessKey, &witness[i])) && witness[i].refs == 2,
              "control: witness %d must be referenced by the swapchain (refs=%ld)", i, witness[i].refs);
        check(SUCCEEDED(swap->Present(0, 0)), "control: host Present %d", i);
        pump(50);
        swap->Release();
        DestroyWindow(window);
        pump(50);
    }
    pump(300);
    for (const Witness& w : witness) result.retained += w.refs > 1;
    return result;
}

Retention probe_bridge(IBxPresentQueue* bridge, Witness (&witness)[kProbeCycles]) {
    Retention result;
    for (int i = 0; i < kProbeCycles; ++i) {
        HWND window = make_window();
        IBxPresentSwapChain* chain = nullptr;
        HRESULT hr = bridge->CreateSwapChain(window, &kDesc, &chain);
        if (!check(SUCCEEDED(hr) && chain, "bridge swapchain %d hr=0x%08lx", i, ul(hr))) return result;
        IDXGISwapChain1* swap = nullptr;
        hr = chain->GetHostSwapChain(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swap));
        if (!check(SUCCEEDED(hr) && swap, "GetHostSwapChain %d hr=0x%08lx", i, ul(hr))) return result;
        result.holder_refs = ref_count(swap) - 1; /* minus the reference taken just above */
        check(SUCCEEDED(swap->SetPrivateDataInterface(kWitnessKey, &witness[i])) && witness[i].refs == 2,
              "witness %d must be referenced by the bridge's swapchain (refs=%ld)", i, witness[i].refs);
        swap->Release();
        hr = chain->Present(0, 0);
        check(SUCCEEDED(hr), "bridge Present %d hr=0x%08lx", i, ul(hr));
        pump(50);
        check(chain->Release() == 0, "bridge swapchain %d still referenced after its final Release", i);
        DestroyWindow(window);
        pump(50);
    }
    pump(300);
    for (const Witness& w : witness) result.retained += w.refs > 1;
    return result;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    char inject[32] = "";
    if (GetEnvironmentVariableA("BX_PRESENTX_INJECT", inject, sizeof(inject)) >= sizeof(inject)) {
        std::strcpy(inject, "?"); /* too long to be a control name */
    }
    if (argc == 3 && !std::strcmp(argv[1], "--inject")) std::snprintf(inject, sizeof(inject), "%s", argv[2]);
    const bool bad_window = !std::strcmp(inject, "bad-window");
    const bool skip_draw = !std::strcmp(inject, "skip-draw");
    const bool wrong_expectation = !std::strcmp(inject, "wrong-expectation");
    if ((argc != 1 && (argc != 3 || std::strcmp(argv[1], "--inject"))) || (inject[0] && !bad_window && !skip_draw && !wrong_expectation)) {
        std::printf("usage: test_d3d12_presentx [--inject bad-window|skip-draw|wrong-expectation]\n");
        return 2;
    }
    if (inject[0]) std::printf("NEGATIVE CONTROL: %s (this run must fail)\n", inject);
    g_log_start = log_size();

    /* --- Device, queue, window ----------------------------------------------------------------- */
    g_stage = kSetup;
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    REQUIRE(hr == S_OK && device, "D3D12XboxCreateDevice hr=0x%08lx", ul(hr));
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    ID3D12CommandQueue* queue = nullptr;
    hr = device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    REQUIRE(hr == S_OK && queue && vtable_in_shim(queue), "CreateCommandQueue hr=0x%08lx", ul(hr));
    HWND window = make_window();
    REQUIRE(window != nullptr, "CreateWindow failed (%lu)", GetLastError());

    /* --- The bridge and its argument contract --------------------------------------------------- */
    g_stage = kContract;
    IBxPresentQueue* bridge = nullptr;
    hr = queue->QueryInterface(IID_IBxPresentQueue, reinterpret_cast<void**>(&bridge));
    REQUIRE(hr == S_OK && bridge && vtable_in_shim(bridge), "queue QI(IBxPresentQueue) hr=0x%08lx", ul(hr));
    {
        IUnknown* from_queue = nullptr;
        IUnknown* from_bridge = nullptr;
        ID3D12CommandQueue* back = nullptr;
        queue->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&from_queue));
        bridge->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&from_bridge));
        check(from_queue && from_queue == from_bridge, "the bridge must report the queue's IUnknown identity");
        check(bridge->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&back)) == S_OK && back == queue,
              "the bridge must answer ID3D12CommandQueue with the queue proxy");
        release(back);
        release(from_bridge);
        release(from_queue);

        ID3D12CommandAllocator* other = nullptr;
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                       reinterpret_cast<void**>(&other));
        void* none = nullptr;
        check(other && other->QueryInterface(IID_IBxPresentQueue, &none) == E_NOINTERFACE && !none,
              "only command queues may answer IBxPresentQueue");
        release(other);

        IBxPresentSwapChain* out = nullptr;
        auto rejected = [&](const char* what, HWND w, const BxPresentDesc* d, HRESULT want) {
            out = reinterpret_cast<IBxPresentSwapChain*>(static_cast<uintptr_t>(0xDEAD));
            HRESULT got = bridge->CreateSwapChain(w, d, &out);
            check(got == want && out == nullptr, "%s: hr=0x%08lx out=%p, wanted 0x%08lx and null", what, ul(got),
                  static_cast<void*>(out), ul(want));
        };
        check(bridge->CreateSwapChain(window, &kDesc, nullptr) == E_POINTER, "null out pointer must be E_POINTER");
        rejected("null description", window, nullptr, E_POINTER);
        rejected("null window", nullptr, &kDesc, DXGI_ERROR_INVALID_CALL);
        HWND dead = make_window();
        DestroyWindow(dead);
        rejected("destroyed window", dead, &kDesc, DXGI_ERROR_INVALID_CALL);
        BxPresentDesc d = kDesc;
        d.width = 0;
        rejected("zero width", window, &d, DXGI_ERROR_INVALID_CALL);
        d = kDesc;
        d.height = 16385;
        rejected("oversized height", window, &d, DXGI_ERROR_INVALID_CALL);
        d = kDesc;
        d.buffer_count = 1;
        rejected("one buffer", window, &d, DXGI_ERROR_INVALID_CALL);
        d = kDesc;
        d.buffer_count = 17;
        rejected("seventeen buffers", window, &d, DXGI_ERROR_INVALID_CALL);
        d = kDesc;
        d.format = DXGI_FORMAT_D32_FLOAT;
        rejected("depth format", window, &d, DXGI_ERROR_INVALID_CALL);

        D3D12_COMMAND_QUEUE_DESC compute_desc = {};
        compute_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        ID3D12CommandQueue* compute = nullptr;
        hr = device->CreateCommandQueue(&compute_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&compute));
        IBxPresentQueue* compute_bridge = nullptr;
        if (check(SUCCEEDED(hr) && compute &&
                      compute->QueryInterface(IID_IBxPresentQueue, reinterpret_cast<void**>(&compute_bridge)) == S_OK,
                  "compute queue setup hr=0x%08lx", ul(hr))) {
            hr = compute_bridge->CreateSwapChain(window, &kDesc, &out);
            check(hr == DXGI_ERROR_INVALID_CALL && !out, "compute queue: hr=0x%08lx", ul(hr));
        }
        release(compute_bridge);
        release(compute);
    }

    /* --- Swapchain ------------------------------------------------------------------------------ */
    g_stage = kCreate;
    HWND target_window = window;
    if (bad_window) {
        target_window = make_window();
        DestroyWindow(target_window);
    }
    const ULONG queue_refs_before = ref_count(queue);
    IBxPresentSwapChain* chain = nullptr;
    hr = bridge->CreateSwapChain(target_window, &kDesc, &chain);
    REQUIRE(hr == S_OK && chain && vtable_in_shim(chain), "CreateSwapChain hr=0x%08lx", ul(hr));
    check(ref_count(queue) == queue_refs_before + 1, "the swapchain must hold one reference on the queue proxy");

    ID3D12Resource* back_buffers[kBuffers] = {};
    for (UINT i = 0; i < kBuffers; ++i) {
        hr = chain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&back_buffers[i]));
        REQUIRE(hr == S_OK && back_buffers[i], "GetBuffer(%u) hr=0x%08lx", i, ul(hr));
    }
    {
        void* none = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD));
        hr = chain->GetBuffer(kBuffers, __uuidof(ID3D12Resource), &none);
        check(FAILED(hr) && !none, "GetBuffer past the last index: hr=0x%08lx", ul(hr));
        check(chain->GetBuffer(0, __uuidof(ID3D12Resource), nullptr) == E_POINTER, "GetBuffer(null) must be E_POINTER");
        check(chain->WaitForIdle(INFINITE) == E_INVALIDARG, "an unbounded wait must be rejected");
    }
    D3D12_RESOURCE_DESC bb_desc;
    back_buffers[0]->GetDesc(&bb_desc);
    check(bb_desc.Width == kWidth && bb_desc.Height == kHeight && bb_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
          "back buffer is %llux%u format %d", static_cast<unsigned long long>(bb_desc.Width), bb_desc.Height,
          static_cast<int>(bb_desc.Format));

    /* --- Pipeline ------------------------------------------------------------------------------- */
    g_stage = kPipeline;
    D3D12_ROOT_PARAMETER constant = {};
    constant.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constant.Constants.Num32BitValues = 1;
    constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc = {};
    rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    rs_desc.Desc_1_0.NumParameters = 1;
    rs_desc.Desc_1_0.pParameters = &constant;
    ID3DBlob* rs_blob = nullptr;
    ID3DBlob* rs_error = nullptr;
    hr = D3D12SerializeVersionedRootSignature(&rs_desc, &rs_blob, &rs_error);
    REQUIRE(SUCCEEDED(hr) && rs_blob, "serialize root signature hr=0x%08lx", ul(hr));
    ID3D12RootSignature* root = nullptr;
    hr = device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                     reinterpret_cast<void**>(&root));
    REQUIRE(SUCCEEDED(hr) && root, "CreateRootSignature hr=0x%08lx", ul(hr));
    release(rs_blob);
    release(rs_error);

    ID3DBlob* vs = compile("vs", "vs_5_0");
    ID3DBlob* ps = compile("ps", "ps_5_0");
    REQUIRE(vs && ps, "shader compilation");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = root;
    pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    hr = device->CreateGraphicsPipelineState(&pso_desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso));
    REQUIRE(SUCCEEDED(hr) && pso, "CreateGraphicsPipelineState hr=0x%08lx", ul(hr));
    release(vs);
    release(ps);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = kBuffers;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    hr = device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&rtv_heap));
    REQUIRE(SUCCEEDED(hr), "CreateDescriptorHeap hr=0x%08lx", ul(hr));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[kBuffers];
    rtv_heap->GetCPUDescriptorHandleForHeapStart(&rtv[0]);
    const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < kBuffers; ++i) {
        rtv[i].ptr = rtv[0].ptr + static_cast<SIZE_T>(i) * rtv_step;
        device->CreateRenderTargetView(back_buffers[i], nullptr, rtv[i]);
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 readback_bytes = 0;
    device->GetCopyableFootprints(&bb_desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_bytes);
    ID3D12Resource* readback[kFrames] = {};
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer_desc = {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = readback_bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT f = 0; f < kFrames; ++f) {
        hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&readback[f]));
        REQUIRE(SUCCEEDED(hr), "readback buffer %u hr=0x%08lx", f, ul(hr));
    }

    ID3D12CommandAllocator* allocators[kBuffers] = {};
    for (UINT i = 0; i < kBuffers; ++i) {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                            reinterpret_cast<void**>(&allocators[i]));
        REQUIRE(SUCCEEDED(hr), "CreateCommandAllocator hr=0x%08lx", ul(hr));
    }
    ID3D12GraphicsCommandList* list = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], nullptr,
                                   __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list));
    REQUIRE(SUCCEEDED(hr) && list && SUCCEEDED(list->Close()), "CreateCommandList hr=0x%08lx", ul(hr));
    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(SUCCEEDED(hr) && fence && event, "fence hr=0x%08lx", ul(hr));

    /* --- Render and present ---------------------------------------------------------------------- */
    g_stage = kRender;
    IDXGISwapChain1* host_chain = nullptr;
    hr = chain->GetHostSwapChain(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&host_chain));
    REQUIRE(SUCCEEDED(hr) && host_chain, "GetHostSwapChain hr=0x%08lx", ul(hr));
    UINT presents_before = 0;
    hr = host_chain->GetLastPresentCount(&presents_before);
    check(SUCCEEDED(hr), "GetLastPresentCount hr=0x%08lx", ul(hr));

    UINT64 slot_value[kBuffers] = {};
    UINT indices_seen = 0;
    UINT frames_presented = 0;
    for (UINT f = 0; f < kFrames; ++f) {
        const UINT slot = chain->GetCurrentBackBufferIndex();
        REQUIRE(slot < kBuffers, "back buffer index %u", slot);
        indices_seen |= 1u << slot;
        /* No wait of our own here: reusing this slot relies on Present() having paced it. */
        check(fence->GetCompletedValue() >= slot_value[slot], "frame %u: Present returned before slot %u was idle", f, slot);
        REQUIRE(SUCCEEDED(allocators[slot]->Reset()) && SUCCEEDED(list->Reset(allocators[slot], pso)), "frame %u: reset", f);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = back_buffers[slot];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        auto transition = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            barrier.Transition.StateBefore = from;
            barrier.Transition.StateAfter = to;
            list->ResourceBarrier(1, &barrier);
        };
        transition(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const Rgba c = clear_colour(f);
        const float clear[4] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
        list->ClearRenderTargetView(rtv[slot], clear, 0, nullptr);
        if (!(skip_draw && f == kSkippedFrame)) {
            const D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
            list->OMSetRenderTargets(1, &rtv[slot], FALSE, nullptr);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &kRect);
            list->SetGraphicsRootSignature(root);
            list->SetGraphicsRoot32BitConstant(0, pattern_blue(f), 0);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
        }
        transition(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback[f];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = back_buffers[slot];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        REQUIRE(SUCCEEDED(list->Close()), "frame %u: Close", f);

        ID3D12CommandList* const lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        slot_value[slot] = f + 1;
        REQUIRE(SUCCEEDED(queue->Signal(fence, f + 1)), "frame %u: Signal", f);
        hr = chain->Present(f % 2, 0); /* alternate immediate and vsync'd presents */
        REQUIRE(SUCCEEDED(hr), "frame %u: Present hr=0x%08lx", f, ul(hr));
        ++frames_presented;
        pump(1);
    }
    hr = chain->WaitForIdle(5000);
    REQUIRE(hr == S_OK && wait_fence(fence, kFrames, event), "GPU did not finish: hr=0x%08lx", ul(hr));
    hr = device->GetDeviceRemovedReason();
    check(hr == S_OK, "device removed: 0x%08lx", ul(hr));
    UINT presents_after = 0;
    hr = host_chain->GetLastPresentCount(&presents_after);
    check(SUCCEEDED(hr) && presents_after - presents_before == kFrames,
          "host swapchain counted %u presents, expected %u (hr=0x%08lx)", presents_after - presents_before, kFrames, ul(hr));
    release(host_chain);
    check(indices_seen == (1u << kBuffers) - 1, "back buffer indices used: mask 0x%x", indices_seen);
    std::printf("frames_presented=%u host_present_count=%u\n", frames_presented, presents_after - presents_before);

    /* Invalid presents are refused by the bridge and never reach the host swapchain. */
    {
        auto refused = [&](const char* what, UINT sync, UINT flags) {
            UINT before = 0, after = 0;
            IDXGISwapChain1* h = nullptr;
            if (FAILED(chain->GetHostSwapChain(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&h)))) {
                check(false, "%s: GetHostSwapChain", what);
                return;
            }
            h->GetLastPresentCount(&before);
            const UINT index = chain->GetCurrentBackBufferIndex();
            HRESULT got = chain->Present(sync, flags);
            h->GetLastPresentCount(&after);
            h->Release();
            check(got == DXGI_ERROR_INVALID_CALL && before == after && index == chain->GetCurrentBackBufferIndex(),
                  "%s: hr=0x%08lx host presents %u -> %u", what, ul(got), before, after);
        };
        refused("sync interval 5", 5, 0);
        refused("DXGI_PRESENT_TEST", 0, DXGI_PRESENT_TEST);
        refused("tearing flag", 0, DXGI_PRESENT_ALLOW_TEARING);

        HWND doomed = make_window();
        IBxPresentSwapChain* orphan = nullptr;
        hr = bridge->CreateSwapChain(doomed, &kDesc, &orphan);
        if (check(SUCCEEDED(hr) && orphan, "swapchain for the destroyed-window case hr=0x%08lx", ul(hr))) {
            DestroyWindow(doomed);
            pump(20);
            hr = orphan->Present(0, 0);
            check(hr == DXGI_ERROR_INVALID_CALL, "present after the window is destroyed: hr=0x%08lx", ul(hr));
            check(orphan->Release() == 0, "orphaned swapchain still referenced");
        }
    }

    /* --- Pixels ---------------------------------------------------------------------------------- */
    g_stage = kPixels;
    UINT64 pixels_checked = 0;
    UINT64 pixels_wrong = 0;
    for (UINT f = 0; f < kFrames; ++f) {
        void* mapped = nullptr;
        const D3D12_RANGE all = {0, static_cast<SIZE_T>(readback_bytes)};
        hr = readback[f]->Map(0, &all, &mapped);
        REQUIRE(SUCCEEDED(hr) && mapped, "Map frame %u hr=0x%08lx", f, ul(hr));
        const auto* data = static_cast<const uint8_t*>(mapped) + footprint.Offset;
        const UINT pitch = footprint.Footprint.RowPitch;
        const UINT bad = count_mismatches(data, pitch, f, wrong_expectation ? 1 : 0, true);
        check(bad == 0, "frame %u: %u of %u pixels differ", f, bad, kWidth * kHeight);
        pixels_checked += kWidth * kHeight;
        pixels_wrong += bad;
        if (f == 0 && !skip_draw && !wrong_expectation) {
            /* The comparison itself must notice: exactly the drawn rectangle differs in each case. */
            check(count_mismatches(data, pitch, f, 1, false) == kRectPixels, "a blue value off by one went unnoticed");
            check(count_mismatches(data, pitch, f + 1, 0, false) == kWidth * kHeight, "a stale frame went unnoticed");
        }
        const D3D12_RANGE none = {0, 0};
        readback[f]->Unmap(0, &none);
    }
    std::printf("pixels_checked=%llu pixels_wrong=%llu row_pitch=%u\n", static_cast<unsigned long long>(pixels_checked),
                static_cast<unsigned long long>(pixels_wrong), footprint.Footprint.RowPitch);
    if (g_failures) finish();

    /* --- Bounded waiting --------------------------------------------------------------------------- */
    g_stage = kSync;
    {
        ID3D12Fence* gate = nullptr;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&gate));
        REQUIRE(SUCCEEDED(hr) && gate && SUCCEEDED(queue->Wait(gate, 1)), "gate fence hr=0x%08lx", ul(hr));
        const ULONGLONG start = GetTickCount64();
        hr = chain->WaitForIdle(300);
        const ULONGLONG waited = GetTickCount64() - start;
        check(hr == kTimedOut && waited >= 250 && waited < 3000, "blocked queue: hr=0x%08lx after %llu ms", ul(hr),
              static_cast<unsigned long long>(waited));
        REQUIRE(SUCCEEDED(gate->Signal(1)), "release the gate");
        hr = chain->WaitForIdle(5000);
        check(hr == S_OK, "after the gate opened: hr=0x%08lx", ul(hr));
        release(gate);
    }
    {
        /*
         * Frame pacing. With the queue blocked, no frame can finish. Presents 1 and 2 move on to
         * buffers with no earlier use; present 3 wraps to the first buffer and has to wait for
         * frame 1, so it must give up after BX_PRESENT_WAIT_MS instead of handing the buffer back.
         */
        HWND paced_window = make_window();
        IBxPresentSwapChain* paced = nullptr;
        ID3D12Fence* gate = nullptr;
        REQUIRE(SUCCEEDED(bridge->CreateSwapChain(paced_window, &kDesc, &paced)), "pacing swapchain");
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&gate));
        REQUIRE(SUCCEEDED(hr) && gate && SUCCEEDED(queue->Wait(gate, 1)), "gate fence hr=0x%08lx", ul(hr));
        for (UINT i = 0; i + 1 < kBuffers; ++i) {
            hr = paced->Present(0, 0);
            check(hr == S_OK, "blocked queue, present %u (unused buffer next): hr=0x%08lx", i + 1, ul(hr));
        }
        const ULONGLONG start = GetTickCount64();
        hr = paced->Present(0, 0);
        const ULONGLONG waited = GetTickCount64() - start;
        check(hr == kTimedOut && waited >= BX_PRESENT_WAIT_MS - 100 && waited < BX_PRESENT_WAIT_MS + 3000,
              "present onto a busy buffer: hr=0x%08lx after %llu ms", ul(hr), static_cast<unsigned long long>(waited));
        std::printf("observed: present_pacing_wait_ms=%llu\n", static_cast<unsigned long long>(waited));
        REQUIRE(SUCCEEDED(gate->Signal(1)), "release the gate");
        check(paced->WaitForIdle(5000) == S_OK, "queue did not drain after the gate opened");
        hr = paced->Present(0, 0);
        check(hr == S_OK, "present after recovery: hr=0x%08lx", ul(hr));
        check(paced->Release() == 0, "pacing swapchain still referenced");
        release(gate);
        DestroyWindow(paced_window);
    }

    /* --- Teardown and repetition ------------------------------------------------------------------ */
    g_stage = kLifetime;
    for (ID3D12Resource*& b : back_buffers) release(b);
    chain->AddRef();
    check(chain->Release() == 1, "swapchain reference counting");
    check(chain->Release() == 0, "the swapchain must reach zero references");
    chain = nullptr;
    check(ref_count(queue) == queue_refs_before, "destroying the swapchain must return its queue reference");

    HostDxgi host;
    REQUIRE(open_host(host), "host DXGI control setup");

    /* Whatever the host does with a second or a recreated swapchain, the bridge must report the same. */
    {
        HWND w = make_window();
        IDXGISwapChain1* first = nullptr;
        IDXGISwapChain1* second = nullptr;
        REQUIRE(SUCCEEDED(host_swapchain(host, w, &first)), "control: first swapchain");
        const HRESULT host_second = host_swapchain(host, w, &second);
        release(second);
        first->Release();
        pump(50);
        const HRESULT host_again = host_swapchain(host, w, &second);
        release(second);
        DestroyWindow(w);

        w = make_window();
        IBxPresentSwapChain* a = nullptr;
        IBxPresentSwapChain* b = nullptr;
        REQUIRE(SUCCEEDED(bridge->CreateSwapChain(w, &kDesc, &a)), "bridge: first swapchain");
        const HRESULT bridge_second = bridge->CreateSwapChain(w, &kDesc, &b);
        release(b);
        release(a);
        pump(50);
        const HRESULT bridge_again = bridge->CreateSwapChain(w, &kDesc, &b);
        release(b);
        DestroyWindow(w);
        check(bridge_second == host_second, "second swapchain on one window: bridge 0x%08lx, host 0x%08lx", ul(bridge_second),
              ul(host_second));
        check(bridge_again == host_again, "recreation on one window: bridge 0x%08lx, host 0x%08lx", ul(bridge_again),
              ul(host_again));
        std::printf("observed: second_swapchain_on_window host=0x%08lx bridge=0x%08lx; recreate_after_release host=0x%08lx bridge=0x%08lx\n",
                    ul(host_second), ul(bridge_second), ul(host_again), ul(bridge_again));
    }

    {
        static Witness host_witness[kProbeCycles]; /* static: a retained swapchain may outlive this scope */
        static Witness bridge_witness[kProbeCycles];
        const Retention direct = probe_host(host, host_witness);
        const Retention bridged = probe_bridge(bridge, bridge_witness);
        check(bridged.holder_refs == direct.holder_refs, "swapchain references while held: bridge %lu, host control %lu",
              bridged.holder_refs, direct.holder_refs);
        check(bridged.retained <= direct.retained, "swapchains retained after release: bridge %ld, host control %ld",
              bridged.retained, direct.retained);
        std::printf("observed: swapchain_refs_while_held host=%lu bridge=%lu; retained_after_release host=%ld/%d bridge=%ld/%d\n",
                    direct.holder_refs, bridged.holder_refs, direct.retained, kProbeCycles, bridged.retained, kProbeCycles);
    }

    char configured_log[MAX_PATH];
    if (log_path(configured_log)) {
        char needle[32];
        std::snprintf(needle, sizeof(needle), "presented=%u ", kFrames);
        check(log_has("present_chain_created", "width=100 height=60"), "no present_chain_created record from this run");
        check(log_has("present_chain_destroyed", needle), "no present_chain_destroyed record with %s", needle);
    } else {
        g_skipped++;
        std::printf("SKIP: BX_LOG_FILE is not set; present_chain log records not verified (use bx-cli run --log)\n");
    }

    release(host.factory);
    release(host.queue);
    release(host.device);
    CloseHandle(event);
    release(fence);
    release(list);
    for (ID3D12CommandAllocator*& a : allocators) release(a);
    for (ID3D12Resource*& r : readback) release(r);
    release(rtv_heap);
    release(pso);
    release(root);
    release(bridge);
    check(queue->Release() == 0, "command queue proxy leaked references");
    check(device->Release() == 0, "device proxy leaked references");
    DestroyWindow(window);
    finish();
}
