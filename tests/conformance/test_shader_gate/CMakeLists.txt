# M2 Shader Format Gate (#466): Create*PipelineState through d3d12_xs.dll with DXIL, ACCC-only and
# constraint-carrying shaders. Needs bx_shader_bridge.dll (shader-transpiler/crates/dxbc-accc/guest/build.sh,
# #445) next to the shims at run time. The DXIL blobs are existing DXC 1.9.2609 outputs in the tree.
add_executable(test_shader_gate main.cpp)
target_include_directories(test_shader_gate PRIVATE
    "${BX_ABI_INCLUDE_DIR}"
    "${BX_ROOT_DIR}/shader-transpiler/crates/d3dmetal-harness/probe/shaders"
    "${BX_ROOT_DIR}/tests/conformance/test_d3dmetal_gpu_limits/shaders"
)
target_link_libraries(test_shader_gate PRIVATE d3d12_xs kernel32)
