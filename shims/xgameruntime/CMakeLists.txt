bx_add_shim_dll(xgameruntime xgameruntime xgameruntime.def
    xgameruntime.cpp
)
target_link_libraries(xgameruntime PRIVATE bx_runtime_veh kernel32)
