bx_add_shim_dll(gameinput GameInput GameInput.def
    gameinput.cpp
)
target_link_libraries(gameinput PRIVATE bx_runtime_veh kernel32)
