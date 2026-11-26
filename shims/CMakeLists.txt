cmake_minimum_required(VERSION 3.22)
project(betterxen_shims LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(BX_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(BX_ABI_INCLUDE_DIR "${BX_ROOT_DIR}/abi/include")
set(BX_ABI_DEFS_DIR "${BX_ROOT_DIR}/abi/defs")

# Put all built DLLs and conformance EXEs into a single staging directory for easy bottle deployment.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fno-exceptions
)

# Static link GCC/C++/winpthread runtimes into every shim DLL so Wine bottles need zero extra MinGW runtime DLLs.
add_link_options(
    -static
    -static-libgcc
    -static-libstdc++
    -Wl,--enable-stdcall-fixup
)

function(bx_add_shim_dll target_name dll_stem def_file)
    add_library(${target_name} SHARED ${ARGN} "${BX_ABI_DEFS_DIR}/${def_file}")
    set_target_properties(${target_name} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${dll_stem}"
        SUFFIX ".dll"
    )
    target_include_directories(${target_name} PRIVATE "${BX_ABI_INCLUDE_DIR}")
    target_compile_definitions(${target_name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endfunction()

# Track A: Core runtime VEH/logging & Win32/PSM forwarders
add_subdirectory(runtime_veh)
add_subdirectory(win32_bridge)

# Track B: Memory & Storage
add_subdirectory(xmem)
add_subdirectory(xcrdapi)

# Track C: Graphics & Texture Tiling
add_subdirectory(d3d12_xs)
add_subdirectory(xg_xs)
add_subdirectory(ffx_bvh_hooks)

# Track E: Platform Runtime, Input & Audio/DSP
add_subdirectory(xgameruntime)
add_subdirectory(gameinput)
add_subdirectory(audio)

# Track F: Synthetic PE32+ Conformance Suite
option(BX_BUILD_CONFORMANCE_TESTS "Build synthetic Windows PE32+ conformance binaries" ON)
if(BX_BUILD_CONFORMANCE_TESTS)
    add_subdirectory("${BX_ROOT_DIR}/tests/conformance" "${CMAKE_BINARY_DIR}/conformance")
endif()
