cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS components)

# Plan 6 (spec P6-5): the app version is <version.txt>+<git short hash>[.dirty], e.g. 1.6.0+80ab00e.dirty. A
# PROJECT_VER given on the command line (-D PROJECT_VER=1.6.0+testb, for test builds) wins. Worked out at configure
# time, so the hash can be stale after a commit without a reconfigure: it is a label only (fw_offer compares the
# ELF SHA-256 in the app descriptor).
if(NOT DEFINED PROJECT_VER)
    file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/version.txt _cc_ver LIMIT_COUNT 1)
    string(STRIP "${_cc_ver}" _cc_ver)
    execute_process(COMMAND git describe --always --dirty --abbrev=7
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_VARIABLE _cc_git RESULT_VARIABLE _cc_git_rc
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(_cc_hash "nogit")
    if(_cc_git_rc EQUAL 0 AND _cc_git)
        # "<hash>[-dirty]" with no tags, "<tag>-<n>-g<hash>[-dirty]" with one
        if(_cc_git MATCHES "-g([0-9a-f]+)(-dirty)?$")
            set(_cc_hash "${CMAKE_MATCH_1}")
        elseif(_cc_git MATCHES "^([0-9a-f]+)(-dirty)?$")
            set(_cc_hash "${CMAKE_MATCH_1}")
        endif()
        if(_cc_git MATCHES "-dirty$")
            string(APPEND _cc_hash ".dirty")
        endif()
    endif()
    set(PROJECT_VER "${_cc_ver}+${_cc_hash}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(claude_companion)

# Plan 6 (spec §6.5): print the app slot's headroom after every build, and warn under 1 MB. It fails the build only
# when the image doesn't fit, which ESP-IDF already enforces.
idf_build_get_property(_cc_python PYTHON)
add_custom_target(image_size_check ALL
    COMMAND ${_cc_python} ${CMAKE_CURRENT_LIST_DIR}/tools/check_image_size.py
            ${CMAKE_CURRENT_LIST_DIR}/partitions.csv ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
    VERBATIM)
add_dependencies(image_size_check app)
