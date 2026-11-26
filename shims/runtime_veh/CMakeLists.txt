bx_add_shim_dll(bx_runtime_veh bx_runtime_veh bx_runtime_veh.def
    runtime_veh.cpp
)
target_link_libraries(bx_runtime_veh PRIVATE kernel32)
