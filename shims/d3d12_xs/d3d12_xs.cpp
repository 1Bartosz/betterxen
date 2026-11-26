#include <windows.h>
#include <cstdint>

#include "bx_log.h"
#include "bx_xmem_internal.h"

/*
 * Track C (M1-C1): Xbox Series D3D12 COM proxy over Wine's d3d12.dll (D3DMetal 3.0).
 * Root signature serialization/deserialization forwards directly to Wine's d3d12.dll;
 * D3D12XboxCreateDevice and D3D12ConfigureVirtualMemoryX are owned by Track C1.
 */

namespace {

HMODULE get_host_d3d12() {
    static HMODULE mod = LoadLibraryA("d3d12.dll");
    return mod;
}

} // namespace

extern "C" {

HRESULT WINAPI D3D12XboxCreateDevice(PVOID /*params*/, REFIID /*riid*/, void** ppv) {
    BX_STUB_LOG("d3d12_xs.dll", "D3D12XboxCreateDevice");
    if (ppv) {
        *ppv = nullptr;
    }
    return E_NOTIMPL;
}

HRESULT WINAPI D3D12ConfigureVirtualMemoryX(PVOID /*config*/) {
    BX_STUB_LOG("d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX");
    return S_OK;
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const void* desc,
    void** blob,
    void** error_blob
) {
    using Fn = HRESULT(WINAPI*)(const void*, void**, void**);
    HMODULE d3d12 = get_host_d3d12();
    if (d3d12) {
        auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
            GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature")));
        if (fn) {
            return fn(desc, blob, error_blob);
        }
    }
    BX_STUB_LOG("d3d12_xs.dll", "D3D12SerializeVersionedRootSignature");
    return E_NOTIMPL;
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    LPCVOID src_data,
    SIZE_T src_size,
    REFIID riid,
    void** out_deserializer
) {
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    HMODULE d3d12 = get_host_d3d12();
    if (d3d12) {
        auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
            GetProcAddress(d3d12, "D3D12CreateRootSignatureDeserializer")));
        if (fn) {
            return fn(src_data, src_size, riid, out_deserializer);
        }
    }
    BX_STUB_LOG("d3d12_xs.dll", "D3D12CreateRootSignatureDeserializer");
    return E_NOTIMPL;
}

} // extern "C"
