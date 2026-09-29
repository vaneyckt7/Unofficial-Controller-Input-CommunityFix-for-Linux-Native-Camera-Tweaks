cmake_minimum_required(VERSION 3.16)
project(linux_native_camera_tweaks)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(SDL2 REQUIRED)

file(GLOB_RECURSE SOURCES "src/*.c" "src/*.h")
add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE src ${SDL2_INCLUDE_DIRS})
target_compile_definitions(${PROJECT_NAME} PRIVATE _GNU_SOURCE)
target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS} m)

include(CTest)
if(BUILD_TESTING)
    add_executable(controller_input_tests tests/controller_input_test.c)
    target_include_directories(controller_input_tests PRIVATE src)
    target_link_libraries(controller_input_tests PRIVATE m)
    add_test(NAME controller_input_tests COMMAND controller_input_tests)

    add_executable(config_tests tests/config_test.c src/config.c)
    target_include_directories(config_tests PRIVATE src)
    target_link_libraries(config_tests PRIVATE m)
    add_test(NAME config_tests COMMAND config_tests)
    set_tests_properties(config_tests PROPERTIES
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config")

    add_executable(camera_state_tests tests/camera_state_test.c)
    target_include_directories(camera_state_tests PRIVATE src)
    target_link_libraries(camera_state_tests PRIVATE m)
    add_test(NAME camera_state_tests COMMAND camera_state_tests)

    add_executable(camera_timing_tests tests/camera_timing_test.c)
    target_include_directories(camera_timing_tests PRIVATE src)
    add_test(NAME camera_timing_tests COMMAND camera_timing_tests)

    add_executable(debug_wobble_tests tests/debug_wobble_test.c)
    target_include_directories(debug_wobble_tests PRIVATE src)
    target_link_libraries(debug_wobble_tests PRIVATE m)
    add_test(NAME debug_wobble_tests COMMAND debug_wobble_tests)

    add_executable(stick_owner_tests tests/stick_owner_test.c)
    target_include_directories(stick_owner_tests PRIVATE src)
    add_test(NAME stick_owner_tests COMMAND stick_owner_tests)

    add_executable(vtable_hook_tests tests/vtable_hook_test.c)
    target_include_directories(vtable_hook_tests PRIVATE src)
    target_compile_definitions(vtable_hook_tests PRIVATE _GNU_SOURCE)
    add_test(NAME vtable_hook_tests COMMAND vtable_hook_tests)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        add_executable(camera_input_hook_tests tests/camera_input_hook_test.c
            src/config.c src/debug_log.c src/file_stamp.c src/portable_paths.c src/offsets.c)
        target_include_directories(camera_input_hook_tests PRIVATE src ${SDL2_INCLUDE_DIRS})
        target_compile_definitions(camera_input_hook_tests PRIVATE _GNU_SOURCE)
        # The test's vtable must land in read-only .data.rel.ro, as BG3's does.
        target_link_options(camera_input_hook_tests PRIVATE -pie -Wl,-z,relro,-z,now)
        target_link_libraries(camera_input_hook_tests PRIVATE ${CMAKE_DL_LIBS} m)
        add_test(NAME camera_input_hook_tests COMMAND camera_input_hook_tests)
        set_tests_properties(camera_input_hook_tests PROPERTIES ENVIRONMENT
            "LNCT_DEBUG_LOG=${CMAKE_BINARY_DIR}/camera_input_hook_test.log;XDG_DATA_HOME=${CMAKE_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config")
    endif()

    add_executable(portable_paths_tests tests/portable_paths_test.c src/portable_paths.c)
    target_include_directories(portable_paths_tests PRIVATE src)
    add_test(NAME portable_paths_tests COMMAND portable_paths_tests)

    add_executable(file_stamp_tests tests/file_stamp_test.c src/file_stamp.c)
    target_include_directories(file_stamp_tests PRIVATE src)
    add_test(NAME file_stamp_tests COMMAND file_stamp_tests)

    add_executable(sdl_bindings_tests tests/sdl_bindings_test.c)
    target_include_directories(sdl_bindings_tests PRIVATE src ${SDL2_INCLUDE_DIRS})
    add_test(NAME sdl_bindings_tests COMMAND sdl_bindings_tests)
endif()
