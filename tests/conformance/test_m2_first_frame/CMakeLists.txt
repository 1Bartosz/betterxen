add_executable(test_m2_first_frame main.cpp)
target_include_directories(test_m2_first_frame PRIVATE
    "${BX_ABI_INCLUDE_DIR}"
    "${BX_ROOT_DIR}/shader-transpiler/crates/d3dmetal-harness/probe/shaders"
)
target_link_libraries(test_m2_first_frame PRIVATE d3d12_xs xmem bx_runtime_veh user32)
target_compile_options(test_m2_first_frame PRIVATE -fexceptions)
