#ifndef BETTERXEN_SYNTHETIC_STORAGE_H
#define BETTERXEN_SYNTHETIC_STORAGE_H

#include <windows.h>
// Host d3d12.dll returns large structs through a hidden pointer. The toolchain
// header only matches that ABI when this macro is set.
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif
#include <d3d12.h>
#include <unknwn.h>

// BetterXen-only COM contract for original conformance guests. The Xbox
// DirectStorage interface IDs and binary layouts are independent of these types.
namespace bx::storage {
inline constexpr IID factory_iid{0xaad71506, 0x7beb, 0x49fc, {0x94, 0x1d, 0x46, 0x58, 0x75, 0xed, 0x35, 0x00}};
inline constexpr IID file_iid{0x723738ce, 0x6772, 0x47dc, {0x95, 0x47, 0x27, 0x7d, 0x67, 0x22, 0xb0, 0xc7}};
inline constexpr IID queue_iid{0xd895469a, 0xd810, 0x4dea, {0xb4, 0x48, 0xf4, 0x42, 0x5e, 0x52, 0x9e, 0x69}};
inline constexpr IID status_iid{0xfb214629, 0xa3ae, 0x4a81, {0x9c, 0xe3, 0xe8, 0x42, 0x6b, 0x08, 0x62, 0xd7}};

struct File : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHandle(HANDLE*) = 0;
};

// Status entries are queue-position scoped: each slot reports the first error
// among entries submitted before it since the previous status entry, and is
// not bound to a specific request handle.
struct Status : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetResult(UINT index) = 0;
};

// BetterXen-defined request. Borrowed CPU pointers stay valid until completion.
// File requests retain a duplicate of the file handle when enqueued.
// Exactly one of `file` or `source` may be non-null.
// A non-zero `size` needs exactly one of `destination` (CPU memory) or `resource`
// (an ID3D12Resource buffer written at `resource_offset`). CPU-visible heaps are
// mapped. A default heap is copied on the device GetDevice returns and must be
// in D3D12_RESOURCE_STATE_COMMON across that copy.
// `compression` values are BetterXen-defined selectors, not an Xbox enum.
struct Request {
    File* file;
    UINT64 file_offset;
    const void* source;
    void* destination;
    DWORD size;
    UINT compression;
    ID3D12Resource* resource = nullptr;
    UINT64 resource_offset = 0;
};

inline constexpr UINT compression_none = 0;
inline constexpr UINT compression_lz = 1;
inline constexpr UINT compression_bcpack = 2;
inline constexpr UINT compression_gdeflate = 3;

struct ErrorRecord {
    HRESULT first_error;
    UINT64 failure_count;
};

struct QueueInfo {
    UINT capacity;
    UINT available;
};

struct Queue : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnqueueRequest(const Request*) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnqueueStatus(Status*, UINT index) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence*, UINT64 value) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE) = 0;
    virtual HRESULT STDMETHODCALLTYPE Submit() = 0;
    virtual HRESULT STDMETHODCALLTYPE RetrieveErrorRecord(ErrorRecord*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Query(QueueInfo*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

struct Factory : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE OpenFile(PCWSTR, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateQueue(UINT capacity, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT count, REFIID, void**) = 0;
    // 0 succeeds as a no-op; non-zero sizes log and return E_NOTIMPL.
    virtual HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT size) = 0;
};
} // namespace bx::storage

extern "C" HRESULT WINAPI DStorageGetFactory(REFIID, void**);

#endif
