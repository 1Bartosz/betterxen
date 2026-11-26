#include <windows.h>
#include <cstdint>

extern "C" uint64_t bx_veh_get_clwb_trap_count(void);

// Run in its own process. The launcher should report failure and the trace
// should contain a single unhandled_exception with RAX set to the marker.
int main() {
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    const uint64_t marker = 0x123456789abcdef0ULL + bx_veh_get_clwb_trap_count();
    __asm__ volatile("ud2" :: "a"(marker) : "memory");
    return 1;
}
