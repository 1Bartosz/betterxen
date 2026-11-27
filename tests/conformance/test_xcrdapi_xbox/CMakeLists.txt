# Synthetic conformance test for Xbox Series DirectStorage COM interfaces (IDStorage*X).
add_executable(test_xcrdapi_xbox main.cpp)
target_include_directories(test_xcrdapi_xbox PRIVATE "${BX_ABI_INCLUDE_DIR}")
target_link_libraries(test_xcrdapi_xbox PRIVATE xcrdapi bx_runtime_veh kernel32)
