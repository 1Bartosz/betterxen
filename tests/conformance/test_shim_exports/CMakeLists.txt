add_executable(test_shim_exports main.cpp)
target_include_directories(test_shim_exports PRIVATE "${BX_ABI_INCLUDE_DIR}")
target_link_libraries(test_shim_exports PRIVATE bx_runtime_veh kernel32)
