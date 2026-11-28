add_executable(test_xcrdapi_abi main.cpp)
target_compile_definitions(test_xcrdapi_abi PRIVATE WIDL_EXPLICIT_AGGREGATE_RETURNS)
target_include_directories(test_xcrdapi_abi PRIVATE "${BX_ROOT_DIR}/shims/xcrdapi/dstorage")
target_link_libraries(test_xcrdapi_abi PRIVATE xcrdapi kernel32 uuid)
