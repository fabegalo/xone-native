# SPDX-License-Identifier: BSD-3-Clause
# GameOS/ERA titles: original AMD64 code mapped in-process and run natively;
# Xbox APIs are bound to host implementations. No CPU emulation.
if(NOT (WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
        CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$"))
    return()
endif()

add_library(xboxone_era STATIC ${PROJECT_SOURCE_DIR}/src/era_loader.cpp ${PROJECT_SOURCE_DIR}/src/era_kernelx.cpp
            ${PROJECT_SOURCE_DIR}/src/era_host.cpp ${PROJECT_SOURCE_DIR}/src/era_etwplus.cpp
            ${PROJECT_SOURCE_DIR}/src/era_d3d11x.cpp ${PROJECT_SOURCE_DIR}/src/era_winrt.cpp
            ${PROJECT_SOURCE_DIR}/src/era_xbox_services.cpp ${PROJECT_SOURCE_DIR}/src/era_winrt_object.cpp
            ${PROJECT_SOURCE_DIR}/src/era_xbox_system.cpp ${PROJECT_SOURCE_DIR}/src/era_xbox_deployment.cpp
            ${PROJECT_SOURCE_DIR}/src/era_core_application.cpp ${PROJECT_SOURCE_DIR}/src/era_audio.cpp)
target_link_libraries(xboxone_era PRIVATE xboxone_inspection d3d12 dxgi dxguid runtimeobject oleaut32 xmllite shlwapi ole32 bcrypt version)
# Diagnostic vtable thunks need /bigobj to stay within COFF section limits.
set_source_files_properties(${PROJECT_SOURCE_DIR}/src/era_d3d11x.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
target_include_directories(xboxone_era PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(xboxone_era PUBLIC cxx_std_20)
target_compile_options(xboxone_era PRIVATE /W4 /permissive-)

# Static TLS slot for the title; must be deployed next to the launcher executable.
add_library(xone-era-tls SHARED ${PROJECT_SOURCE_DIR}/src/era_tls_slot.cpp)
target_compile_options(xone-era-tls PRIVATE /W4 /permissive- /GS- /Zl)
target_link_libraries(xone-era-tls PRIVATE kernel32)
target_link_options(xone-era-tls PRIVATE /NODEFAULTLIB /ENTRY:era_tls_dll_main /INCLUDE:_tls_used
                    /MANIFEST:NO /INCREMENTAL:NO)
add_dependencies(xboxone_era xone-era-tls)

add_executable(xone-era ${PROJECT_SOURCE_DIR}/src/era_main.cpp)
target_link_libraries(xone-era PRIVATE xboxone_era)

if(XBOXONE_NATIVE_BUILD_TESTS)
    # Import library naming kernelx.dll; only the in-process loader can satisfy it.
    set(era_kernelx_lib "${CMAKE_CURRENT_BINARY_DIR}/era_kernelx_imports.lib")
    add_custom_command(OUTPUT "${era_kernelx_lib}"
        COMMAND "${CMAKE_AR}" /NOLOGO /MACHINE:X64 "/DEF:${PROJECT_SOURCE_DIR}/tests/era_kernelx_imports.def"
                "/OUT:${era_kernelx_lib}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tests/era_kernelx_imports.def" VERBATIM)
    add_custom_target(era_kernelx_imports DEPENDS "${era_kernelx_lib}")

    enable_language(RC)
    add_executable(era_component ${PROJECT_SOURCE_DIR}/tests/era_component.cpp ${PROJECT_SOURCE_DIR}/tests/era_component.rc)
    add_dependencies(era_component era_kernelx_imports)
    target_compile_options(era_component PRIVATE /GS- /Zl)
    target_link_libraries(era_component PRIVATE "${era_kernelx_lib}")
    target_link_options(era_component PRIVATE /NODEFAULTLIB /ENTRY:era_test_entry /SUBSYSTEM:CONSOLE
                        /MANIFEST:NO /INCREMENTAL:NO /DYNAMICBASE /FIXED:NO)

    add_library(era_module_component SHARED ${PROJECT_SOURCE_DIR}/tests/era_component.cpp)
    add_dependencies(era_module_component era_kernelx_imports)
    target_compile_definitions(era_module_component PRIVATE ERA_MODULE_FIXTURE)
    target_compile_options(era_module_component PRIVATE /GS- /Zl)
    target_link_libraries(era_module_component PRIVATE "${era_kernelx_lib}")
    target_link_options(era_module_component PRIVATE /NODEFAULTLIB /ENTRY:era_test_dll_main
                        /MANIFEST:NO /INCREMENTAL:NO /DYNAMICBASE /FIXED:NO)

    add_executable(era_tls_component ${PROJECT_SOURCE_DIR}/tests/era_tls_component.cpp)
    add_dependencies(era_tls_component era_kernelx_imports)
    target_compile_options(era_tls_component PRIVATE /GS- /Zl)
    target_link_libraries(era_tls_component PRIVATE "${era_kernelx_lib}")
    target_link_options(era_tls_component PRIVATE /NODEFAULTLIB /ENTRY:era_tls_entry /SUBSYSTEM:CONSOLE
                        /MANIFEST:NO /INCREMENTAL:NO /DYNAMICBASE /FIXED:NO /INCLUDE:_tls_used)

    add_executable(xboxone_era_loader_tests ${PROJECT_SOURCE_DIR}/tests/era_loader_tests.cpp)
    add_dependencies(xboxone_era_loader_tests era_module_component)
    target_link_libraries(xboxone_era_loader_tests PRIVATE xboxone_era)
    add_executable(xboxone_era_memory_tests ${PROJECT_SOURCE_DIR}/tests/era_memory_tests.cpp)
    add_executable(xboxone_era_audio_tests ${PROJECT_SOURCE_DIR}/tests/era_audio_tests.cpp)
    target_link_libraries(xboxone_era_audio_tests PRIVATE xboxone_era)
    add_test(NAME era_audio_regressions COMMAND xboxone_era_audio_tests)
    set_tests_properties(era_audio_regressions PROPERTIES TIMEOUT 10 LABELS "windows;synthetic;era")
    target_link_libraries(xboxone_era_memory_tests PRIVATE xboxone_era)
    add_test(NAME era_memory_hardware COMMAND xboxone_era_memory_tests hardware)
    add_test(NAME era_memory_warp COMMAND xboxone_era_memory_tests warp)
    set_tests_properties(era_memory_hardware PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77 LABELS "windows;synthetic;era;hardware")
    set_tests_properties(era_memory_warp PROPERTIES TIMEOUT 15 LABELS "windows;synthetic;era;software-gpu")
    add_test(NAME era_loader_regressions COMMAND xboxone_era_loader_tests $<TARGET_FILE:era_component>
             $<TARGET_FILE:era_tls_component> $<TARGET_FILE:era_module_component>)
    set_tests_properties(era_loader_regressions PROPERTIES TIMEOUT 20 LABELS "windows;synthetic;era")
    add_test(NAME era_check_cli COMMAND xone-era check $<TARGET_FILE:era_component>)
    set_tests_properties(era_check_cli PROPERTIES TIMEOUT 10 PASS_REGULAR_EXPRESSION "resolved 2, unresolved 0")
    if(Python3_Interpreter_FOUND)
        add_test(NAME era_diagnostic_cli
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/era_cli_tests.py"
                    $<TARGET_FILE:xone-era> $<TARGET_FILE:era_component> $<TARGET_FILE:xboxone_era_loader_tests>)
        set_tests_properties(era_diagnostic_cli PROPERTIES TIMEOUT 20 LABELS "windows;synthetic;era")
    endif()
endif()
