cmake_minimum_required(VERSION 3.16)
project(companion_host_tests C)
set(CMAKE_C_STANDARD 11)
# cJSON and Unity come from ESP-IDF: $IDF_PATH, else the default ~/esp/esp-idf.
if(DEFINED ENV{IDF_PATH})
    set(IDF $ENV{IDF_PATH})
elseif(EXISTS $ENV{HOME}/esp/esp-idf/components/json/cJSON/cJSON.c)
    set(IDF $ENV{HOME}/esp/esp-idf)
else()
    message(FATAL_ERROR "ESP-IDF not found: set IDF_PATH (or run . ~/esp/esp-idf/export.sh)")
endif()
set(COMP ${CMAKE_CURRENT_SOURCE_DIR}/../../main/companion)

add_library(cjson STATIC ${IDF}/components/json/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC ${IDF}/components/json/cJSON)
add_library(unity STATIC ${IDF}/components/unity/unity/src/unity.c)
target_include_directories(unity PUBLIC ${IDF}/components/unity/unity/src)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_CONFIG_H)
target_include_directories(unity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/unity_cfg)
add_library(companion STATIC ${COMP}/cc_sessions.c ${COMP}/cc_cmd.c ${COMP}/cc_watch.c)
target_include_directories(companion PUBLIC ${COMP})
target_link_libraries(companion PUBLIC cjson)
target_compile_options(companion PRIVATE -Wall -Wextra)

# Pure modules under main/sys (no ESP-IDF headers). Later tasks add theirs here.
set(MAIN ${CMAKE_CURRENT_SOURCE_DIR}/../../main)
add_library(pure STATIC ${MAIN}/sys/plog.c ${MAIN}/sys/vitals.c ${MAIN}/sys/power_rules.c ${MAIN}/sys/hib_rules.c ${MAIN}/ui/hotkeys.c ${MAIN}/input/key_log.c ${MAIN}/ssh/kh_rules.c
    ${MAIN}/companion/kh_carry.c ${MAIN}/settings/settings_migrate.c ${MAIN}/net/ts_host.c)
target_include_directories(pure PUBLIC ${MAIN} ${MAIN}/sys)
target_link_libraries(pure PUBLIC companion)
target_compile_options(pure PRIVATE -Wall -Wextra)

enable_testing()
foreach(t test_cc_sessions test_cc_cmd test_cc_watch)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} companion unity)
    target_compile_definitions(${t} PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    add_test(NAME ${t} COMMAND ${t})
endforeach()

add_executable(test_plog test_plog.c)
target_link_libraries(test_plog pure unity)
add_test(NAME test_plog COMMAND test_plog)

add_executable(test_vitals test_vitals.c)
target_link_libraries(test_vitals pure unity)
add_test(NAME test_vitals COMMAND test_vitals)

add_executable(test_power_rules test_power_rules.c)
target_link_libraries(test_power_rules pure unity)
add_test(NAME test_power_rules COMMAND test_power_rules)

add_executable(test_hib_rules test_hib_rules.c)
target_link_libraries(test_hib_rules pure unity)
add_test(NAME test_hib_rules COMMAND test_hib_rules)

add_executable(test_hotkeys test_hotkeys.c)
target_link_libraries(test_hotkeys pure unity)
add_test(NAME test_hotkeys COMMAND test_hotkeys)

add_executable(test_key_log test_key_log.c)
target_link_libraries(test_key_log pure unity)
add_test(NAME test_key_log COMMAND test_key_log)

add_executable(test_kh_rules test_kh_rules.c)
target_link_libraries(test_kh_rules pure unity)
add_test(NAME test_kh_rules COMMAND test_kh_rules)

foreach(t test_kh_carry test_settings_migrate test_ts_host)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} pure unity)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# Prints cc_cmd_watch's output for firmware/tools/test_watch_loop.py (not a test itself).
add_executable(cc_cmd_print cc_cmd_print.c)
target_link_libraries(cc_cmd_print companion)
