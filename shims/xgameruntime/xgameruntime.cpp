#include "task_queue.h"

#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <new>
#include <unordered_set>

#include "bx_config.h"
#include "bx_log.h"

namespace {

using namespace bx::gdk;

// ABI values and layouts checked against Microsoft's public PC GDK 2510.5.6322.
constexpr HRESULT runtime_not_initialized = static_cast<HRESULT>(0x89240100u);
constexpr HRESULT runtime_invalid_handle = static_cast<HRESULT>(0x8924010cu);
constexpr HRESULT illegal_method_call = static_cast<HRESULT>(0x8000000eu);
constexpr HRESULT insufficient_buffer = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

struct AsyncBlock {
    void* queue;
    void* context;
    void (CALLBACK* callback)(AsyncBlock*);
    unsigned char internal[4 * sizeof(void*)];
};

struct RuntimeOptions {
    uint32_t config_source;
    const char* config;
};

struct GameLicense {
    char store_sku_id[18];
    bool active;
    bool trial_owned_by_user;
    bool disc;
    bool trial;
    uint32_t trial_seconds;
    char trial_id[64];
    int64_t expiration_date;
};
static_assert(offsetof(GameLicense, active) == 18);
static_assert(offsetof(GameLicense, expiration_date) == 96);
static_assert(sizeof(GameLicense) == 104);

struct SpaceInfo {
    uint64_t available_free_bytes;
    uint64_t total_free_bytes;
    uint64_t used_bytes;
    uint64_t total_bytes;
};

enum class Operation : uint32_t { add_user, game_license };

struct AsyncState {
    const AsyncBlock* owner;
    uint64_t sequence;
    HRESULT status;
    Operation operation;
    uint32_t runtime_generation;
    bool cancel_requested;
    bool callback_pending;
    bool result_consumed;
    bool entitled;
};
static_assert(sizeof(AsyncState) == sizeof(AsyncBlock::internal));

std::mutex state_mutex;
std::condition_variable state_changed;
size_t runtime_refs = 0;
uint32_t runtime_generation = 0;
uint64_t next_operation = 1;
uintptr_t next_handle = 2;
std::unordered_set<void*> users;
std::unordered_set<void*> stores;

HRESULT create_handle(std::unordered_set<void*>& handles, void** out) {
    auto handle = reinterpret_cast<void*>(next_handle);
    next_handle += 2;
    try {
        handles.insert(handle);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    *out = handle;
    return S_OK;
}

AsyncState read_state(const AsyncBlock* block) {
    AsyncState state{};
    std::memcpy(&state, block->internal, sizeof(state));
    return state;
}

struct StatusWaiter {
    uint64_t sequence;
    HRESULT status;
    bool callback_pending;
    StatusWaiter* next;
};

StatusWaiter* status_waiters = nullptr;

void write_state(AsyncBlock* block, const AsyncState& state) {
    std::memcpy(block->internal, &state, sizeof(state));
    for (auto* waiter = status_waiters; waiter; waiter = waiter->next) {
        if (waiter->sequence == state.sequence) {
            waiter->status = state.status;
            waiter->callback_pending = state.callback_pending;
        }
    }
    state_changed.notify_all();
}

class CompletionTask;
CompletionTask* running_callbacks = nullptr;

class CompletionTask final : public Task {
public:
    AsyncBlock* block;
    decltype(AsyncBlock::callback) callback;
    uint64_t sequence = 0;
    DWORD thread = 0;
    CompletionTask* running_next = nullptr;

    explicit CompletionTask(AsyncBlock* block) : block(block), callback(block->callback) {}

    void run(const QueueRef&) override {
        {
            std::lock_guard lock(state_mutex);
            auto state = read_state(block);
            thread = GetCurrentThreadId();
            running_next = running_callbacks;
            running_callbacks = this;
            state.callback_pending = false;
            write_state(block, state);
        }
        callback(block);

        // The callback may free or reuse the block. Only access our own task now.
        std::lock_guard lock(state_mutex);
        auto** link = &running_callbacks;
        while (*link != this) link = &(*link)->running_next;
        *link = running_next;
        state_changed.notify_all();
    }
};

bool fixture_entitlement();
bool callback_running(uint64_t sequence, DWORD caller);

class AsyncTask final : public Task {
public:
    AsyncBlock* block;
    Operation operation;
    std::unique_ptr<CompletionTask> completion;
    bool queued = false;

    AsyncTask(AsyncBlock* block, Operation operation) : block(block), operation(operation) {}

    ~AsyncTask() override {
        if (!queued) return;
        // Submission lost the race with queue close. No callback was accepted.
        std::lock_guard lock(state_mutex);
        auto state = read_state(block);
        state.status = E_HANDLE;
        state.callback_pending = false;
        write_state(block, state);
    }

    void run(const QueueRef& queue) override {
        finish(queue, false);
    }

    void cancel(const QueueRef& queue) override {
        finish(queue, true);
    }

private:
    void finish(const QueueRef& queue, bool canceled) {
        const bool entitled = !canceled && operation == Operation::game_license && fixture_entitlement();
        {
            std::lock_guard lock(state_mutex);
            auto state = read_state(block);
            state.status = (canceled || state.cancel_requested) ? E_ABORT : S_OK;
            state.entitled = entitled;
            if (completion) completion->sequence = state.sequence;
            queued = false;
            write_state(block, state);
        }
        if (completion) submit(queue, Port::completion, std::move(completion), true);
    }
};

HRESULT begin(AsyncBlock* block, Operation operation, void* store = nullptr) {
    if (!block) return E_POINTER;
    const auto queue = acquire_queue(block->queue);
    if (!queue) return block->queue ? E_HANDLE : E_OUTOFMEMORY;

    auto work = std::unique_ptr<AsyncTask>(new (std::nothrow) AsyncTask(block, operation));
    if (!work) return E_OUTOFMEMORY;
    if (block->callback) {
        work->completion.reset(new (std::nothrow) CompletionTask(block));
        if (!work->completion) return E_OUTOFMEMORY;
    }

    {
        std::lock_guard lock(state_mutex);
        if (!runtime_refs) return runtime_not_initialized;
        if (operation == Operation::game_license && !stores.contains(store)) {
            return runtime_invalid_handle;
        }
        const auto previous = read_state(block);
        if (previous.owner == block &&
            (previous.status == E_PENDING || previous.callback_pending ||
             callback_running(previous.sequence, GetCurrentThreadId()))) {
            return illegal_method_call;
        }
        write_state(block, {block, next_operation++, E_PENDING, operation, runtime_generation,
                            false, block->callback != nullptr, false, false});
        work->queued = true;
    }
    return submit(queue, Port::work, std::move(work)) ? S_OK : E_HANDLE;
}

HRESULT check_result(AsyncBlock* block, Operation expected, AsyncState& state) {
    if (!block) return E_POINTER;
    state = read_state(block);
    if (state.owner != block || state.operation != expected || state.result_consumed) {
        return illegal_method_call;
    }
    if (!runtime_refs || state.runtime_generation != runtime_generation) return runtime_not_initialized;
    return state.status;
}

bool callback_running(uint64_t sequence, DWORD caller) {
    for (auto* callback = running_callbacks; callback; callback = callback->running_next) {
        if (callback->sequence == sequence && callback->thread != caller) return true;
    }
    return false;
}

HRESULT copy_text(const char* text, size_t size, char* out, size_t* used) {
    if (used) *used = 0;
    if (!out) return E_POINTER;
    const auto required = std::strlen(text) + 1;
    if (size < required) return insufficient_buffer;
    std::memcpy(out, text, required);
    if (used) *used = required;
    return S_OK;
}

constexpr wchar_t storage_root[] = L"C:\\bx_pls";
constexpr wchar_t fixture_name[] = L"test_xgameruntime.exe";

HRESULT executable_name(wchar_t (&module)[MAX_PATH], const wchar_t*& name) {
    const auto size = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (!size) return HRESULT_FROM_WIN32(GetLastError());
    if (size == MAX_PATH) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    const auto slash = std::wcsrchr(module, L'\\');
    name = slash ? slash + 1 : module;
    return S_OK;
}

bool fixture_entitlement() {
    // The original conformance executable explicitly identifies its synthetic SKU.
    wchar_t module[MAX_PATH];
    const wchar_t* name = nullptr;
    return SUCCEEDED(executable_name(module, name)) &&
           std::wcscmp(name, fixture_name) == 0 &&
           GetProcAddress(GetModuleHandleW(nullptr), "BetterXenSyntheticStoreFixture") != nullptr;
}

HRESULT ensure_directory(const wchar_t* path) {
    if (CreateDirectoryW(path, nullptr)) return S_OK;
    const auto error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS) return HRESULT_FROM_WIN32(error);

    const auto attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(GetLastError());
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != FILE_ATTRIBUTE_DIRECTORY) {
        return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    }
    return S_OK;
}

HRESULT storage_path(wchar_t (&path)[MAX_PATH]) {
    wchar_t module[MAX_PATH];
    const wchar_t* name = nullptr;
    const auto hr = executable_name(module, name);
    if (FAILED(hr)) return hr;
    // Hash the full Windows path so identically named workloads in separate
    // directories get distinct storage within the same bottle.
    wchar_t normalized[MAX_PATH];
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, module, -1,
                       normalized, MAX_PATH, nullptr, nullptr, 0)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    uint64_t path_id = 14695981039346656037ULL;
    for (auto* ch = normalized; *ch; ++ch) {
        path_id ^= static_cast<uint16_t>(*ch);
        path_id *= 1099511628211ULL;
    }
    const auto length = std::swprintf(path, MAX_PATH, L"%ls\\%016llx", storage_root,
                                      static_cast<unsigned long long>(path_id));
    return length < 0 ? HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE) : S_OK;
}

HRESULT create_storage(const wchar_t* path) {
    const auto hr = ensure_directory(storage_root);
    return FAILED(hr) ? hr : ensure_directory(path);
}

// Caller holds state_mutex. Same transition XGameRuntimeUninitialize uses.
bool release_runtime_ref() {
    if (!runtime_refs) return false;
    if (!--runtime_refs) {
        users.clear();
        stores.clear();
    }
    return true;
}

bool runtime_active() {
    std::lock_guard lock(state_mutex);
    return runtime_refs != 0;
}

struct ApiInitOptions {
    uint32_t unknown;
    uint8_t is_inline;
    uint8_t pad[3];
    const char* game_config;
};
static_assert(offsetof(ApiInitOptions, game_config) == 8);

constexpr uint16_t family_data2 = 0x5853;
constexpr uint16_t family_data3 = 0x4000;

bool synthetic_guid(const GUID* guid) {
    return guid->Data2 == family_data2 && guid->Data3 == family_data3 && guid->Data4[0] == 0x80 &&
           guid->Data4[1] == 0 && guid->Data4[2] == 0 && guid->Data4[3] == 0 && guid->Data4[4] == 0 &&
           guid->Data4[5] == 0 && guid->Data4[6] == 0 && guid->Data4[7] == 1;
}

void format_guid(char* dst, size_t cap, const GUID* guid) {
    std::snprintf(dst, cap, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned long>(guid->Data1), static_cast<unsigned>(guid->Data2),
                  static_cast<unsigned>(guid->Data3), guid->Data4[0], guid->Data4[1], guid->Data4[2],
                  guid->Data4[3], guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
}

void append_escaped(char* dst, size_t cap, size_t* used, const char* src) {
    size_t u = *used;
    if (!src) src = "(null)";
    for (size_t i = 0; src[i] && u + 2 < cap; ++i) {
        const auto c = static_cast<unsigned char>(src[i]);
        if (c == '"' || c == '\\') {
            dst[u++] = '\\';
            dst[u++] = static_cast<char>(c);
        } else if (c < 0x20) {
            dst[u++] = ' ';
        } else {
            dst[u++] = static_cast<char>(c);
        }
    }
    dst[u < cap ? u : cap - 1] = '\0';
    *used = u < cap ? u : cap - 1;
}

HRESULT check_loader_versions(uint32_t gdk_ver, uint32_t gs_ver, const char* event) {
    if (gdk_ver == 0 || gs_ver == 0) {
        bx_log_emit(BX_LOG_WARN, "xgameruntime", event, "invalid_args");
        return E_INVALIDARG;
    }
    return S_OK;
}

} // namespace

extern "C" {

HRESULT WINAPI XGameRuntimeInitialize() {
    std::lock_guard lock(state_mutex);
    if (!runtime_refs++) ++runtime_generation;
    return S_OK;
}

HRESULT WINAPI XGameRuntimeInitializeWithOptions(const RuntimeOptions* options) {
    if (!options) return E_POINTER;
    if (options->config_source > 2 || (options->config_source == 0) != (options->config == nullptr)) {
        return E_INVALIDARG;
    }
    if (options->config_source != 0) {
        BX_STUB_LOG("xgameruntime.dll", "XGameRuntimeInitializeWithOptions");
        return E_NOTIMPL;
    }
    return XGameRuntimeInitialize();
}

void WINAPI XGameRuntimeUninitialize() {
    std::lock_guard lock(state_mutex);
    release_runtime_ref();
}

HRESULT WINAPI XAsyncGetStatus(AsyncBlock* block, bool wait) {
    if (!block) return E_POINTER;
    std::unique_lock lock(state_mutex);
    const auto state = read_state(block);
    if (state.owner != block) return E_INVALIDARG;
    if (!wait) return state.status;

    // A callback can recycle the same block before this waiter wakes up.
    StatusWaiter waiter{state.sequence, state.status, state.callback_pending, status_waiters};
    status_waiters = &waiter;
    const auto caller = GetCurrentThreadId();
    state_changed.wait(lock, [&] {
        return waiter.status != E_PENDING && !waiter.callback_pending &&
               !callback_running(waiter.sequence, caller);
    });
    auto** link = &status_waiters;
    while (*link != &waiter) link = &(*link)->next;
    *link = waiter.next;
    return waiter.status;
}

HRESULT WINAPI XAsyncGetResultSize(AsyncBlock* block, size_t* size) {
    if (!size) return E_POINTER;
    *size = 0;
    if (!block) return E_POINTER;
    std::lock_guard lock(state_mutex);
    const auto state = read_state(block);
    if (state.owner != block || state.result_consumed) return illegal_method_call;
    if (SUCCEEDED(state.status)) {
        *size = state.operation == Operation::add_user ? sizeof(void*) : sizeof(GameLicense);
    }
    return state.status;
}

void WINAPI XAsyncCancel(AsyncBlock* block) {
    if (!block) return;
    std::lock_guard lock(state_mutex);
    auto state = read_state(block);
    if (state.owner == block && state.status == E_PENDING) {
        state.cancel_requested = true;
        write_state(block, state);
    }
}

HRESULT WINAPI XUserAddAsync(uint32_t options, AsyncBlock* block) {
    if (options & ~7u) return E_INVALIDARG;
    return begin(block, Operation::add_user);
}

HRESULT WINAPI XUserAddResult(AsyncBlock* block, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    std::lock_guard lock(state_mutex);
    AsyncState state{};
    const auto hr = check_result(block, Operation::add_user, state);
    if (FAILED(hr)) return hr;

    const auto result = create_handle(users, out);
    if (SUCCEEDED(result)) {
        state.result_consumed = true;
        write_state(block, state);
    }
    return result;
}

void WINAPI XUserCloseHandle(void* user) {
    std::lock_guard lock(state_mutex);
    users.erase(user);
}

HRESULT WINAPI XUserGetId(void* user, uint64_t* id) {
    if (!id) return E_POINTER;
    std::lock_guard lock(state_mutex);
    if (!users.contains(user)) return runtime_invalid_handle;
    *id = 0x0009000000000001ULL;
    return S_OK;
}

HRESULT WINAPI XUserGetGamertag(void* user, uint32_t component, size_t size, char* out, size_t* used) {
    std::lock_guard lock(state_mutex);
    if (!users.contains(user)) return runtime_invalid_handle;
    if (component > 3) return E_INVALIDARG;
    return copy_text(component == 2 ? "" : "BetterXenLocal", size, out, used);
}

HRESULT WINAPI XStoreCreateContext(void* user, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    std::lock_guard lock(state_mutex);
    if (!runtime_refs) return runtime_not_initialized;
    if (user && !users.contains(user)) return runtime_invalid_handle;
    return create_handle(stores, out);
}

void WINAPI XStoreCloseContextHandle(void* store) {
    std::lock_guard lock(state_mutex);
    stores.erase(store);
}

HRESULT WINAPI XStoreQueryGameLicenseAsync(void* store, AsyncBlock* block) {
    return begin(block, Operation::game_license, store);
}

HRESULT WINAPI XStoreQueryGameLicenseResult(AsyncBlock* block, GameLicense* out) {
    if (!out) return E_POINTER;
    std::lock_guard lock(state_mutex);
    AsyncState state{};
    const auto hr = check_result(block, Operation::game_license, state);
    if (FAILED(hr)) return hr;

    std::memset(out, 0, sizeof(*out));
    if (state.entitled) {
        constexpr char sku[] = "BXTEST000001/0001";
        static_assert(sizeof(sku) <= sizeof(out->store_sku_id));
        std::memcpy(out->store_sku_id, sku, sizeof(sku));
        out->active = true;
        out->expiration_date = INT64_MAX;
    }
    state.result_consumed = true;
    write_state(block, state);
    return S_OK;
}

HRESULT WINAPI XSystemGetConsoleId(size_t size, char* out, size_t* used) {
    constexpr char id[39] = "00000000.00000000.00000000.00000000.00";
    if (used) *used = 0;
    if (!out) return E_POINTER;
    if (size < sizeof(id)) return insufficient_buffer;
    std::memcpy(out, id, sizeof(id));
    if (used) *used = sizeof(id);
    return S_OK;
}

uint32_t WINAPI XSystemGetDeviceType() {
    const auto* config = bx_get_runtime_config();
    return config && config->profile == BX_PROFILE_ANACONDA ? 7u : 6u;
}

HRESULT WINAPI XSystemGetXboxLiveSandboxId(size_t size, char* out, size_t* used) {
    return copy_text("RETAIL", size, out, used);
}

HRESULT WINAPI XPersistentLocalStorageGetPathSize(size_t* size) {
    if (!size) return E_POINTER;
    *size = 0;
    wchar_t path[MAX_PATH];
    const auto hr = storage_path(path);
    if (FAILED(hr)) return hr;
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, nullptr, 0, nullptr, nullptr);
    if (!bytes) return HRESULT_FROM_WIN32(GetLastError());
    *size = static_cast<size_t>(bytes);
    return S_OK;
}

HRESULT WINAPI XPersistentLocalStorageGetPath(size_t size, char* out, size_t* used) {
    if (used) *used = 0;
    if (!out) return E_POINTER;
    size_t required;
    auto hr = XPersistentLocalStorageGetPathSize(&required);
    if (FAILED(hr)) return hr;
    if (size < required) return insufficient_buffer;

    wchar_t path[MAX_PATH];
    hr = storage_path(path);
    if (FAILED(hr)) return hr;
    hr = create_storage(path);
    if (FAILED(hr)) return hr;

    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, out,
                                           static_cast<int>(required), nullptr, nullptr);
    if (!bytes) return HRESULT_FROM_WIN32(GetLastError());
    if (used) *used = static_cast<size_t>(bytes);
    return S_OK;
}

HRESULT WINAPI XPersistentLocalStorageGetSpaceInfo(SpaceInfo* out) {
    if (!out) return E_POINTER;
    wchar_t path[MAX_PATH];
    auto hr = storage_path(path);
    if (FAILED(hr)) return hr;
    hr = create_storage(path);
    if (FAILED(hr)) return hr;

    ULARGE_INTEGER available, total, free;
    if (!GetDiskFreeSpaceExW(path, &available, &total, &free)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    // The bottle shares its volume with other data and has no title storage quota.
    *out = {available.QuadPart, free.QuadPart,
            total.QuadPart >= free.QuadPart ? total.QuadPart - free.QuadPart : 0, total.QuadPart};
    return S_OK;
}

#define OFFLINE_STUB(symbol, args) \
    HRESULT WINAPI symbol args { BX_STUB_LOG("xgameruntime.dll", #symbol); return E_NOTIMPL; }

OFFLINE_STUB(XUserGetTokenAndSignatureAsync, (void*, uint32_t, const char*, const char*, size_t, const void*, size_t, const void*, AsyncBlock*))
OFFLINE_STUB(XUserGetTokenAndSignatureResultSize, (AsyncBlock*, size_t*))
OFFLINE_STUB(XUserGetTokenAndSignatureResult, (AsyncBlock*, size_t, void*, void**, size_t*))
OFFLINE_STUB(XUserResolvePrivilegeWithUiAsync, (void*, uint32_t, uint32_t, AsyncBlock*))
OFFLINE_STUB(XUserResolvePrivilegeWithUiResult, (AsyncBlock*))
OFFLINE_STUB(XStoreAcquireLicenseForPackageAsync, (void*, const char*, AsyncBlock*))
OFFLINE_STUB(XStoreAcquireLicenseForPackageResult, (AsyncBlock*, void**))
OFFLINE_STUB(XStoreAcquireLicenseForDurablesAsync, (void*, const char*, AsyncBlock*))
OFFLINE_STUB(XStoreAcquireLicenseForDurablesResult, (AsyncBlock*, void**))
OFFLINE_STUB(XStoreQueryEntitledProductsAsync, (void*, uint32_t, uint32_t, AsyncBlock*))
OFFLINE_STUB(XStoreQueryEntitledProductsResult, (AsyncBlock*, void**))
OFFLINE_STUB(XStoreQueryAssociatedProductsAsync, (void*, uint32_t, uint32_t, AsyncBlock*))
OFFLINE_STUB(XStoreQueryAssociatedProductsResult, (AsyncBlock*, void**))
OFFLINE_STUB(XGameUiShowPlayerProfileCardAsync, (void*, uint64_t, AsyncBlock*))
OFFLINE_STUB(XGameUiShowTextEntryAsync, (const char*, const char*, const char*, uint32_t, uint32_t, AsyncBlock*))
OFFLINE_STUB(XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync, (AsyncBlock*))
OFFLINE_STUB(XNetworkingQuerySecurityInformationForUrlAsync, (const char*, AsyncBlock*))

#undef OFFLINE_STUB

HRESULT WINAPI XTaskQueueCreate(bx::gdk::DispatchMode, bx::gdk::DispatchMode, void**);
void WINAPI XTaskQueueCloseHandle(void*);
bool WINAPI XTaskQueueDispatch(void*, bx::gdk::Port, uint32_t);
HRESULT WINAPI XTaskQueueSubmitCallback(void*, bx::gdk::Port, void*, void (CALLBACK*)(void*, bool));
bool WINAPI XTaskQueueGetCurrentProcessTaskQueue(void**);

HRESULT WINAPI InitializeApiImpl(uint32_t gdk_ver, uint32_t gs_ver) {
    const auto hr = check_loader_versions(gdk_ver, gs_ver, "InitializeApiImpl");
    return FAILED(hr) ? hr : XGameRuntimeInitialize();
}

HRESULT WINAPI InitializeApiImplEx(uint32_t gdk_ver, uint32_t gs_ver, char /*mode*/) {
    const auto hr = check_loader_versions(gdk_ver, gs_ver, "InitializeApiImplEx");
    return FAILED(hr) ? hr : XGameRuntimeInitialize();
}

HRESULT WINAPI InitializeApiImplEx2(uint32_t gdk_ver, uint32_t gs_ver, char /*mode*/, const ApiInitOptions* options) {
    const auto hr = check_loader_versions(gdk_ver, gs_ver, "InitializeApiImplEx2");
    if (FAILED(hr)) return hr;
    if (options && options->is_inline && !options->game_config) {
        bx_log_emit(BX_LOG_WARN, "xgameruntime", "InitializeApiImplEx2", "invalid_args");
        return E_INVALIDARG;
    }
    // Custom game config is the same unimplemented path as XGameRuntimeInitializeWithOptions.
    if (options && (options->is_inline || options->game_config)) {
        BX_STUB_LOG("xgameruntime.dll", "InitializeApiImplEx2");
        return E_NOTIMPL;
    }
    return XGameRuntimeInitialize();
}

HRESULT WINAPI UninitializeApiImpl() {
    bool released;
    {
        std::lock_guard lock(state_mutex);
        released = release_runtime_ref();
    }
    if (!released) {
        bx_log_emit(BX_LOG_WARN, "xgameruntime", "UninitializeApiImpl", "not_initialized");
        return runtime_not_initialized;
    }
    return S_OK;
}

void WINAPI XErrorReport(HRESULT hr, const char* message) {
    char detail[480];
    const auto prefix = std::snprintf(detail, sizeof(detail), "hr=0x%08lX msg=",
                                      static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    size_t used = prefix > 0 ? static_cast<size_t>(prefix) : 0;
    if (used >= sizeof(detail)) used = sizeof(detail) - 1;
    append_escaped(detail, sizeof(detail), &used, message);
    bx_log_emit(BX_LOG_ERROR, "xgameruntime", "XErrorReport", detail);
}

struct TaskQueueDispatch {
    decltype(&XTaskQueueCreate) create;
    decltype(&XTaskQueueCloseHandle) close_handle;
    decltype(&XTaskQueueDispatch) dispatch;
    decltype(&XTaskQueueSubmitCallback) submit;
    decltype(&XTaskQueueGetCurrentProcessTaskQueue) current;
};
struct AsyncDispatch {
    decltype(&XAsyncGetStatus) get_status;
    decltype(&XAsyncGetResultSize) get_result_size;
    decltype(&XAsyncCancel) cancel;
};
struct UserDispatch {
    decltype(&XUserAddAsync) add_async;
    decltype(&XUserAddResult) add_result;
    decltype(&XUserGetId) get_id;
    decltype(&XUserGetGamertag) get_gamertag;
};
struct StoreDispatch {
    decltype(&XStoreCreateContext) create_context;
    decltype(&XStoreQueryGameLicenseAsync) query_license_async;
    decltype(&XStoreQueryGameLicenseResult) query_license_result;
};
struct SystemDispatch {
    decltype(&XSystemGetDeviceType) get_device_type;
    decltype(&XSystemGetConsoleId) get_console_id;
    decltype(&XSystemGetXboxLiveSandboxId) get_sandbox_id;
};
struct StorageDispatch {
    decltype(&XPersistentLocalStorageGetPathSize) get_path_size;
    decltype(&XPersistentLocalStorageGetPath) get_path;
    decltype(&XPersistentLocalStorageGetSpaceInfo) get_space_info;
};

const TaskQueueDispatch task_queue_api{&XTaskQueueCreate, &XTaskQueueCloseHandle, &XTaskQueueDispatch,
                                       &XTaskQueueSubmitCallback, &XTaskQueueGetCurrentProcessTaskQueue};
const AsyncDispatch async_api{&XAsyncGetStatus, &XAsyncGetResultSize, &XAsyncCancel};
const UserDispatch user_api{&XUserAddAsync, &XUserAddResult, &XUserGetId, &XUserGetGamertag};
const StoreDispatch store_api{&XStoreCreateContext, &XStoreQueryGameLicenseAsync, &XStoreQueryGameLicenseResult};
const SystemDispatch system_api{&XSystemGetDeviceType, &XSystemGetConsoleId, &XSystemGetXboxLiveSandboxId};
const StorageDispatch storage_api{&XPersistentLocalStorageGetPathSize, &XPersistentLocalStorageGetPath,
                                  &XPersistentLocalStorageGetSpaceInfo};

struct Family {
    uint32_t clsid;
    uint32_t iid;
    const char* name;
    const void* table;
};

// Synthetic BetterXen dispatch keys, not Microsoft CLSIDs.
// Data2 = 0x5853, Data3 = 0x4000, Data4 = 80 00 00 00 00 00 00 01.
const Family families[] = {
    {0xB7581001, 0xB7581002, "xtaskqueue", &task_queue_api},
    {0xB7581101, 0xB7581102, "xasync", &async_api},
    {0xB7581201, 0xB7581202, "xuser", &user_api},
    {0xB7581301, 0xB7581302, "xstore", &store_api},
    {0xB7581401, 0xB7581402, "xsystem", &system_api},
    {0xB7581501, 0xB7581502, "xpersistentlocalstorage", &storage_api},
};

const Family* find_family(const GUID* clsid) {
    if (!synthetic_guid(clsid)) return nullptr;
    for (const auto& family : families) {
        if (clsid->Data1 == family.clsid) return &family;
    }
    return nullptr;
}

HRESULT WINAPI QueryApiImpl(const GUID* clsid, const GUID* iid, void** out) {
    if (!out) {
        bx_log_emit(BX_LOG_WARN, "xgameruntime", "QueryApiImpl", "invalid_args");
        return E_POINTER;
    }
    *out = nullptr;
    if (!clsid || !iid) {
        bx_log_emit(BX_LOG_WARN, "xgameruntime", "QueryApiImpl", "invalid_args");
        return E_INVALIDARG;
    }
    if (!runtime_active()) {
        bx_log_emit(BX_LOG_WARN, "xgameruntime", "QueryApiImpl", "not_initialized");
        return runtime_not_initialized;
    }

    const auto* family = find_family(clsid);
    if (family && synthetic_guid(iid) && iid->Data1 == family->iid) {
        *out = const_cast<void*>(family->table);
        char detail[64];
        std::snprintf(detail, sizeof(detail), "dispatch family=%s", family->name);
        bx_log_emit(BX_LOG_INFO, "xgameruntime", "QueryApiImpl", detail);
        return S_OK;
    }

    char clsid_text[48], iid_text[48], detail[192], symbol[160];
    format_guid(clsid_text, sizeof(clsid_text), clsid);
    format_guid(iid_text, sizeof(iid_text), iid);
    std::snprintf(symbol, sizeof(symbol), "QueryApiImpl(%s/%s)", clsid_text, iid_text);
    if (family) {
        std::snprintf(detail, sizeof(detail), "iid_mismatch family=%s clsid=%s iid=%s", family->name, clsid_text,
                      iid_text);
    } else {
        std::snprintf(detail, sizeof(detail), "unsupported clsid=%s iid=%s", clsid_text, iid_text);
    }
    bx_log_emit(BX_LOG_WARN, "xgameruntime", "QueryApiImpl", detail);
    BX_STUB_LOG("xgameruntime.dll", symbol);
    return E_NOINTERFACE;
}

} // extern "C"
