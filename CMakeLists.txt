cmake_minimum_required(VERSION 3.21)
project(xone_native VERSION 0.1.0 LANGUAGES CXX)

option(XBOXONE_NATIVE_BUILD_TESTS "Build M0 regression tests" ON)

add_library(xboxone_inspection STATIC
    src/inspection.cpp
    src/inventory.cpp
)
target_include_directories(xboxone_inspection PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(xboxone_inspection PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(xboxone_inspection PRIVATE /W4 /permissive-)
else()
    target_compile_options(xboxone_inspection PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(xboxone_presentation STATIC
    src/presentation.cpp
    src/presentation_d3d12.cpp
    src/frame_pipe.cpp
)
target_include_directories(xboxone_presentation PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(xboxone_presentation PUBLIC cxx_std_20)
target_link_libraries(xboxone_presentation PRIVATE xboxone_inspection)
if(MSVC)
    target_compile_options(xboxone_presentation PRIVATE /W4 /permissive-)
else()
    target_compile_options(xboxone_presentation PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
    target_link_libraries(xboxone_presentation PRIVATE user32 d3d12 dxgi dxguid ole32)
endif()

add_executable(xone-native src/main.cpp)
target_link_libraries(xone-native PRIVATE xboxone_inspection)

if(XBOXONE_NATIVE_BUILD_TESTS)
    enable_testing()
    add_executable(xboxone_inspection_tests tests/inspection_tests.cpp)
    target_link_libraries(xboxone_inspection_tests PRIVATE xboxone_inspection)
    add_test(NAME inspection_regressions COMMAND xboxone_inspection_tests)
    add_executable(xboxone_inventory_tests tests/inventory_tests.cpp)
    target_include_directories(xboxone_inventory_tests PRIVATE src)
    target_link_libraries(xboxone_inventory_tests PRIVATE xboxone_inspection)
    add_test(NAME inventory_regressions COMMAND xboxone_inventory_tests)
    add_executable(xboxone_presentation_tests tests/presentation_tests.cpp)
    target_link_libraries(xboxone_presentation_tests PRIVATE xboxone_presentation)
    add_test(NAME presentation_regressions COMMAND xboxone_presentation_tests)
    if(WIN32)
        add_executable(xboxone_frame_pipe_tests tests/frame_pipe_tests.cpp)
        target_include_directories(xboxone_frame_pipe_tests PRIVATE src)
        target_link_libraries(xboxone_frame_pipe_tests PRIVATE xboxone_presentation xboxone_inspection)
        add_test(NAME frame_pipe_regressions COMMAND xboxone_frame_pipe_tests)
        set_tests_properties(frame_pipe_regressions PROPERTIES TIMEOUT 45 LABELS "windows;synthetic;no-deployment")
    endif()
    add_test(NAME cli_help COMMAND xone-native --help)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME update_zip_and_cli_regressions
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/update_zip_tests.py"
                    $<TARGET_FILE:xone-native>)
        add_test(NAME systemos_preflight_regressions
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/systemos_preflight_tests.py"
                    $<TARGET_FILE:xone-native>)
    endif()
endif()

include(cmake/SystemOS.cmake)
include(cmake/Era.cmake)

# Shared console state, library catalog, session supervisor and the approved
# local dashboard front-end (frames: local_recreation, never SystemOS).
add_library(xboxone_console STATIC
    src/console_state.cpp
    src/title_catalog.cpp
    src/session_supervisor.cpp
    src/dashboard.cpp
    src/dashboard_render.cpp
)
target_include_directories(xboxone_console PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(xboxone_console PUBLIC cxx_std_20)
target_link_libraries(xboxone_console PUBLIC xboxone_inspection PRIVATE xboxone_systemos)
if(WIN32)
    target_sources(xboxone_console PRIVATE src/dashboard_windows.cpp)
    target_link_libraries(xboxone_console PRIVATE xboxone_presentation user32 gdi32 ole32 shell32 windowscodecs winmm)
endif()
if(MSVC)
    target_compile_options(xboxone_console PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(xboxone_console PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xboxone_console PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(xone-native PRIVATE xboxone_console)

if(XBOXONE_NATIVE_BUILD_TESTS)
    add_executable(xboxone_dashboard_tests tests/dashboard_tests.cpp)
    target_include_directories(xboxone_dashboard_tests PRIVATE src)
    target_link_libraries(xboxone_dashboard_tests PRIVATE xboxone_console)
    if(MSVC)
        target_compile_options(xboxone_dashboard_tests PRIVATE /utf-8)
    endif()
    add_test(NAME dashboard_regressions COMMAND xboxone_dashboard_tests)
    set_tests_properties(dashboard_regressions PROPERTIES TIMEOUT 20 LABELS "synthetic;dashboard")
    if(TARGET xone-era)
        add_executable(xboxone_dashboard_era_tests tests/dashboard_era_tests.cpp)
        target_include_directories(xboxone_dashboard_era_tests PRIVATE src)
        target_link_libraries(xboxone_dashboard_era_tests PRIVATE xboxone_console gdi32 ole32)
        target_compile_options(xboxone_dashboard_era_tests PRIVATE /utf-8)
        add_dependencies(xboxone_dashboard_era_tests xone-era era_component era_suspend_component)
        add_test(NAME dashboard_era_lifecycle
            COMMAND xboxone_dashboard_era_tests $<TARGET_FILE:xone-era> $<TARGET_FILE:era_component>
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/appmodel/AppxManifest.xml" $<TARGET_FILE:era_suspend_component>)
        set_tests_properties(dashboard_era_lifecycle PROPERTIES TIMEOUT 60 LABELS "windows;synthetic;era;dashboard")
        add_executable(xboxone_era_system_ui_tests tests/era_system_ui_tests.cpp)
        target_include_directories(xboxone_era_system_ui_tests PRIVATE src)
        target_link_libraries(xboxone_era_system_ui_tests PRIVATE xboxone_era)
        target_compile_options(xboxone_era_system_ui_tests PRIVATE /utf-8)
        add_test(NAME era_system_ui_lifecycle COMMAND xboxone_era_system_ui_tests
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/appmodel/AppxManifest.xml")
        set_tests_properties(era_system_ui_lifecycle PROPERTIES TIMEOUT 30 LABELS "windows;synthetic;era")
    endif()
endif()
