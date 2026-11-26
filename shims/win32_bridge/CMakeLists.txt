bx_add_shim_dll(psm_appnotify_1_0 api-ms-win-core-psm-appnotify-l1-1-0 api-ms-win-core-psm-appnotify-l1-1-0.def
    psm_appnotify.cpp
)
target_link_libraries(psm_appnotify_1_0 PRIVATE bx_runtime_veh kernel32)

bx_add_shim_dll(psm_appnotify_1_1 api-ms-win-core-psm-appnotify-l1-1-1 api-ms-win-core-psm-appnotify-l1-1-1.def
    psm_appnotify.cpp
)
target_link_libraries(psm_appnotify_1_1 PRIVATE bx_runtime_veh kernel32)

bx_add_shim_dll(ntuser_msg_1_0 ext-ms-win-rtcore-ntuser-message-l1-1-0 ext-ms-win-rtcore-ntuser-message-l1-1-0.def
    empty_forwarder.cpp
)
target_link_libraries(ntuser_msg_1_0 PRIVATE user32)

bx_add_shim_dll(ntuser_msg_ansi_1_0 ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0 ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0.def
    empty_forwarder.cpp
)
target_link_libraries(ntuser_msg_ansi_1_0 PRIVATE user32)

bx_add_shim_dll(ntuser_win_1_0 ext-ms-win-rtcore-ntuser-window-l1-1-0 ext-ms-win-rtcore-ntuser-window-l1-1-0.def
    empty_forwarder.cpp
)
target_link_libraries(ntuser_win_1_0 PRIVATE user32)

bx_add_shim_dll(ntuser_win_ansi_1_0 ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0 ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.def
    empty_forwarder.cpp
)
target_link_libraries(ntuser_win_ansi_1_0 PRIVATE user32)
