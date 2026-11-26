# #457: xgameruntime.dll reached by LOAD_LIBRARY_SEARCH_SYSTEM32 after bx-cli run stages it.
# Links no shim: the load under test is the dynamic one GDK titles make.
add_executable(test_system32_staging main.cpp)
target_link_libraries(test_system32_staging PRIVATE kernel32)
