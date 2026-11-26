#ifndef BETTERXEN_D3D12_XS_VMEM_H
#define BETTERXEN_D3D12_XS_VMEM_H

#include <stddef.h>
#include <stdint.h>
#include <windows.h>

/*
 * BetterXen-defined parameter block for D3D12ConfigureVirtualMemoryX.
 * version is 1. flags and reserved must be 0.
 * base_va is 0, or an address bx_xmem_query_region resolves. It is not a GPU pointer.
 * size_bytes is 0 or a 64 KiB multiple that does not exceed the active title limit.
 * cpu_pointer is an output. It is the persistent Map address of the WRITE_BACK buffer
 * that backs the region, or 0 until that heap has been created. The block passed here
 * must stay alive until the matching CreateHeap returns, because that is when a 0
 * becomes the map address. There is no separate export for this pointer.
 * A null argument, or a block whose version is not 1 or whose flags or reserved
 * word is non-zero, is not this struct. That call logs the unimplemented-API stub
 * and returns S_OK. It does not store the pointer and does not write cpu_pointer.
 * The layout must match tests/conformance/test_d3d12_placed/main.cpp.
 */
struct BxD3d12VirtualMemoryConfig {
    uint32_t version;
    uint32_t flags;
    uint64_t base_va;
    uint64_t size_bytes;
    uint64_t reserved;
    uint64_t cpu_pointer;
};
static_assert(sizeof(BxD3d12VirtualMemoryConfig) == 40, "config block is 40 bytes");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, base_va) == 8, "base_va offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, size_bytes) == 16, "size_bytes offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, reserved) == 24, "reserved offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, cpu_pointer) == 32, "cpu_pointer offset");

extern "C" HRESULT bx_d3d12_configure_virtual_memory(void* config);

/* Patches the device proxy vtable so CreateHeap and CreatePlacedResource run the code in vmem.cpp. */
void bx_vmem_install_device_hooks(void* device);

#endif /* BETTERXEN_D3D12_XS_VMEM_H */
