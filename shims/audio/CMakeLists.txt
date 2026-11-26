bx_add_shim_dll(acphal AcpHal AcpHal.def
    acphal.cpp
)
target_link_libraries(acphal PRIVATE bx_runtime_veh kernel32)

bx_add_shim_dll(xapu xapu xapu.def
    xapu.cpp
)
target_link_libraries(xapu PRIVATE bx_runtime_veh kernel32)

bx_add_shim_dll(chad chad chad.def
    chad.cpp
)
target_link_libraries(chad PRIVATE bx_runtime_veh kernel32)
