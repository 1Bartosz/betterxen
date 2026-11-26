# #437: which module serves the six API-set import names under CrossOver?
#
# Links statically (PE import directory, not LoadLibrary) against the import
# library of each win32_bridge shim. The ntuser forwarder libraries come first so
# the ntuser exports bind to the shim import libraries instead of user32's.
add_executable(test_apiset_resolution main.cpp)
target_link_libraries(test_apiset_resolution PRIVATE
    ntuser_win_ansi_1_0
    ntuser_win_1_0
    ntuser_msg_ansi_1_0
    ntuser_msg_1_0
    psm_appnotify_1_0
    psm_appnotify_1_1
    user32
    kernel32
)
