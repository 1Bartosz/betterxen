// XCRDAPI.dll public DirectStorage ABI conformance (#597).
//
// Every call below goes through a hand-typed C vtable read out of the object
// itself: `slots(object)[n]` is the n-th function pointer, and each function
// pointer is cast to a signature typed here. The slot numbers and signatures
// are transcribed from the DECLARE_INTERFACE_IID_ blocks of the vendored
// shims/xcrdapi/dstorage/dstorage.h (SDK 1.4.0, MIT) in declaration order,
// including the inherited IUnknown slots 0..2, so the expected slot comes
// from a different code path than the shim's C++ class declarations.
//
// The header is included for its structure layouts, enumerations and error
// codes only; no interface method is called through a C++ interface pointer.
//
// Slot tables (header order, IUnknown = 0..2 in every interface):
//
//   IDStorageFile        3 Close, 4 GetFileInformation                       (5 slots)
//   IDStorageFactory     3 CreateQueue, 4 OpenFile, 5 CreateStatusArray,
//                         6 SetDebugFlags, 7 SetStagingBufferSize             (8)
//   IDStorageFactory1    8 CreateQueue1                                     (9)
//   IDStorageStatusArray 3 IsComplete, 4 GetHResult                         (5)
//   IDStorageQueue       3 EnqueueRequest, 4 EnqueueStatus, 5 EnqueueSignal,
//                         6 Submit, 7 CancelRequestsWithTag, 8 Close,
//                         9 GetErrorEvent, 10 RetrieveErrorRecord, 11 Query (12)
//   IDStorageQueue1     12 EnqueueSetEvent                                  (13)
//   IDStorageQueue2     13 GetCompressionSupport                           (14)
//   IDStorageQueue3     14 EnqueueRequests                                 (15)
//   IDStorageQueue4     15 Query1                                          (16)
//
// Cross-checked against Silk.NET's DirectStorage bindings (MIT): they bind the
// factory's CreateQueue/OpenFile/CreateStatusArray/SetDebugFlags/
// SetStagingBufferSize to LpVtbl[3..7], the file's Close/GetFileInformation to
// [3..4], the status array's IsComplete/GetHResult to [3..4], the queue's
// EnqueueRequest..Query to [3..11], EnqueueSetEvent to [12],
// GetCompressionSupport to [13] and EnqueueRequests to [14]. Silk.NET 1.3.0
// predates IDStorageFactory1 and IDStorageQueue4, so slots 8 and 15 are fixed
// by the header's inheritance chain alone.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "dstorage.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

int failures = 0;

void fail(int line, const char* what) {
    std::fprintf(stderr, "xcrdapi_abi:%d: %s (Win32 error %lu)\n", line, what, static_cast<unsigned long>(GetLastError()));
    ++failures;
}

#define CHECK(condition)                          \
    do {                                          \
        if (!(condition)) {                       \
            fail(__LINE__, #condition);           \
            return 1;                             \
        }                                         \
    } while (false)

// ---------------------------------------------------------------------------
// Hand-typed IIDs. dstorage.h attaches them with __declspec(uuid(...)), which
// MinGW ignores, and dstorage_mingw.h restates them for the shim; the values
// here are typed independently from the header text and cross-checked against
// Silk.NET's [Guid] attributes.
// ---------------------------------------------------------------------------
const IID kIID_File{0x5de95e7b, 0x955a, 0x4868, {0xa7, 0x3c, 0x24, 0x3b, 0x29, 0xf4, 0xb8, 0xda}};
const IID kIID_Factory{0x6924ea0c, 0xc3cd, 0x4826, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};
const IID kIID_Factory1{0x1e0da17e, 0xb6d9, 0x4c17, {0x91, 0xb0, 0xe5, 0x7a, 0xe9, 0xbd, 0xa1, 0x2e}};
const IID kIID_StatusArray{0x82397587, 0x7cd5, 0x453b, {0xa0, 0x2e, 0x31, 0x37, 0x9b, 0xd6, 0x46, 0x56}};
const IID kIID_Queue{0xcfdbd83f, 0x9e06, 0x4fda, {0x8e, 0xa5, 0x69, 0x04, 0x21, 0x37, 0xf4, 0x9b}};
const IID kIID_Queue1{0xdd2f482c, 0x5eff, 0x41e8, {0x9c, 0x9e, 0xd2, 0x37, 0x4b, 0x27, 0x81, 0x28}};
const IID kIID_Queue2{0xb1c9d643, 0x3a49, 0x44a2, {0xb4, 0x6f, 0x65, 0x36, 0x49, 0x47, 0x0d, 0x18}};
const IID kIID_Queue3{0xdeb54c52, 0xeca8, 0x46b3, {0x82, 0xa7, 0x03, 0x1b, 0x72, 0x26, 0x26, 0x53}};
const IID kIID_Queue4{0x639ff1a7, 0x739e, 0x4291, {0xb6, 0x72, 0xc7, 0x21, 0xc5, 0xf2, 0xa3, 0xda}};

// ---------------------------------------------------------------------------
// Slot numbers, transcribed from the header's declaration order.
// ---------------------------------------------------------------------------
enum : size_t { kSlotQueryInterface = 0, kSlotAddRef = 1, kSlotRelease = 2 };

namespace file_slot {
enum : size_t { kClose = 3, kGetFileInformation = 4, kCount = 5 };
}
namespace factory_slot {
enum : size_t {
    kCreateQueue = 3,
    kOpenFile = 4,
    kCreateStatusArray = 5,
    kSetDebugFlags = 6,
    kSetStagingBufferSize = 7,
    kCount = 8,
};
}
namespace factory1_slot {
enum : size_t { kCreateQueue1 = 8, kCount = 9 };
}
namespace status_slot {
enum : size_t { kIsComplete = 3, kGetHResult = 4, kCount = 5 };
}
namespace queue_slot {
enum : size_t {
    kEnqueueRequest = 3,
    kEnqueueStatus = 4,
    kEnqueueSignal = 5,
    kSubmit = 6,
    kCancelRequestsWithTag = 7,
    kClose = 8,
    kGetErrorEvent = 9,
    kRetrieveErrorRecord = 10,
    kQuery = 11,
    kCount = 12,
};
}
namespace queue1_slot {
enum : size_t { kEnqueueSetEvent = 12, kCount = 13 };
}
namespace queue2_slot {
enum : size_t { kGetCompressionSupport = 13, kCount = 14 };
}
namespace queue3_slot {
enum : size_t { kEnqueueRequests = 14, kCount = 15 };
}
namespace queue4_slot {
enum : size_t { kQuery1 = 15, kCount = 16 };
}

// ---------------------------------------------------------------------------
// The C vtable: slots are read straight out of the object.
// ---------------------------------------------------------------------------
void** slots(void* object) {
    return *static_cast<void***>(object);
}

template<class Fn>
Fn method(void* object, size_t slot) {
    return reinterpret_cast<Fn>(slots(object)[slot]);
}

// IUnknown, and the (UINT64, UINT64) pair shared by CancelRequestsWithTag.
using QueryInterfaceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
using AddRefFn = ULONG(STDMETHODCALLTYPE*)(void*);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(void*);

using FileCloseFn = void(STDMETHODCALLTYPE*)(void*);
using GetFileInformationFn = HRESULT(STDMETHODCALLTYPE*)(void*, BY_HANDLE_FILE_INFORMATION*);

using CreateQueueFn = HRESULT(STDMETHODCALLTYPE*)(void*, const DSTORAGE_QUEUE_DESC*, REFIID, void**);
using OpenFileFn = HRESULT(STDMETHODCALLTYPE*)(void*, const WCHAR*, REFIID, void**);
using CreateStatusArrayFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, PCSTR, REFIID, void**);
using SetDebugFlagsFn = void(STDMETHODCALLTYPE*)(void*, UINT32);
using SetStagingBufferSizeFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32);
using CreateQueue1Fn = HRESULT(STDMETHODCALLTYPE*)(void*, const DSTORAGE_QUEUE_DESC1*, REFIID, void**);

using IsCompleteFn = bool(STDMETHODCALLTYPE*)(void*, UINT32);
using GetHResultFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32);

using EnqueueRequestFn = void(STDMETHODCALLTYPE*)(void*, const DSTORAGE_REQUEST*);
using EnqueueStatusFn = void(STDMETHODCALLTYPE*)(void*, IDStorageStatusArray*, UINT32);
using EnqueueSignalFn = void(STDMETHODCALLTYPE*)(void*, ID3D12Fence*, UINT64);
using SubmitFn = void(STDMETHODCALLTYPE*)(void*);
using CancelRequestsWithTagFn = void(STDMETHODCALLTYPE*)(void*, UINT64, UINT64);
using QueueCloseFn = void(STDMETHODCALLTYPE*)(void*);
using GetErrorEventFn = HANDLE(STDMETHODCALLTYPE*)(void*);
using RetrieveErrorRecordFn = void(STDMETHODCALLTYPE*)(void*, DSTORAGE_ERROR_RECORD*);
using QueryFn = void(STDMETHODCALLTYPE*)(void*, DSTORAGE_QUEUE_INFO*);
using EnqueueSetEventFn = void(STDMETHODCALLTYPE*)(void*, HANDLE);
using GetCompressionSupportFn = DSTORAGE_COMPRESSION_SUPPORT(STDMETHODCALLTYPE*)(
    void*, DSTORAGE_COMPRESSION_FORMAT);
using EnqueueRequestsFn = void(STDMETHODCALLTYPE*)(void*, const DSTORAGE_REQUEST*, UINT,
                                                   ID3D12Fence*, UINT64,
                                                   DSTORAGE_ENQUEUE_REQUEST_FLAGS);
using Query1Fn = void(STDMETHODCALLTYPE*)(void*, DSTORAGE_QUEUE_INFO1*);

// ---------------------------------------------------------------------------
// Fakes reached through hand-built C vtables: no C++ class is involved, so a
// wrong slot in the shim cannot be absorbed by a matching C++ declaration.
// ---------------------------------------------------------------------------
struct FakeStatusArray {
    void** vtbl;
};

HRESULT STDMETHODCALLTYPE fake_status_qi(void*, REFIID, void** out) {
    if (out) *out = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE fake_status_addref(void*) { return 1; }
ULONG STDMETHODCALLTYPE fake_status_release(void*) { return 1; }
bool STDMETHODCALLTYPE fake_status_is_complete(void*, UINT32) { return false; }
HRESULT STDMETHODCALLTYPE fake_status_get_hresult(void*, UINT32) { return E_PENDING; }

void* fake_status_vtbl[] = {
    reinterpret_cast<void*>(static_cast<QueryInterfaceFn>(fake_status_qi)),
    reinterpret_cast<void*>(static_cast<AddRefFn>(fake_status_addref)),
    reinterpret_cast<void*>(static_cast<ReleaseFn>(fake_status_release)),
    reinterpret_cast<void*>(static_cast<IsCompleteFn>(fake_status_is_complete)),
    reinterpret_cast<void*>(static_cast<GetHResultFn>(fake_status_get_hresult)),
};

// ID3D12Fence, in the public d3d12.h order: IUnknown 0..2, GetPrivateData 3,
// SetPrivateData 4, SetPrivateDataInterface 5, SetName 6, GetDevice 7,
// GetCompletedValue 8, SetEventOnCompletion 9, Signal 10.
struct FakeFence {
    void** vtbl;
    LONG refs;
    UINT64 signalled;
    LONG signals;
};

HRESULT STDMETHODCALLTYPE fence_qi(void*, REFIID, void** out) {
    if (out) *out = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE fence_addref(void* self) {
    return static_cast<ULONG>(InterlockedIncrement(&static_cast<FakeFence*>(self)->refs));
}
ULONG STDMETHODCALLTYPE fence_release(void* self) {
    return static_cast<ULONG>(InterlockedDecrement(&static_cast<FakeFence*>(self)->refs));
}
HRESULT STDMETHODCALLTYPE fence_private(void*, REFGUID, UINT, const void*) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE fence_private_set(void*, REFGUID, UINT, const void*) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE fence_private_interface(void*, REFGUID, const IUnknown*) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE fence_set_name(void*, LPCWSTR) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE fence_get_device(void*, REFIID, void**) { return E_NOTIMPL; }
UINT64 STDMETHODCALLTYPE fence_completed(void*, UINT64) { return 0; }
HRESULT STDMETHODCALLTYPE fence_set_event(void*, UINT64, HANDLE) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE fence_signal(void* self, UINT64 value) {
    auto* fence = static_cast<FakeFence*>(self);
    fence->signalled = value;
    InterlockedIncrement(&fence->signals);
    return S_OK;
}

void* fake_fence_vtbl[] = {
    reinterpret_cast<void*>(static_cast<QueryInterfaceFn>(fence_qi)),
    reinterpret_cast<void*>(static_cast<AddRefFn>(fence_addref)),
    reinterpret_cast<void*>(static_cast<ReleaseFn>(fence_release)),
    reinterpret_cast<void*>(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, UINT, const void*)>(
        fence_private)),
    reinterpret_cast<void*>(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, UINT, const void*)>(
        fence_private_set)),
    reinterpret_cast<void*>(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, const IUnknown*)>(
        fence_private_interface)),
    reinterpret_cast<void*>(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, LPCWSTR)>(fence_set_name)),
    reinterpret_cast<void*>(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**)>(
        fence_get_device)),
    reinterpret_cast<void*>(reinterpret_cast<UINT64(STDMETHODCALLTYPE*)(void*, UINT64)>(fence_completed)),
    reinterpret_cast<void*>(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, UINT64, HANDLE)>(
        fence_set_event)),
    reinterpret_cast<void*>(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, UINT64)>(fence_signal)),
};

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------
struct SlotName {
    size_t slot;
    const char* name;
};

// Reads the current reference count through slots 1 and 2, leaving it as it was.
ULONG live_refs(void* object, AddRefFn add_ref, ReleaseFn release) {
    const ULONG held = add_ref(object);
    release(object);
    return held - 1;
}

// Every slot of one interface must be a different function: a vtable that
// repeats one pointer cannot serve the header's method order.
int check_distinct(const char* interface_name, void* object, size_t count) {
    void** vtbl = slots(object);
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            if (vtbl[i] == vtbl[j]) {
                std::fprintf(stderr, "xcrdapi_abi: %s slots %zu and %zu share a method\n",
                             interface_name, i, j);
                ++failures;
                return 1;
            }
        }
    }
    return 0;
}

void print_table(const char* interface_name, const SlotName* names, size_t count) {
    std::printf("%s: %zu slots\n", interface_name, count);
    for (size_t i = 0; i < count; ++i) {
        std::printf("  [%2zu] %s\n", names[i].slot, names[i].name);
    }
}

const SlotName kFileSlots[] = {
    {kSlotQueryInterface, "QueryInterface"}, {kSlotAddRef, "AddRef"},
    {kSlotRelease, "Release"}, {file_slot::kClose, "Close"},
    {file_slot::kGetFileInformation, "GetFileInformation"},
};
const SlotName kFactorySlots[] = {
    {kSlotQueryInterface, "QueryInterface"}, {kSlotAddRef, "AddRef"},
    {kSlotRelease, "Release"}, {factory_slot::kCreateQueue, "CreateQueue"},
    {factory_slot::kOpenFile, "OpenFile"},
    {factory_slot::kCreateStatusArray, "CreateStatusArray"},
    {factory_slot::kSetDebugFlags, "SetDebugFlags"},
    {factory_slot::kSetStagingBufferSize, "SetStagingBufferSize"},
    {factory1_slot::kCreateQueue1, "CreateQueue1"},
};
const SlotName kStatusSlots[] = {
    {kSlotQueryInterface, "QueryInterface"}, {kSlotAddRef, "AddRef"},
    {kSlotRelease, "Release"}, {status_slot::kIsComplete, "IsComplete"},
    {status_slot::kGetHResult, "GetHResult"},
};
const SlotName kQueueSlots[] = {
    {kSlotQueryInterface, "QueryInterface"},
    {kSlotAddRef, "AddRef"},
    {kSlotRelease, "Release"},
    {queue_slot::kEnqueueRequest, "EnqueueRequest"},
    {queue_slot::kEnqueueStatus, "EnqueueStatus"},
    {queue_slot::kEnqueueSignal, "EnqueueSignal"},
    {queue_slot::kSubmit, "Submit"},
    {queue_slot::kCancelRequestsWithTag, "CancelRequestsWithTag"},
    {queue_slot::kClose, "Close"},
    {queue_slot::kGetErrorEvent, "GetErrorEvent"},
    {queue_slot::kRetrieveErrorRecord, "RetrieveErrorRecord"},
    {queue_slot::kQuery, "Query"},
    {queue1_slot::kEnqueueSetEvent, "EnqueueSetEvent"},
    {queue2_slot::kGetCompressionSupport, "GetCompressionSupport"},
    {queue3_slot::kEnqueueRequests, "EnqueueRequests"},
    {queue4_slot::kQuery1, "Query1"},
};

DSTORAGE_REQUEST memory_to_memory(const void* source, void* destination, UINT32 size) {
    DSTORAGE_REQUEST request{};
    request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    request.Source.Memory.Source = source;
    request.Source.Memory.Size = size;
    request.Destination.Memory.Buffer = destination;
    request.Destination.Memory.Size = size;
    return request;
}

struct TemporaryFile {
    WCHAR name[MAX_PATH]{};
    ~TemporaryFile() {
        if (*name) DeleteFileW(name);
    }
};

int write_file(TemporaryFile* file, const char* contents, DWORD size) {
    WCHAR directory[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    CHECK(length && length < MAX_PATH);
    CHECK(GetTempFileNameW(directory, L"bxa", 0, file->name));
    const HANDLE writer = CreateFileW(file->name, GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(writer != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    const BOOL wrote = WriteFile(writer, contents, size, &written, nullptr);
    CloseHandle(writer);
    CHECK(wrote && written == size);
    return 0;
}

struct Event {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Event() {
        if (handle) CloseHandle(handle);
    }
};

// ---------------------------------------------------------------------------
// IDStorageFactory / IDStorageFactory1.
// ---------------------------------------------------------------------------
int check_factory(void* factory, TemporaryFile* file, const char* contents) {
    // Slot 0/1/2: IUnknown. The factory answers Identity for its own IIDs and
    // for the private "same implementation" IID the shim's wrappers use, so
    // QueryInterface(IDStorageFactory1) returns the same object.
    auto query_interface = method<QueryInterfaceFn>(factory, kSlotQueryInterface);
    auto add_ref = method<AddRefFn>(factory, kSlotAddRef);
    auto release = method<ReleaseFn>(factory, kSlotRelease);

    // Refcount arithmetic is checked against a read of the count itself, not
    // against an absolute number that depends on how DStorageGetFactory
    // returned the object.
    const ULONG base = live_refs(factory, add_ref, release);

    void* as_factory1 = nullptr;
    CHECK(query_interface(factory, kIID_Factory1, &as_factory1) == S_OK && as_factory1 == factory);
    CHECK(live_refs(factory, add_ref, release) == base + 1);
    CHECK(query_interface(factory, kIID_Factory, &as_factory1) == S_OK && as_factory1 == factory);
    CHECK(live_refs(factory, add_ref, release) == base + 2);
    const IID unknown{0x0badf00d, 0x1234, 0x4abc, {0x80, 1, 2, 3, 4, 5, 6, 7}};
    void* refused = reinterpret_cast<void*>(1);
    CHECK(query_interface(factory, unknown, &refused) == E_NOINTERFACE && !refused);
    CHECK(live_refs(factory, add_ref, release) == base + 2);
    CHECK(release(factory) == base + 1);
    CHECK(release(factory) == base);

    auto create_queue = method<CreateQueueFn>(factory, factory_slot::kCreateQueue);
    auto open_file = method<OpenFileFn>(factory, factory_slot::kOpenFile);
    auto create_status = method<CreateStatusArrayFn>(factory, factory_slot::kCreateStatusArray);
    auto set_debug_flags = method<SetDebugFlagsFn>(factory, factory_slot::kSetDebugFlags);
    auto set_staging = method<SetStagingBufferSizeFn>(factory, factory_slot::kSetStagingBufferSize);
    auto create_queue1 = method<CreateQueue1Fn>(factory, factory1_slot::kCreateQueue1);

    DSTORAGE_QUEUE_DESC desc{};
    desc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    desc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    desc.Priority = DSTORAGE_PRIORITY_NORMAL;
    desc.Name = "xcrdapi abi";

    // Slot 3 must be CreateQueue: it validates capacity (127 is below the
    // header's DSTORAGE_MIN_QUEUE_CAPACITY) and priority.
    DSTORAGE_QUEUE_DESC bad = desc;
    bad.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY - 1;
    void* rejected = reinterpret_cast<void*>(1);
    CHECK(create_queue(factory, &bad, kIID_Queue, &rejected) == E_DSTORAGE_INVALID_QUEUE_CAPACITY
          && !rejected);
    bad = desc;
    bad.Priority = static_cast<DSTORAGE_PRIORITY>(DSTORAGE_PRIORITY_LAST + 1);
    CHECK(create_queue(factory, &bad, kIID_Queue, &rejected) == E_DSTORAGE_INVALID_QUEUE_PRIORITY);

    // Slot 4 must be OpenFile: a missing path is a Win32 file error, not a
    // queue-capacity error, which is what distinguishes the two slots.
    void* no_file = reinterpret_cast<void*>(1);
    const HRESULT missing = open_file(factory, L"Z:\\bx597\\does-not-exist.bin", kIID_File, &no_file);
    CHECK(FAILED(missing) && missing != E_DSTORAGE_INVALID_QUEUE_CAPACITY && !no_file);
    CHECK(open_file(factory, nullptr, kIID_File, &no_file) == E_INVALIDARG);

    // Slot 5 must be CreateStatusArray: capacity first, then the optional
    // name, then the IID and the out pointer.
    void* no_status = nullptr;
    CHECK(create_status(factory, 0, "abi", kIID_StatusArray, &no_status) == E_INVALIDARG && !no_status);

    // Slot 6 must be SetDebugFlags: void, one UINT32. It has no observable
    // state in the shim, so the check is that the factory still works.
    set_debug_flags(factory, DSTORAGE_DEBUG_SHOW_ERRORS | DSTORAGE_DEBUG_BREAK_ON_ERROR);

    // Slot 7 must be SetStagingBufferSize: HRESULT, one UINT32.
    CHECK(set_staging(factory, DSTORAGE_STAGING_BUFFER_SIZE_32MB) == S_OK);

    // Slot 8 (IDStorageFactory1) must be CreateQueue1: UsesGaclShuffleTransform
    // exists only on DSTORAGE_QUEUE_DESC1, and a queue created by slot 3 can
    // never see it.
    DSTORAGE_QUEUE_DESC1 desc1{};
    desc1.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    desc1.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    desc1.Priority = DSTORAGE_PRIORITY_NORMAL;
    desc1.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    desc1.UsesGaclShuffleTransform = TRUE;
    void* no_queue = nullptr;
    CHECK(create_queue1(factory, &desc1, kIID_Queue, &no_queue) == E_NOTIMPL && !no_queue);
    desc1.UsesGaclShuffleTransform = FALSE;
    DSTORAGE_QUEUE_DESC1 short_queue = desc1;
    short_queue.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY - 1;
    CHECK(create_queue1(factory, &short_queue, kIID_Queue, &no_queue)
          == E_DSTORAGE_INVALID_QUEUE_CAPACITY);

    // Slot 4 produces a real IDStorageFile; its own table is checked next.
    void* opened = nullptr;
    CHECK(open_file(factory, file->name, kIID_File, &opened) == S_OK && opened);

    auto file_close = method<FileCloseFn>(opened, file_slot::kClose);
    auto file_info = method<GetFileInformationFn>(opened, file_slot::kGetFileInformation);
    auto file_release = method<ReleaseFn>(opened, kSlotRelease);
    CHECK(check_distinct("IDStorageFile", opened, file_slot::kCount) == 0);
    BY_HANDLE_FILE_INFORMATION info{};
    CHECK(file_info(opened, &info) == S_OK && info.nFileSizeLow == std::strlen(contents));
    CHECK(file_info(opened, nullptr) == E_POINTER);
    // Slot 3 is void Close(); afterwards slot 4 reports an invalid handle.
    file_close(opened);
    CHECK(file_info(opened, &info) == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
    CHECK(file_release(opened) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// IDStorageStatusArray.
// ---------------------------------------------------------------------------
int check_status_array(void* factory) {
    auto create_status = method<CreateStatusArrayFn>(factory, factory_slot::kCreateStatusArray);
    void* status = nullptr;
    CHECK(create_status(factory, 2, "xcrdapi abi", kIID_StatusArray, &status) == S_OK && status);
    CHECK(check_distinct("IDStorageStatusArray", status, status_slot::kCount) == 0);

    auto is_complete = method<IsCompleteFn>(status, status_slot::kIsComplete);
    auto get_hresult = method<GetHResultFn>(status, status_slot::kGetHResult);
    // The header: "This is equivalent to GetHResult(index) != E_PENDING".
    CHECK(get_hresult(status, 0) == E_PENDING);
    CHECK(is_complete(status, 0) == false);
    CHECK(get_hresult(status, 2) == E_INVALIDARG);
    CHECK(is_complete(status, 2) == true);
    return 0;
}

// ---------------------------------------------------------------------------
// IDStorageQueue … IDStorageQueue4.
// ---------------------------------------------------------------------------
int check_queue(void* factory, void* status) {
    auto create_queue = method<CreateQueueFn>(factory, factory_slot::kCreateQueue);
    DSTORAGE_QUEUE_DESC desc{};
    desc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    desc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    desc.Priority = DSTORAGE_PRIORITY_NORMAL;
    desc.Name = "xcrdapi abi queue";
    void* queue = nullptr;
    CHECK(create_queue(factory, &desc, kIID_Queue, &queue) == S_OK && queue);
    CHECK(check_distinct("IDStorageQueue4", queue, queue4_slot::kCount) == 0);

    auto query_interface = method<QueryInterfaceFn>(queue, kSlotQueryInterface);
    auto add_ref = method<AddRefFn>(queue, kSlotAddRef);
    auto release = method<ReleaseFn>(queue, kSlotRelease);

    // One implementation serves every queue level, so each IID resolves to the
    // same object and therefore the same vtable. A wrong slot here would call
    // a different method with a REFIID and a void** out pointer.
    const ULONG base = live_refs(queue, add_ref, release);
    for (const IID* iid : {&kIID_Queue, &kIID_Queue1, &kIID_Queue2, &kIID_Queue3, &kIID_Queue4}) {
        void* level = nullptr;
        CHECK(query_interface(queue, *iid, &level) == S_OK && level == queue);
        release(level);
    }
    CHECK(live_refs(queue, add_ref, release) == base);
    void* refused = reinterpret_cast<void*>(1);
    const IID unknown{0x0badf00d, 0x1234, 0x4abc, {0x80, 1, 2, 3, 4, 5, 6, 7}};
    CHECK(query_interface(queue, unknown, &refused) == E_NOINTERFACE && !refused);
    CHECK(live_refs(queue, add_ref, release) == base);

    auto enqueue_request = method<EnqueueRequestFn>(queue, queue_slot::kEnqueueRequest);
    auto enqueue_status = method<EnqueueStatusFn>(queue, queue_slot::kEnqueueStatus);
    auto enqueue_signal = method<EnqueueSignalFn>(queue, queue_slot::kEnqueueSignal);
    auto submit = method<SubmitFn>(queue, queue_slot::kSubmit);
    auto cancel = method<CancelRequestsWithTagFn>(queue, queue_slot::kCancelRequestsWithTag);
    auto close = method<QueueCloseFn>(queue, queue_slot::kClose);
    auto error_event = method<GetErrorEventFn>(queue, queue_slot::kGetErrorEvent);
    auto error_record = method<RetrieveErrorRecordFn>(queue, queue_slot::kRetrieveErrorRecord);
    auto query = method<QueryFn>(queue, queue_slot::kQuery);
    auto set_event = method<EnqueueSetEventFn>(queue, queue1_slot::kEnqueueSetEvent);
    auto compression = method<GetCompressionSupportFn>(queue, queue2_slot::kGetCompressionSupport);
    auto enqueue_requests = method<EnqueueRequestsFn>(queue, queue3_slot::kEnqueueRequests);
    auto query1 = method<Query1Fn>(queue, queue4_slot::kQuery1);

    // Slot 11: DSTORAGE_QUEUE_INFO. The header: a queue with capacity N
    // reports N - 1 empty slots and auto-submits at half capacity.
    DSTORAGE_QUEUE_INFO info{};
    query(queue, &info);
    CHECK(info.Desc.Capacity == DSTORAGE_MIN_QUEUE_CAPACITY);
    CHECK(info.Desc.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY);
    CHECK(info.Desc.Priority == DSTORAGE_PRIORITY_NORMAL);
    CHECK(info.Desc.Name && std::strcmp(info.Desc.Name, "xcrdapi abi queue") == 0);
    CHECK(info.EmptySlotCount == DSTORAGE_MIN_QUEUE_CAPACITY - 1);
    CHECK(info.RequestCountUntilAutoSubmit == DSTORAGE_MIN_QUEUE_CAPACITY / 2);
    query(queue, nullptr);

    // Slot 15: DSTORAGE_QUEUE_INFO1, which slot 11 cannot write (its
    // DSTORAGE_QUEUE_INFO is shorter, so an EnqueueStatus there would have
    // crashed on the flag word below).
    DSTORAGE_QUEUE_INFO1 info1{};
    std::memset(&info1, 0xCD, sizeof(info1));
    query1(queue, &info1);
    CHECK(info1.Desc.Capacity == DSTORAGE_MIN_QUEUE_CAPACITY);
    CHECK(info1.Desc.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY);
    CHECK(info1.Desc.UsesGaclShuffleTransform == FALSE);
    CHECK(info1.EmptySlotCount == DSTORAGE_MIN_QUEUE_CAPACITY - 1);
    CHECK(info1.RequestCountUntilAutoSubmit == DSTORAGE_MIN_QUEUE_CAPACITY / 2);
    query1(queue, nullptr);

    // Slot 13: GetCompressionSupport returns DSTORAGE_COMPRESSION_SUPPORT.
    CHECK(compression(queue, DSTORAGE_COMPRESSION_FORMAT_GDEFLATE) == DSTORAGE_COMPRESSION_SUPPORT_NONE);
    CHECK(compression(queue, DSTORAGE_COMPRESSION_FORMAT_ZSTD) == DSTORAGE_COMPRESSION_SUPPORT_NONE);

    // Slot 9/10: the error event and its record. A fresh queue has neither.
    const HANDLE event = error_event(queue);
    CHECK(event && event != INVALID_HANDLE_VALUE);
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);
    DSTORAGE_ERROR_RECORD record{};
    std::memset(&record, 0xA5, sizeof(record));
    error_record(queue, &record);
    CHECK(record.FailureCount == 0);
    CHECK(record.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_NONE);
    CHECK(record.FirstFailure.HResult == S_OK);
    error_record(queue, nullptr);

    // Slot 3: EnqueueRequest, then slot 4: EnqueueStatus with the index as a
    // separate argument, then slot 12: EnqueueSetEvent. Nothing runs before
    // slot 6 Submit.
    const char source[4] = {'a', 'b', 'c', 'd'};
    char first[4] = {};
    char second[4] = {};
    char third[4] = {};
    char fourth[4] = {};
    Event done;
    CHECK(done.handle);
    DSTORAGE_REQUEST one = memory_to_memory(source, first, 4);
    enqueue_request(queue, &one);
    enqueue_request(queue, nullptr);  // rejected, recorded in queue order
    FakeStatusArray foreign{fake_status_vtbl};
    enqueue_status(queue, reinterpret_cast<IDStorageStatusArray*>(&foreign), 1);
    enqueue_status(queue, static_cast<IDStorageStatusArray*>(status), 0);
    DSTORAGE_REQUEST two = memory_to_memory(source + 0, second, 4);
    enqueue_requests(queue, &two, 1, nullptr, 0, DSTORAGE_ENQUEUE_REQUEST_FLAG_NONE);
    FakeFence fence{fake_fence_vtbl, 1, 0, 0};
    enqueue_signal(queue, reinterpret_cast<ID3D12Fence*>(&fence), 7);
    set_event(queue, done.handle);

    auto is_complete = method<IsCompleteFn>(status, status_slot::kIsComplete);
    auto get_hresult = method<GetHResultFn>(status, status_slot::kGetHResult);
    CHECK(is_complete(status, 0) == false);
    CHECK(get_hresult(status, 0) == E_PENDING);
    CHECK(std::memcmp(first, "\0\0\0\0", 4) == 0 && std::memcmp(second, "\0\0\0\0", 4) == 0);

    submit(queue);
    if (WaitForSingleObject(done.handle, 10000) != WAIT_OBJECT_0) {
        // A vtable that swaps slot 6 with another void method (for example
        // CancelRequestsWithTag) never runs the batch: this is where it shows.
        std::fprintf(stderr, "xcrdapi_abi: queue slot 6 (Submit) did not submit\n");
        ++failures;
        return 1;
    }
    CHECK(is_complete(status, 0) == true);
    // The header: the status entry reports "the failure code of the first failed
    // request in the enqueue order" since the previous status entry, so the null
    // request enqueued between the two status entries gives entry 0 E_INVALIDARG.
    CHECK(get_hresult(status, 0) == E_INVALIDARG);
    CHECK(std::memcmp(first, source, 4) == 0);
    CHECK(std::memcmp(second, source, 4) == 0);
    // Slot 5 EnqueueSignal reached the fence through ID3D12Fence slot 10, and
    // the queue balanced its AddRef.
    CHECK(fence.signalled == 7 && fence.signals == 1 && fence.refs == 1);

    // Slot 9 and 10: the two rejected commands (a null request, then a foreign
    // status array) are reported with the first one's HRESULT and kind, and
    // retrieving the record clears the event.
    CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
    error_record(queue, &record);
    CHECK(record.FailureCount == 2);
    CHECK(record.FirstFailure.HResult == E_INVALIDARG);
    CHECK(record.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_REQUEST);
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);

    // Slot 7: CancelRequestsWithTag is accepted and, in this shim, cancels
    // nothing; the next request still copies.
    cancel(queue, 0xFFFFFFFFFFFFFFFFull, 0);
    DSTORAGE_REQUEST three = memory_to_memory(source, third, 4);
    enqueue_request(queue, &three);
    enqueue_status(queue, static_cast<IDStorageStatusArray*>(status), 1);
    set_event(queue, done.handle);
    submit(queue);
    CHECK(WaitForSingleObject(done.handle, 10000) == WAIT_OBJECT_0);
    CHECK(get_hresult(status, 1) == S_OK);
    CHECK(std::memcmp(third, source, 4) == 0);

    // Slot 8: Close drains, and later enqueues are dropped.
    close(queue);
    DSTORAGE_REQUEST four = memory_to_memory(source, fourth, 4);
    enqueue_request(queue, &four);
    submit(queue);
    CHECK(std::memcmp(fourth, "\0\0\0\0", 4) == 0);
    close(queue);

    release(queue);
    return 0;
}

}  // namespace

int main() {
    // Unbuffered, so the slot table and every failure line survive an abort.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("xcrdapi_abi: slot tables (dstorage.h 1.4.0 declaration order)\n");
    print_table("IDStorageFile", kFileSlots, file_slot::kCount);
    print_table("IDStorageFactory1", kFactorySlots, factory1_slot::kCount);
    print_table("IDStorageStatusArray", kStatusSlots, status_slot::kCount);
    print_table("IDStorageQueue4", kQueueSlots, queue4_slot::kCount);

    // DStorageGetFactory creates a factory per call; both public factory IIDs
    // must be served, and the object must answer its own table.
    void* factory = nullptr;
    if (DStorageGetFactory(kIID_Factory, &factory) != S_OK || !factory) {
        std::fprintf(stderr, "xcrdapi_abi: DStorageGetFactory failed\n");
        return 1;
    }
    void* direct = nullptr;
    if (DStorageGetFactory(kIID_Factory1, &direct) != S_OK || !direct) {
        std::fprintf(stderr, "xcrdapi_abi: DStorageGetFactory(IDStorageFactory1) failed\n");
        return 1;
    }
    if (check_distinct("IDStorageFactory1 (direct)", direct, factory1_slot::kCount) != 0) return 1;
    if (method<ReleaseFn>(direct, kSlotRelease)(direct) != 0) {
        std::fprintf(stderr, "xcrdapi_abi: direct factory release did not reach zero\n");
        return 1;
    }
    if (check_distinct("IDStorageFactory1", factory, factory1_slot::kCount) != 0) return 1;
    if (check_distinct("IDStorageFactory", factory, factory_slot::kCount) != 0) return 1;

    TemporaryFile file;
    constexpr char contents[] = "ABI597!";
    if (write_file(&file, contents, sizeof(contents) - 1) != 0) return 1;

    if (check_factory(factory, &file, contents) != 0) return 1;
    if (check_status_array(factory) != 0) return 1;

    // A second status array is created here so that the queue test can assert
    // one index per array index the header documents.
    auto create_status = method<CreateStatusArrayFn>(factory, factory_slot::kCreateStatusArray);
    void* status = nullptr;
    CHECK(create_status(factory, 2, "queue status", kIID_StatusArray, &status) == S_OK && status);
    if (check_queue(factory, status) != 0) return 1;

    method<ReleaseFn>(status, kSlotRelease)(status);
    CHECK(method<ReleaseFn>(factory, kSlotRelease)(factory) == 0);

    if (failures) {
        std::fprintf(stderr, "xcrdapi_abi: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_xcrdapi_abi: passed\n");
    return 0;
}
