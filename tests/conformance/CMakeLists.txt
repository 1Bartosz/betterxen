# Auto-discover all `test_*/CMakeLists.txt` directories so Tracks A-F can add new
# synthetic PE32+ conformance tests simultaneously with zero shared-file merge conflicts.
file(GLOB BX_CONFORMANCE_SUBDIRS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/test_*")
list(SORT BX_CONFORMANCE_SUBDIRS)

foreach(subdir ${BX_CONFORMANCE_SUBDIRS})
    if(IS_DIRECTORY "${subdir}" AND EXISTS "${subdir}/CMakeLists.txt")
        get_filename_component(name "${subdir}" NAME)
        add_subdirectory("${subdir}" "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    endif()
endforeach()
