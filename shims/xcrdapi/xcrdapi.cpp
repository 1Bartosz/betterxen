#include "synthetic_storage.h"

#include <cstring>
#include <limits>
#include <new>

#include "bx_log.h"

namespace {
using namespace bx::storage;

HRESULT win32_error() { return HRESULT_FROM_WIN32(GetLastError()); }

class Lock {
public:
    explicit Lock(SRWLOCK& mutex) : mutex_(mutex) { AcquireSRWLockExclusive(&mutex_); }
    ~Lock() { ReleaseSRWLockExclusive(&mutex_); }

private:
    SRWLOCK& mutex_;
};

template<class Interface, const IID& interface_id>
class Object : public Interface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!IsEqualIID(riid, interface_id) && !IsEqualIID(riid, IID_IUnknown)) {
            return E_NOINTERFACE;
        }
        *result = static_cast<Interface*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = InterlockedDecrement(&references_);
        if (!references) delete this;
        return references;
    }

protected:
    virtual ~Object() = default;

private:
    LONG references_ = 1;
};

template<class Interface>
HRESULT publish(Interface* object, REFIID iid, void** result) {
    const HRESULT status = object->QueryInterface(iid, result);
    object->Release();
    return status;
}

HRESULT duplicate_handle(HANDLE handle, HANDLE* copy) {
    const HANDLE process = GetCurrentProcess();
    return DuplicateHandle(process, handle, process, copy, 0, FALSE, DUPLICATE_SAME_ACCESS)
        ? S_OK : win32_error();
}

class StorageFile final : public Object<File, file_iid> {
public:
    explicit StorageFile(HANDLE handle) : handle_(handle) {}

    HRESULT STDMETHODCALLTYPE Close() override {
        Lock lock(mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) return S_OK;
        if (!CloseHandle(handle_)) return win32_error();
        handle_ = INVALID_HANDLE_VALUE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) override {
        if (!info) return E_POINTER;
        Lock lock(mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        return GetFileInformationByHandle(handle_, info) ? S_OK : win32_error();
    }

    HRESULT STDMETHODCALLTYPE GetHandle(HANDLE* copy) override {
        if (!copy) return E_POINTER;
        *copy = nullptr;
        Lock lock(mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        return duplicate_handle(handle_, copy);
    }

private:
    ~StorageFile() override { Close(); }

    SRWLOCK mutex_ = SRWLOCK_INIT;
    HANDLE handle_;
};

class StatusArray final : public Object<Status, status_iid> {
public:
    struct Slot {
        HRESULT result = E_PENDING;
        bool busy = false;
    };

    StatusArray(Slot* slots, UINT count) : slots_(slots), count_(count) {}

    HRESULT STDMETHODCALLTYPE GetResult(UINT index) override {
        if (index >= count_) return E_INVALIDARG;
        Lock lock(mutex_);
        return slots_[index].result;
    }

    HRESULT begin(UINT index) {
        if (index >= count_) return E_INVALIDARG;
        Lock lock(mutex_);
        if (slots_[index].busy) return HRESULT_FROM_WIN32(ERROR_BUSY);
        slots_[index] = {E_PENDING, true};
        return S_OK;
    }

    void complete(UINT index, HRESULT result) {
        Lock lock(mutex_);
        slots_[index] = {result, false};
    }

private:
    ~StatusArray() override { delete[] slots_; }

    SRWLOCK mutex_ = SRWLOCK_INIT;
    Slot* slots_;
    UINT count_;
};

enum class Kind { request, status, signal, event };

struct Entry {
    Kind kind;
    Request request{};
    HANDLE handle = nullptr;
    StatusArray* status = nullptr;
    UINT index = 0;
    ID3D12Fence* fence = nullptr;
    UINT64 value = 0;
};

template<class T>
struct ComRelease {
    T* pointer = nullptr;
    ~ComRelease() { if (pointer) pointer->Release(); }
    void** out() { return reinterpret_cast<void**>(&pointer); }
};

struct WinEvent {
    HANDLE handle = nullptr;
    ~WinEvent() { if (handle) CloseHandle(handle); }
};

// Queue::EnqueueRequest is vtable slot 3: IUnknown occupies 0..2.
constexpr UINT kEnqueueRequestSlot = 3;

void log_compression(UINT compression) {
    const char* method = "compression";
    if (compression == compression_lz) method = "compression_lz";
    else if (compression == compression_bcpack) method = "compression_bcpack";
    else if (compression == compression_gdeflate) method = "compression_gdeflate";
    BX_VTABLE_STUB_LOG("Queue", kEnqueueRequestSlot, method);
}

bool cpu_visible_heap(const D3D12_HEAP_PROPERTIES& heap) {
    if (heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_READBACK) return true;
    if (heap.Type != D3D12_HEAP_TYPE_CUSTOM) return false;
    return heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE
        || heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 width) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = width;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!list || !resource || before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

HRESULT read_bytes(HANDLE file, UINT64 offset, void* destination, const void* source, DWORD size) {
    if (!size) return S_OK;
    if (!destination) return E_INVALIDARG;
    if (!file) {
        if (!source) return E_INVALIDARG;
        std::memmove(destination, source, size);
        return S_OK;
    }

    OVERLAPPED operation{};
    operation.Offset = static_cast<DWORD>(offset);
    operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return win32_error();

    DWORD bytes = 0;
    HRESULT result = S_OK;
    if (!ReadFile(file, destination, size, &bytes, &operation)) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            result = HRESULT_FROM_WIN32(error);
        } else if (!GetOverlappedResult(file, &operation, &bytes, TRUE)) {
            result = win32_error();
        }
    }
    CloseHandle(operation.hEvent);
    if (SUCCEEDED(result) && bytes != size) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
    return result;
}

HRESULT write_mapped(HANDLE file, const Request& request) {
    D3D12_RANGE no_read{0, 0};
    void* mapped = nullptr;
    const HRESULT map_hr = request.resource->Map(0, &no_read, &mapped);
    if (FAILED(map_hr)) return map_hr;
    if (!mapped) {
        request.resource->Unmap(0, nullptr);
        return E_FAIL;
    }
    auto* destination = static_cast<unsigned char*>(mapped) + static_cast<SIZE_T>(request.resource_offset);
    const HRESULT read_hr = read_bytes(file, request.file_offset, destination, request.source, request.size);
    D3D12_RANGE written{
        static_cast<SIZE_T>(request.resource_offset),
        static_cast<SIZE_T>(request.resource_offset + request.size)};
    request.resource->Unmap(0, &written);
    return read_hr;
}

struct CopyResult {
    HRESULT hr = S_OK;
    // The queue's enqueue AddRef on the destination must stay held.
    bool keep_destination = false;
};

// BX_XCRDAPI_COPY_TIMEOUT_MS: see synthetic_storage.h. Default 10000, clamp 0..60000.
DWORD staged_copy_timeout_ms() {
    wchar_t buf[32];
    const DWORD n = GetEnvironmentVariableW(L"BX_XCRDAPI_COPY_TIMEOUT_MS", buf, 32);
    if (n == 0 || n >= 32) return 10000;
    DWORD value = 0;
    for (DWORD i = 0; i < n; ++i) {
        if (buf[i] < L'0' || buf[i] > L'9') return 10000;
        const DWORD digit = static_cast<DWORD>(buf[i] - L'0');
        if (value > (60000u - digit) / 10u) return 60000;
        value = value * 10u + digit;
    }
    return value;
}

bool defer_staged_copy() {
    wchar_t buf[8];
    const DWORD n = GetEnvironmentVariableW(L"BX_XCRDAPI_DEFER_COPY", buf, 8);
    return n == 1 && buf[0] == L'1';
}

// Objects created for one staged copy. After ExecuteCommandLists, destroying
// them while the fence is still below 1 is a use-after-free on the GPU timeline.
struct StagedGpu {
    ComRelease<ID3D12Device> device;
    ComRelease<ID3D12Resource> upload;
    ComRelease<ID3D12CommandQueue> queue;
    ComRelease<ID3D12CommandAllocator> allocator;
    ComRelease<ID3D12GraphicsCommandList> list;
    ComRelease<ID3D12Fence> fence;
    ComRelease<ID3D12Fence> defer;
    WinEvent done;
    bool executed = false;
    bool event_armed = false;

    bool completion_unconfirmed() const {
        if (!executed) return false;
        // A removed device is no longer executing the copy. UINT64_MAX from
        // GetCompletedValue means the same thing, so the removed check is first.
        if (device.pointer && device.pointer->GetDeviceRemovedReason() != S_OK) return false;
        if (fence.pointer && fence.pointer->GetCompletedValue() >= 1) return false;
        return true;
    }

    void leak() {
        bx_log_emit(BX_LOG_WARN, "xcrdapi", "staged_copy",
                    "staged copy completion unconfirmed, keeping GPU objects alive");
        device.pointer = nullptr;
        upload.pointer = nullptr;
        queue.pointer = nullptr;
        allocator.pointer = nullptr;
        list.pointer = nullptr;
        fence.pointer = nullptr;
        defer.pointer = nullptr;
        if (event_armed) done.handle = nullptr;
    }

    // Decide the leak before signaling `defer`, or the unblocked copy can retire
    // the completion fence and look confirmed.
    CopyResult finish(HRESULT hr) {
        const bool keep = completion_unconfirmed();
        ID3D12Fence* deferred = defer.pointer;
        if (keep) leak();
        if (deferred) (void)deferred->Signal(1);
        return {hr, keep};
    }
};

// Default-heap buffers are not mapped. Stage the bytes in an upload resource and
// copy them on the device that owns `dest`, which must be in D3D12_RESOURCE_STATE_COMMON.
// Failures before ExecuteCommandLists release every object. After it, an unconfirmed
// completion deliberately leaks them (and tells the caller to keep `dest`).
CopyResult copy_staged(HANDLE file, const Request& request) {
    StagedGpu staged;
    HRESULT hr = request.resource->GetDevice(__uuidof(ID3D12Device), staged.device.out());
    if (FAILED(hr) || !staged.device.pointer) return {FAILED(hr) ? hr : E_FAIL, false};

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_DESC upload_desc = buffer_desc(request.size);
    hr = staged.device.pointer->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        __uuidof(ID3D12Resource), staged.upload.out());
    if (FAILED(hr) || !staged.upload.pointer) return {FAILED(hr) ? hr : E_FAIL, false};

    D3D12_RANGE no_read{0, 0};
    void* mapped = nullptr;
    hr = staged.upload.pointer->Map(0, &no_read, &mapped);
    if (FAILED(hr) || !mapped) {
        if (SUCCEEDED(hr)) staged.upload.pointer->Unmap(0, nullptr);
        return {FAILED(hr) ? hr : E_FAIL, false};
    }
    hr = read_bytes(file, request.file_offset, mapped, request.source, request.size);
    staged.upload.pointer->Unmap(0, nullptr);
    if (FAILED(hr)) return {hr, false};

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = staged.device.pointer->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), staged.queue.out());
    if (FAILED(hr) || !staged.queue.pointer) return {FAILED(hr) ? hr : E_FAIL, false};

    hr = staged.device.pointer->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       __uuidof(ID3D12CommandAllocator), staged.allocator.out());
    if (FAILED(hr) || !staged.allocator.pointer) return {FAILED(hr) ? hr : E_FAIL, false};

    hr = staged.device.pointer->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, staged.allocator.pointer,
                                                  nullptr, __uuidof(ID3D12GraphicsCommandList), staged.list.out());
    if (FAILED(hr) || !staged.list.pointer) return {FAILED(hr) ? hr : E_FAIL, false};

    transition(staged.list.pointer, request.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    staged.list.pointer->CopyBufferRegion(request.resource, request.resource_offset, staged.upload.pointer, 0,
                                          request.size);
    transition(staged.list.pointer, request.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    hr = staged.list.pointer->Close();
    if (FAILED(hr)) return {hr, false};

    // Test seam: block this queue until finish() signals the fence, so a 0 ms
    // wait cannot observe a copy the GPU already retired. A failed defer falls
    // through to the normal race. It is not itself a copy failure.
    if (defer_staged_copy()) {
        hr = staged.device.pointer->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), staged.defer.out());
        if (FAILED(hr) || !staged.defer.pointer
            || FAILED(staged.queue.pointer->Wait(staged.defer.pointer, 1))) {
            if (staged.defer.pointer) staged.defer.pointer->Release();
            staged.defer.pointer = nullptr;
        }
    }

    ID3D12CommandList* lists[] = {staged.list.pointer};
    staged.queue.pointer->ExecuteCommandLists(1, lists);
    staged.executed = true;

    hr = staged.device.pointer->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), staged.fence.out());
    if (FAILED(hr) || !staged.fence.pointer) return staged.finish(FAILED(hr) ? hr : E_FAIL);
    hr = staged.queue.pointer->Signal(staged.fence.pointer, 1);
    if (FAILED(hr)) return staged.finish(hr);

    staged.done.handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!staged.done.handle) return staged.finish(win32_error());
    hr = staged.fence.pointer->SetEventOnCompletion(1, staged.done.handle);
    if (FAILED(hr)) return staged.finish(hr);
    staged.event_armed = true;

    const DWORD waited = WaitForSingleObject(staged.done.handle, staged_copy_timeout_ms());
    if (waited == WAIT_OBJECT_0) {
        if (staged.defer.pointer) (void)staged.defer.pointer->Signal(1);
        return {S_OK, false};
    }
    if (waited == WAIT_TIMEOUT) return staged.finish(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
    return staged.finish(win32_error());
}

CopyResult fulfill(HANDLE file, const Request& request) {
    if (request.compression) {
        log_compression(request.compression);
        return {E_NOTIMPL, false};
    }
    if (!request.size) return {};
    if (!request.resource) {
        return {read_bytes(file, request.file_offset, request.destination, request.source, request.size), false};
    }

    const D3D12_RESOURCE_DESC desc = request.resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return {E_INVALIDARG, false};
    if (request.resource_offset > desc.Width
        || static_cast<UINT64>(request.size) > desc.Width - request.resource_offset) {
        return {E_INVALIDARG, false};
    }

    D3D12_HEAP_PROPERTIES heap{};
    const HRESULT heap_hr = request.resource->GetHeapProperties(&heap, nullptr);
    if (SUCCEEDED(heap_hr)) {
        return cpu_visible_heap(heap) ? CopyResult{write_mapped(file, request), false}
                                      : copy_staged(file, request);
    }
    const HRESULT mapped = write_mapped(file, request);
    return SUCCEEDED(mapped) ? CopyResult{mapped, false} : copy_staged(file, request);
}

// Each queue is drained serially, in FIFO order, by a single threadpool work item.
class WorkQueue final : public Object<Queue, queue_iid> {
public:
    WorkQueue(Entry* entries, UINT capacity) : entries_(entries), capacity_(capacity) {}

    bool create_work() {
        work_ = CreateThreadpoolWork(run, this, nullptr);
        return work_ != nullptr;
    }

    HRESULT STDMETHODCALLTYPE EnqueueRequest(const Request* request) override {
        constexpr UINT64 max_file_offset = std::numeric_limits<LONGLONG>::max();
        if (!request
            || (request->file && request->source)
            || (request->destination && request->resource)
            || (request->size && !request->destination && !request->resource)
            || (request->size && !request->source && !request->file)
            || (request->file && request->file_offset > max_file_offset - request->size)
            || (request->resource && request->size
                && request->resource_offset > std::numeric_limits<UINT64>::max() - request->size)) {
            return E_INVALIDARG;
        }
        Entry entry{Kind::request, *request};
        if (request->resource) request->resource->AddRef();
        if (request->file && !request->compression) {
            const HRESULT result = request->file->GetHandle(&entry.handle);
            if (FAILED(result)) {
                if (request->resource) request->resource->Release();
                return result;
            }
        }
        const HRESULT result = enqueue(entry);
        if (FAILED(result)) {
            if (entry.handle) CloseHandle(entry.handle);
            if (request->resource) request->resource->Release();
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE EnqueueStatus(Status* status, UINT index) override {
        auto* array = dynamic_cast<StatusArray*>(status);
        if (!array) return E_INVALIDARG;

        Lock lock(mutex_);
        const HRESULT available = check_available();
        if (FAILED(available)) return available;
        const HRESULT result = array->begin(index);
        if (FAILED(result)) return result;

        array->AddRef();
        Entry entry{Kind::status};
        entry.status = array;
        entry.index = index;
        append(entry);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, UINT64 value) override {
        if (!fence) return E_POINTER;
        Entry entry{Kind::signal};
        entry.fence = fence;
        entry.value = value;
        fence->AddRef();
        const HRESULT result = enqueue(entry);
        if (FAILED(result)) fence->Release();
        return result;
    }

    HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) override {
        if (!event || event == INVALID_HANDLE_VALUE) return E_INVALIDARG;
        Entry entry{Kind::event};
        const HRESULT duplicated = duplicate_handle(event, &entry.handle);
        if (FAILED(duplicated)) return duplicated;
        const HRESULT result = enqueue(entry);
        if (FAILED(result)) CloseHandle(entry.handle);
        return result;
    }

    HRESULT STDMETHODCALLTYPE Submit() override {
        Lock lock(mutex_);
        if (closed_) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        submit();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RetrieveErrorRecord(ErrorRecord* out) override {
        if (!out) return E_POINTER;
        Lock lock(mutex_);
        *out = errors_;
        errors_ = {};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Query(QueueInfo* info) override {
        if (!info) return E_POINTER;
        Lock lock(mutex_);
        *info = {capacity_, capacity_ - size_};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Close() override {
        {
            Lock lock(mutex_);
            closed_ = true;
            submit();
        }
        if (work_) WaitForThreadpoolWorkCallbacks(work_, FALSE);
        return S_OK;
    }

private:
    ~WorkQueue() override {
        Close();
        if (work_) CloseThreadpoolWork(work_);
        delete[] entries_;
    }

    HRESULT check_available() const {
        if (closed_) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        return size_ == capacity_ ? HRESULT_FROM_WIN32(ERROR_BUSY) : S_OK;
    }

    void append(const Entry& entry) {
        entries_[(head_ + size_) % capacity_] = entry;
        ++size_;
    }

    HRESULT enqueue(const Entry& entry) {
        Lock lock(mutex_);
        const HRESULT result = check_available();
        if (FAILED(result)) return result;
        append(entry);
        return S_OK;
    }

    void submit() {
        submitted_ = size_;
        if (submitted_ && !active_) {
            active_ = true;
            SubmitThreadpoolWork(work_);
        }
    }

    static void CALLBACK run(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
        static_cast<WorkQueue*>(context)->drain();
    }

    void drain() {
        for (;;) {
            Entry entry{};
            {
                Lock lock(mutex_);
                if (!submitted_) {
                    active_ = false;
                    return;
                }
                entry = entries_[head_];
            }
            HRESULT result = S_OK;
            switch (entry.kind) {
                case Kind::request: {
                    const CopyResult outcome = fulfill(entry.handle, entry.request);
                    result = outcome.hr;
                    if (entry.handle) CloseHandle(entry.handle);
                    // keep_destination holds the AddRef taken at enqueue. Releasing
                    // it while the staged copy is still on the GPU frees the dest.
                    if (entry.request.resource && !outcome.keep_destination) entry.request.resource->Release();
                    break;
                }
                case Kind::status:
                    entry.status->complete(entry.index, status_error_);
                    entry.status->Release();
                    status_error_ = S_OK;
                    break;
                case Kind::signal:
                    result = entry.fence->Signal(entry.value);
                    entry.fence->Release();
                    break;
                case Kind::event:
                    if (!SetEvent(entry.handle)) result = win32_error();
                    CloseHandle(entry.handle);
                    break;
            }
            Lock lock(mutex_);
            if (FAILED(result)) {
                if (SUCCEEDED(status_error_)) status_error_ = result;
                if (!errors_.failure_count) errors_.first_error = result;
                ++errors_.failure_count;
            }
            head_ = (head_ + 1) % capacity_;
            --size_;
            --submitted_;
        }
    }

    SRWLOCK mutex_ = SRWLOCK_INIT;
    Entry* entries_;
    UINT capacity_;
    UINT head_ = 0;
    UINT size_ = 0;
    UINT submitted_ = 0;
    bool active_ = false;
    bool closed_ = false;
    PTP_WORK work_ = nullptr;
    HRESULT status_error_ = S_OK;
    ErrorRecord errors_{};
};

class StorageFactory final : public Object<Factory, factory_iid> {
public:
    HRESULT STDMETHODCALLTYPE OpenFile(PCWSTR path, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!path || !*path) return E_INVALIDARG;
        HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return win32_error();
        auto* file = new (std::nothrow) StorageFile(handle);
        if (!file) {
            CloseHandle(handle);
            return E_OUTOFMEMORY;
        }
        return publish(file, iid, out);
    }

    HRESULT STDMETHODCALLTYPE CreateQueue(UINT capacity, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!capacity || capacity > 65535) return E_INVALIDARG;
        auto* entries = new (std::nothrow) Entry[capacity];
        if (!entries) return E_OUTOFMEMORY;
        auto* queue = new (std::nothrow) WorkQueue(entries, capacity);
        if (!queue) {
            delete[] entries;
            return E_OUTOFMEMORY;
        }
        if (!queue->create_work()) {
            const HRESULT error = win32_error();
            queue->Release();
            return error;
        }
        return publish(queue, iid, out);
    }

    HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT count, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!count || count > 65535) return E_INVALIDARG;
        auto* slots = new (std::nothrow) StatusArray::Slot[count];
        if (!slots) return E_OUTOFMEMORY;
        auto* array = new (std::nothrow) StatusArray(slots, count);
        if (!array) {
            delete[] slots;
            return E_OUTOFMEMORY;
        }
        return publish(array, iid, out);
    }

    HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT size) override {
        if (!size) return S_OK;
        BX_STUB_LOG("XCRDAPI.dll", "SetStagingBufferSize");
        return E_NOTIMPL;
    }
};
} // namespace

extern "C" HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualIID(riid, factory_iid) && !IsEqualIID(riid, IID_IUnknown)) {
        BX_STUB_LOG("XCRDAPI.dll", "DStorageGetFactory");
        return E_NOINTERFACE;
    }
    auto* factory = new (std::nothrow) StorageFactory;
    return factory ? publish(factory, riid, ppv) : E_OUTOFMEMORY;
}
