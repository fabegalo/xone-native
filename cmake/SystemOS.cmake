# SPDX-License-Identifier: BSD-3-Clause
# Independent bootstrap; deliberately has no dependency on presentation code.
add_library(xboxone_compatibility STATIC ${PROJECT_SOURCE_DIR}/src/compatibility.cpp)
target_include_directories(xboxone_compatibility PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(xboxone_compatibility PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(xboxone_compatibility PRIVATE /W4 /permissive-)
else()
    target_compile_options(xboxone_compatibility PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(xboxone_systemos STATIC ${PROJECT_SOURCE_DIR}/src/bootstrap.cpp)
target_sources(xboxone_systemos PRIVATE
    ${PROJECT_SOURCE_DIR}/src/appmodel_manifest.cpp
    ${PROJECT_SOURCE_DIR}/src/appmodel.cpp
    ${PROJECT_SOURCE_DIR}/src/appmodel_activation.cpp)
if(WIN32)
    target_link_libraries(xboxone_systemos PRIVATE ole32 shlwapi xmllite uuid)
endif()
target_link_libraries(xboxone_systemos PUBLIC xboxone_inspection xboxone_compatibility)
target_compile_features(xboxone_systemos PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(xboxone_systemos PRIVATE /W4 /permissive-)
else()
    target_compile_options(xboxone_systemos PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(xboxone-systemos ${PROJECT_SOURCE_DIR}/src/bootstrap_main.cpp)
target_link_libraries(xboxone-systemos PRIVATE xboxone_systemos)

if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
    add_executable(xboxone_xvd_mount_probe WIN32 ${PROJECT_SOURCE_DIR}/tools/xvd_mount_probe.cpp)
    target_compile_features(xboxone_xvd_mount_probe PRIVATE cxx_std_20)
    target_compile_options(xboxone_xvd_mount_probe PRIVATE /W4 /permissive-)
    target_link_libraries(xboxone_xvd_mount_probe PRIVATE advapi32)
    target_link_options(xboxone_xvd_mount_probe PRIVATE /ENTRY:wmainCRTStartup)
endif()

if(XBOXONE_NATIVE_BUILD_TESTS)
    add_executable(xboxone_appmodel_path_tests ${PROJECT_SOURCE_DIR}/tests/appmodel_path_tests.cpp)
    target_link_libraries(xboxone_appmodel_path_tests PRIVATE xboxone_systemos)
    add_test(NAME appmodel_path_contracts COMMAND xboxone_appmodel_path_tests)
    set_tests_properties(appmodel_path_contracts PROPERTIES TIMEOUT 10)
    if(DEFINED ENV{GITHUB_ACTIONS} AND NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "CI requires Python regression coverage")
    endif()
    add_executable(xboxone_bootstrap_tests ${PROJECT_SOURCE_DIR}/tests/bootstrap_tests.cpp)
    target_link_libraries(xboxone_bootstrap_tests PRIVATE xboxone_systemos)
    add_test(NAME bootstrap_regressions COMMAND xboxone_bootstrap_tests)
    set_tests_properties(bootstrap_regressions PROPERTIES TIMEOUT 20)
    if(Python3_Interpreter_FOUND)
        add_test(NAME pe_dependency_regressions
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/pe_dependency_tests.py"
                    $<TARGET_FILE:xone-native> $<TARGET_FILE:xboxone-systemos>)
        set_tests_properties(pe_dependency_regressions PROPERTIES TIMEOUT 30)
    endif()
    if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        if(Python3_Interpreter_FOUND)
            add_test(NAME xvd_probe_safety
                COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/xvd_probe_tests.py"
                        $<TARGET_FILE:xboxone_xvd_mount_probe>)
            set_tests_properties(xvd_probe_safety PROPERTIES TIMEOUT 10 LABELS "windows;no-mount;no-deployment")
        endif()
        add_executable(xboxone_appmodel_activation_tests ${PROJECT_SOURCE_DIR}/tests/appmodel_activation_tests.cpp)
        target_include_directories(xboxone_appmodel_activation_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(xboxone_appmodel_activation_tests PRIVATE xboxone_systemos)
        add_test(NAME appmodel_activation_failure COMMAND xboxone_appmodel_activation_tests)
        add_test(NAME appmodel_process_observation COMMAND xboxone_appmodel_activation_tests
                 $<TARGET_FILE:bootstrap_component_ok> $<TARGET_FILE:bootstrap_component_hang>
                 $<TARGET_FILE:bootstrap_component_fail>)
        set_tests_properties(appmodel_process_observation PROPERTIES TIMEOUT 20 LABELS "windows;synthetic;no-deployment")
        set_tests_properties(appmodel_activation_failure PROPERTIES TIMEOUT 20 LABELS "windows;appmodel;no-deployment")
        add_executable(appmodel_component ${PROJECT_SOURCE_DIR}/tests/appmodel_component.cpp)
        target_compile_options(appmodel_component PRIVATE /GS- /Zl)
        target_link_libraries(appmodel_component PRIVATE kernel32)
        target_link_options(appmodel_component PRIVATE /NODEFAULTLIB /ENTRY:appmodel_test_entry
                            /SUBSYSTEM:WINDOWS /MANIFEST:NO /INCREMENTAL:NO)
        if(Python3_Interpreter_FOUND)
            add_test(NAME appmodel_manifest_regressions
                COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/appmodel_tests.py"
                        $<TARGET_FILE:xboxone-systemos> $<TARGET_FILE:appmodel_component>
                        "${CMAKE_CURRENT_BINARY_DIR}/appmodel-evidence.json")
            set_tests_properties(appmodel_manifest_regressions PROPERTIES TIMEOUT 60 LABELS "windows;appmodel;no-deployment")
        endif()
        foreach(variant IN ITEMS ok hang fail)
            add_executable(bootstrap_component_${variant} ${PROJECT_SOURCE_DIR}/tests/bootstrap_component.cpp)
            target_compile_options(bootstrap_component_${variant} PRIVATE /GS- /Zl)
            target_link_libraries(bootstrap_component_${variant} PRIVATE kernel32)
            target_link_options(bootstrap_component_${variant} PRIVATE /NODEFAULTLIB /ENTRY:bootstrap_test_entry
                                /SUBSYSTEM:CONSOLE /MANIFEST:NO /INCREMENTAL:NO)
        endforeach()
        target_compile_definitions(bootstrap_component_hang PRIVATE BOOTSTRAP_TEST_HANG)
        target_compile_definitions(bootstrap_component_fail PRIVATE BOOTSTRAP_TEST_FAIL)
        add_executable(bootstrap_component_tls ${PROJECT_SOURCE_DIR}/tests/bootstrap_tls_component.cpp)
        target_compile_options(bootstrap_component_tls PRIVATE /GS- /Zl)
        target_link_libraries(bootstrap_component_tls PRIVATE kernel32)
        target_link_options(bootstrap_component_tls PRIVATE /NODEFAULTLIB /ENTRY:bootstrap_tls_entry
                            /SUBSYSTEM:CONSOLE /MANIFEST:NO /INCREMENTAL:NO /INCLUDE:_tls_used)
        add_test(NAME bootstrap_native_execution COMMAND xboxone_bootstrap_tests
                 $<TARGET_FILE:bootstrap_component_ok> $<TARGET_FILE:bootstrap_component_hang>
                 $<TARGET_FILE:bootstrap_component_fail>)
        set_tests_properties(bootstrap_native_execution PROPERTIES TIMEOUT 20)
        if(Python3_Interpreter_FOUND)
            add_test(NAME bootstrap_cli_regressions
                COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/bootstrap_cli_tests.py"
                        $<TARGET_FILE:xboxone-systemos> $<TARGET_FILE:bootstrap_component_ok>
                        "${CMAKE_CURRENT_BINARY_DIR}/m1-bootstrap-evidence.json" $<TARGET_FILE:xone-native>
                        $<TARGET_FILE:bootstrap_component_tls>)
            set_tests_properties(bootstrap_cli_regressions PROPERTIES TIMEOUT 20)
        endif()
    endif()
endif()
