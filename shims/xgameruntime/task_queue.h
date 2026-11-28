#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>

namespace bx::gdk {

enum class DispatchMode : uint32_t { manual, parallel, serialized, immediate };
enum class Port : uint32_t { work, completion };

class Queue;
using QueueRef = std::shared_ptr<Queue>;

struct Task {
    virtual ~Task() = default;
    virtual void run(const QueueRef& queue) = 0;
    virtual void cancel(const QueueRef& queue) { run(queue); }

    // GetTickCount64 deadline; zero means due immediately.
    uint64_t due_ms = 0;
    std::unique_ptr<Task> next;
};

QueueRef acquire_queue(void* handle);
bool submit(const QueueRef& queue, Port port, std::unique_ptr<Task> task, bool is_completion = false);

// Largest finite Sleep() chunk. INFINITE (0xFFFFFFFF) means "wait forever", so
// a delay whose remaining time is exactly INFINITE must never be passed to
// Sleep: the wait would never wake to re-check the deadline, and the callback
// would never run. Clamp every chunk to at most INFINITE - 1.
inline constexpr uint32_t kMaxFiniteSleepMs = INFINITE - 1;

// Split a GetTickCount64 remainder into one finite Sleep() chunk. `left` is
// uint64_t because a deadline subtraction can exceed 32 bits; the result is
// always in [1, INFINITE - 1] for any positive `left`, so the caller's loop
// always wakes up and re-checks the deadline.
inline uint32_t finite_sleep_chunk_ms(uint64_t left) {
    return static_cast<uint32_t>(left < kMaxFiniteSleepMs ? left : kMaxFiniteSleepMs);
}

}
