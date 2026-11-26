#ifndef BETTERXEN_D3D12_XS_PRESENTX_H
#define BETTERXEN_D3D12_XS_PRESENTX_H

/*
 * Track C (M2-C2, #465): the PresentX presentation bridge over host DXGI.
 *
 * THESE INTERFACES ARE DEFINED BY BETTERXEN. They are not the console's PresentX, whose
 * signature and structures are not defined in abi/, and no binary compatibility with any Xbox
 * interface is claimed. A guest that calls a console-only command queue slot still lands on the
 * proxy's logged stub.
 *
 * Use: QueryInterface the command queue returned by a D3D12XboxCreateDevice device for
 * IID_IBxPresentQueue, then create a swapchain on a Win32 window. No new DLL export is involved.
 *
 * The contract below the bridge is the public DXGI flip model: the caller records its own
 * resource transitions and every back buffer must be in D3D12_RESOURCE_STATE_PRESENT when it is
 * presented. D3D12 offers no way to query a resource's state, so the bridge cannot check this.
 * ResizeBuffers and fullscreen are not implemented.
 */
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>
#include <dxgi1_4.h>

/* {34127279-2D93-4642-88A6-97B834217B8A} */
inline const GUID IID_IBxPresentQueue = {0x34127279, 0x2d93, 0x4642, {0x88, 0xa6, 0x97, 0xb8, 0x34, 0x21, 0x7b, 0x8a}};
/* {DEC9F73E-DE84-4F87-9167-26203B3A1152} */
inline const GUID IID_IBxPresentSwapChain = {0xdec9f73e, 0xde84, 0x4f87, {0x91, 0x67, 0x26, 0x20, 0x3b, 0x3a, 0x11, 0x52}};

/* Longest wait Present() performs for the next back buffer, and the destructor for the GPU. */
inline constexpr DWORD BX_PRESENT_WAIT_MS = 5000;

struct BxPresentDesc {
    UINT width;          /* 1..16384; 0 ("use the window size") is not accepted */
    UINT height;         /* 1..16384 */
    DXGI_FORMAT format;  /* R8G8B8A8_UNORM, B8G8R8A8_UNORM, R10G10B10A2_UNORM or R16G16B16A16_FLOAT */
    UINT buffer_count;   /* 2..DXGI_MAX_SWAP_CHAIN_BUFFERS */
};

/*
 * One swapchain. Holds a reference on the command queue it was created from.
 * The final Release waits (bounded) for the queue to go idle. If it cannot establish that the
 * GPU has finished, it logs and deliberately leaks the host objects rather than freeing memory
 * that may still be in use. Time spent inside the host's own Release is not bounded: on the
 * wined3d/vkd3d path (not D3DMetal) an intermittent stall in it was observed.
 */
struct IBxPresentSwapChain : public IUnknown {
    /* Same rules as IDXGISwapChain::GetBuffer. The caller owns the returned reference. */
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** ppv) = 0;
    virtual UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() = 0;
    /*
     * IDXGISwapChain::Present, then a fence signal on the queue, then a wait until the GPU has
     * finished the previous use of the back buffer that is now current. Returns the DXGI result
     * (including success codes such as DXGI_STATUS_OCCLUDED), the first failure, or
     * HRESULT_FROM_WIN32(ERROR_TIMEOUT) when that wait exceeds BX_PRESENT_WAIT_MS. The bound
     * covers BetterXen's own wait, not time spent inside the host's Present.
     *
     * The bridge itself returns DXGI_ERROR_INVALID_CALL, without calling the host, for a
     * sync_interval above 4, for any flag (none is supported), and once the window is destroyed.
     * D3DMetal 3.0 was observed to answer S_OK to all of these.
     */
    virtual HRESULT STDMETHODCALLTYPE Present(UINT sync_interval, UINT flags) = 0;
    /*
     * Waits until everything submitted to the queue before this call has finished.
     * INFINITE is rejected with E_INVALIDARG; a timeout returns HRESULT_FROM_WIN32(ERROR_TIMEOUT).
     */
    virtual HRESULT STDMETHODCALLTYPE WaitForIdle(DWORD timeout_ms) = 0;
    /* The host DXGI swapchain, for diagnostics and tests. The caller owns the reference. */
    virtual HRESULT STDMETHODCALLTYPE GetHostSwapChain(REFIID riid, void** ppv) = 0;
};

/* Tear-off on the command queue proxy; QueryInterface for any other IID returns the queue's answer. */
struct IBxPresentQueue : public IUnknown {
    /*
     * The queue must be a DIRECT queue and `window` an existing window. Argument errors return
     * E_POINTER or DXGI_ERROR_INVALID_CALL; host failures are returned unchanged and logged.
     */
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChain(HWND window, const BxPresentDesc* desc, IBxPresentSwapChain** out) = 0;
};

/* Internal to d3d12_xs.dll: the command queue proxy answers IID_IBxPresentQueue with this. */
HRESULT bx_present_query_queue(ID3D12CommandQueue* queue_proxy, void** ppv);

#endif /* BETTERXEN_D3D12_XS_PRESENTX_H */
