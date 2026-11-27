// BetterXen guest-side contracts from M2-C1 and M2-C2.
#pragma once
#include <dxgi1_4.h>

inline const GUID IID_IBxPresentQueue = {0x34127279, 0x2d93, 0x4642, {0x88, 0xa6, 0x97, 0xb8, 0x34, 0x21, 0x7b, 0x8a}};

struct BxPresentDesc {
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    UINT buffer_count;
};
struct IBxPresentSwapChain : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** ppv) = 0;
    virtual UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() = 0;
    virtual HRESULT STDMETHODCALLTYPE Present(UINT sync_interval, UINT flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE WaitForIdle(DWORD timeout_ms) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHostSwapChain(REFIID riid, void** ppv) = 0;
};
struct IBxPresentQueue : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChain(HWND window, const BxPresentDesc* desc,
                                                      IBxPresentSwapChain** out) = 0;
};

struct VmemConfig {
    uint32_t magic, size, version, flags;
    uint64_t base, bytes, reserved, cpu_pointer;
};
static_assert(sizeof(VmemConfig) == 48);
