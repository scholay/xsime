cmake_minimum_required(VERSION 3.27)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RIMES_PRODUCT_VERSION)
string(STRIP "${RIMES_PRODUCT_VERSION}" RIMES_PRODUCT_VERSION)
if(NOT RIMES_PRODUCT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain MAJOR.MINOR.PATCH")
endif()
project(RIMESWindows VERSION ${RIMES_PRODUCT_VERSION} LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The native RIMES frontend currently builds on Windows only.")
endif()

option(RIMES_BUILD_TESTS "Build the native Windows foundation tests" ON)
option(RIMES_TSF_DIAGNOSTICS
       "Write privacy-preserving TSF stage codes to the user's temp directory"
       OFF)

include(CTest)
enable_language(RC)
configure_file(src/product_version.hpp.in generated/rimes_version.hpp @ONLY)

# Keep every runnable artifact together so architecture checks, registration,
# and later packaging never need to guess target-specific subdirectories.
foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${configuration}
        "${CMAKE_BINARY_DIR}/$<CONFIG>")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${configuration}
        "${CMAKE_BINARY_DIR}/$<CONFIG>")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${configuration}
        "${CMAKE_BINARY_DIR}/$<CONFIG>")
endforeach()

function(rimes_harden_target target)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX
            /permissive-
            /utf-8
            /sdl
            /guard:cf
            /Zc:__cplusplus
            /Zc:inline
            /Zc:preprocessor
        )
        target_link_options(${target} PRIVATE
            /DYNAMICBASE
            /NXCOMPAT
            /guard:cf
        )
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endfunction()

# TSF runs inside applications that may have already loaded an older MSVC CRT.
# Compile its whole private object-code closure with /MT (/MTd in Debug), rather
# than depending on whichever MSVCP140.dll the host happened to load first.
# cmake_minimum_required(3.27) sets CMP0091 to NEW before project() above.
function(rimes_use_static_runtime target)
    if(MSVC)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()

# MinGW does not infer its Unicode console CRT startup object from a wmain
# definition. MSVC selects it automatically, so keep this narrowly scoped to
# executable targets whose source entry point is wmain.
function(rimes_use_unicode_console_entrypoint target)
    if(MINGW)
        target_link_options(${target} PRIVATE -municode)
    endif()
endfunction()

# librime receives UTF-8 paths, including calls into dependencies that use
# narrow CRT/Win32 filesystem APIs. Opt in per standalone engine process;
# the in-process TSF DLL must never change its application's code page.
function(rimes_use_utf8_paths target)
    if(MSVC)
        target_sources(${target} PRIVATE src/utf8_paths.manifest)
    else()
        configure_file(src/utf8_paths.rc.in generated/utf8_paths.rc @ONLY)
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/utf8_paths.rc")
    endif()
endfunction()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../Shared/SchemeImport" "scheme-import")

set(rimes_core_sources
    src/core/broker_protocol.cpp
)
add_library(RimesCore STATIC ${rimes_core_sources})
target_include_directories(RimesCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
)
rimes_harden_target(RimesCore)

# Keep the Broker's /MD protocol objects separate from the TSF's /MT objects.
# The only cross-process boundary is the serialized named-pipe protocol; no
# C++ containers or CRT-owned allocations cross between these two libraries.
add_library(RimesTsfCore STATIC ${rimes_core_sources})
target_include_directories(RimesTsfCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
)
rimes_harden_target(RimesTsfCore)
rimes_use_static_runtime(RimesTsfCore)

add_library(RimesEngine STATIC
    src/engine/rime_engine.cpp
    src/engine/rime_snapshot.cpp
)
target_include_directories(RimesEngine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine"
)
target_link_libraries(RimesEngine PUBLIC RimesCore)
rimes_harden_target(RimesEngine)

add_library(RimesWorkbench STATIC
    src/workbench/model.cpp
    src/workbench/chord_profile.cpp
    src/workbench/official_plugins.cpp
    src/workbench/provider.cpp
    src/workbench/connection_secrets.cpp
    src/workbench/parity_settings.cpp
    src/workbench/connection_probe.cpp
    src/workbench/codex_cli.cpp src/workbench/local_translation.cpp
    src/workbench/runtime.cpp
    src/workbench/settings_ui.cpp src/workbench/application_update.cpp
    src/workbench/user_lexicon.cpp src/workbench/user_lexicon_ui.cpp
    src/workbench/metrics.cpp
    src/workbench/metrics_articles.cpp
    src/workbench/metrics_ui.cpp
    src/workbench/window.cpp
    src/workbench/local_gateway.cpp
    src/workbench/buffer_anchor.cpp
    src/peripherals/store.cpp
    src/peripherals/terminal.cpp
    src/peripherals/window.cpp
    src/peripherals/media.cpp
    src/peripherals/clipboard_actions.cpp
    src/peripherals/capture.cpp
    src/peripherals/messages.cpp
)
target_link_libraries(RimesWorkbench PUBLIC RimesCore RimesEngine RimesSchemeImport comdlg32 winhttp ws2_32 bcrypt crypt32 advapi32 user32 d2d1 dwrite shell32 wtsapi32 ole32 oleaut32 uuid comctl32 gdi32 msimg32 dwmapi windowscodecs icu normaliz imm32)
target_include_directories(RimesWorkbench SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
rimes_harden_target(RimesWorkbench)

add_executable(RimesBroker
    src/schemes/scheme_manager.cpp
    src/broker/autostart.cpp
    src/broker/broker_connection.cpp
    src/broker/broker_options.cpp
    src/broker/default_paths.cpp
    src/broker/key_translation.cpp
    src/broker/main.cpp
    src/broker/named_pipe_server.cpp
    src/broker/single_instance.cpp
    src/broker/ui_command.cpp
    src/broker/win32_security.cpp
)
target_include_directories(RimesBroker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/broker"
)
target_link_libraries(RimesBroker PRIVATE
    RimesCore
    RimesEngine
    RimesWorkbench
    advapi32
)
rimes_harden_target(RimesBroker)
rimes_use_unicode_console_entrypoint(RimesBroker)
rimes_use_utf8_paths(RimesBroker)
add_custom_command(TARGET RimesBroker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../translation" "$<TARGET_FILE_DIR:RimesBroker>/translation")

add_library(RimesTsf SHARED
    src/tsf/BrokerClient.cpp
    src/tsf/CandidateWindow.cpp
    src/tsf/ClassFactory.cpp
    src/tsf/DisplayAttribute.cpp
    src/tsf/DllMain.cpp
    src/tsf/Exports.cpp
    src/tsf/ModuleState.cpp
    src/tsf/TextService.cpp
    src/tsf/RimesTsf.def
)
target_include_directories(RimesTsf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/tsf"
)
target_link_libraries(RimesTsf PRIVATE
    RimesTsfCore
    advapi32
    gdi32
    msimg32
    ole32
    oleaut32
    user32
    uuid
)
set_target_properties(RimesTsf PROPERTIES
    PREFIX ""
    OUTPUT_NAME "RimesTsf"
)
rimes_harden_target(RimesTsf)
rimes_use_static_runtime(RimesTsf)
if(RIMES_TSF_DIAGNOSTICS)
    target_sources(RimesTsf PRIVATE src/tsf/Diagnostics.cpp)
    target_compile_definitions(RimesTsf PRIVATE RIMES_TSF_DIAGNOSTICS=1)
endif()
if(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    # The .def deliberately publishes undecorated COM entry points. On Win32,
    # MinGW resolves them to the stdcall-decorated implementations; spelling
    # out the supported fixup keeps that intentional alias warning-free.
    target_link_options(RimesTsf PRIVATE
        "LINKER:--enable-stdcall-fixup"
    )
endif()

add_subdirectory(src/registrar)
rimes_harden_target(RimesRegistrar)
rimes_use_unicode_console_entrypoint(RimesRegistrar)

if(RIMES_BUILD_TESTS)
    # Same implementation and security checks, but a compile-time endpoint
    # identity that cannot connect to or retire the user's daily Broker.
    get_target_property(e2e_broker_sources RimesBroker SOURCES)
    add_executable(RimesE2EBroker ${e2e_broker_sources})
    target_include_directories(RimesE2EBroker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/broker")
    target_compile_definitions(RimesE2EBroker PRIVATE RIMES_E2E_TEST_IDENTITY=1)
    target_link_libraries(RimesE2EBroker PRIVATE RimesCore RimesEngine RimesWorkbench advapi32)
    rimes_harden_target(RimesE2EBroker)
    rimes_use_unicode_console_entrypoint(RimesE2EBroker)
    rimes_use_utf8_paths(RimesE2EBroker)
    add_test(NAME RimesE2EBrokerEndpoint COMMAND RimesE2EBroker --print-endpoint)
    set_tests_properties(RimesE2EBrokerEndpoint PROPERTIES
        PASS_REGULAR_EXPRESSION "RIMES[.]E2E[.]Broker[.]v2[.]session-" TIMEOUT 10)

    add_executable(RimesCaretRegressionTests
        src/e2e/caret_regression.cpp
        src/e2e/fake_tsf.cpp
        src/tsf/BrokerClient.cpp
        src/tsf/CandidateWindow.cpp
        src/tsf/DisplayAttribute.cpp
        src/tsf/ModuleState.cpp
        src/tsf/TextService.cpp
    )
    target_include_directories(RimesCaretRegressionTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/e2e"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tsf"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(RimesCaretRegressionTests PRIVATE
        RimesCore
        advapi32
        gdi32
        msimg32
        ole32
        oleaut32
        user32
        uuid
    )
    rimes_harden_target(RimesCaretRegressionTests)
    add_test(NAME RimesCaretRegression COMMAND RimesCaretRegressionTests)
    get_target_property(ui_test_sources RimesCaretRegressionTests SOURCES)
    list(REMOVE_ITEM ui_test_sources src/e2e/caret_regression.cpp)
    add_executable(RimesFirstKeyTests src/e2e/first_key_tests.cpp ${ui_test_sources})
    target_include_directories(RimesFirstKeyTests PRIVATE src/e2e src/tsf src)
    target_link_libraries(RimesFirstKeyTests PRIVATE RimesTsfCore advapi32 gdi32 msimg32 ole32 oleaut32 user32 uuid)
    rimes_harden_target(RimesFirstKeyTests)
    rimes_use_static_runtime(RimesFirstKeyTests)
    add_test(NAME RimesFirstKey COMMAND RimesFirstKeyTests)
    add_executable(RimesCandidateUITests src/e2e/candidate_ui_tests.cpp ${ui_test_sources})
    target_include_directories(RimesCandidateUITests PRIVATE src/e2e src/tsf src)
    target_link_libraries(RimesCandidateUITests PRIVATE RimesTsfCore advapi32 gdi32 msimg32 ole32 oleaut32 user32 uuid)
    rimes_harden_target(RimesCandidateUITests)
    rimes_use_static_runtime(RimesCandidateUITests)
    add_test(NAME RimesCandidateUI COMMAND RimesCandidateUITests)
    add_executable(RimesSandboxProbe src/e2e/sandbox_probe.cpp ${ui_test_sources})
    target_include_directories(RimesSandboxProbe PRIVATE src/e2e src/tsf src)
    target_link_libraries(RimesSandboxProbe PRIVATE RimesTsfCore advapi32 gdi32 msimg32 ole32 oleaut32 user32 uuid)
    rimes_harden_target(RimesSandboxProbe)
    rimes_use_static_runtime(RimesSandboxProbe)
    target_compile_definitions(RimesSandboxProbe PRIVATE RIMES_E2E_TEST_IDENTITY=1)
    add_executable(RimesAppContainerLauncher src/e2e/appcontainer_launcher.cpp)
    target_link_libraries(RimesAppContainerLauncher PRIVATE advapi32 userenv ole32)
    if(MSVC)
        target_link_libraries(RimesAppContainerLauncher PRIVATE OneCoreUAP)
    elseif(MINGW)
        target_link_libraries(RimesAppContainerLauncher PRIVATE onecoreuap_apiset)
    endif()
    rimes_harden_target(RimesAppContainerLauncher)
    rimes_use_static_runtime(RimesAppContainerLauncher)
    rimes_use_unicode_console_entrypoint(RimesAppContainerLauncher)
    # Separate COM/TSF identity for elevated recovery lifecycle testing. These
    # binaries are never packaged or installed as the daily input method.
    get_target_property(recovery_tsf_sources RimesTsf SOURCES)
    add_library(RimesRecoveryTsf SHARED ${recovery_tsf_sources})
    target_include_directories(RimesRecoveryTsf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/tsf")
    target_compile_definitions(RimesRecoveryTsf PRIVATE RIMES_RECOVERY_TEST_IDENTITY=1)
    target_link_libraries(RimesRecoveryTsf PRIVATE RimesTsfCore advapi32 gdi32 msimg32 ole32 oleaut32 user32 uuid)
    rimes_harden_target(RimesRecoveryTsf)
    rimes_use_static_runtime(RimesRecoveryTsf)

    # A fresh, static-CRT host checks the actual DLL's imports before loading it,
    # then exercises the COM destruction path which crashed with QQ's old CRT.
    add_executable(RimesTsfRuntimeTests src/tsf/runtime_compatibility_tests.cpp)
    target_link_libraries(RimesTsfRuntimeTests PRIVATE ole32 oleaut32 uuid)
    rimes_harden_target(RimesTsfRuntimeTests)
    rimes_use_static_runtime(RimesTsfRuntimeTests)
    rimes_use_unicode_console_entrypoint(RimesTsfRuntimeTests)
    add_dependencies(RimesTsfRuntimeTests RimesTsf RimesRecoveryTsf)
    add_test(NAME RimesTsfRuntime
        COMMAND RimesTsfRuntimeTests "$<TARGET_FILE:RimesTsf>")
    if(MSVC)
        add_test(NAME RimesRecoveryTsfRuntimeImports
            COMMAND RimesTsfRuntimeTests --check-static-crt
                    "$<TARGET_FILE:RimesRecoveryTsf>")
        # A deliberately /MD test-only DLL proves the import guard detects the
        # original failure condition, instead of silently accepting every DLL.
        add_library(RimesSharedCrtFixture SHARED src/tsf/runtime_shared_crt_fixture.cpp)
        rimes_harden_target(RimesSharedCrtFixture)
        add_dependencies(RimesTsfRuntimeTests RimesSharedCrtFixture)
        add_test(NAME RimesTsfRuntimeImportGuard
            COMMAND RimesTsfRuntimeTests --expect-shared-crt
                    "$<TARGET_FILE:RimesSharedCrtFixture>")
        set_tests_properties(RimesRecoveryTsfRuntimeImports
            RimesTsfRuntimeImportGuard PROPERTIES TIMEOUT 15)
    endif()
    set_tests_properties(RimesTsfRuntime PROPERTIES TIMEOUT 30)

    add_executable(RimesRecoveryRegistrar src/registrar/main.cpp)
    target_include_directories(RimesRecoveryRegistrar PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(RimesRecoveryRegistrar PRIVATE RIMES_RECOVERY_TEST_IDENTITY=1)
    target_link_libraries(RimesRecoveryRegistrar PRIVATE advapi32 ole32 oleaut32 uuid)
    rimes_harden_target(RimesRecoveryRegistrar)
    rimes_use_unicode_console_entrypoint(RimesRecoveryRegistrar)
    add_executable(RimesRecoveryCategoryFixture src/registrar/recovery_category_fixture.cpp)
    target_include_directories(RimesRecoveryCategoryFixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(RimesRecoveryCategoryFixture PRIVATE RIMES_RECOVERY_TEST_IDENTITY=1)
    target_link_libraries(RimesRecoveryCategoryFixture PRIVATE ole32 uuid)
    rimes_harden_target(RimesRecoveryCategoryFixture)
    rimes_use_unicode_console_entrypoint(RimesRecoveryCategoryFixture)
    add_executable(RimesProductProbe src/engine/product_probe.cpp src/broker/key_translation.cpp
        src/broker/broker_connection.cpp)
    target_link_libraries(RimesProductProbe PRIVATE RimesEngine RimesWorkbench)
    rimes_harden_target(RimesProductProbe)
    rimes_use_unicode_console_entrypoint(RimesProductProbe)
    rimes_use_utf8_paths(RimesProductProbe)
    add_executable(RimesSurfaceTests src/workbench/surface_tests.cpp)
    target_link_libraries(RimesSurfaceTests PRIVATE RimesWorkbench windowscodecs)
    rimes_harden_target(RimesSurfaceTests)
    rimes_use_unicode_console_entrypoint(RimesSurfaceTests)
    add_test(NAME RimesSurface COMMAND RimesSurfaceTests)
    add_executable(RimesRuntimeTests src/workbench/runtime_tests.cpp)
    add_executable(RimesMetricsTests src/workbench/metrics_tests.cpp)
    target_link_libraries(RimesMetricsTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesMetricsTests)
    add_test(NAME RimesMetrics COMMAND RimesMetricsTests)
    add_executable(RimesMetricsUiTests src/workbench/metrics_ui_tests.cpp)
    target_link_libraries(RimesMetricsUiTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesMetricsUiTests)
    rimes_use_unicode_console_entrypoint(RimesMetricsUiTests)
    add_test(NAME RimesHistoryRefresh COMMAND RimesMetricsUiTests --history-refresh)
    add_executable(RimesPeripheralTests src/peripherals/tests.cpp)
    target_link_libraries(RimesPeripheralTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesPeripheralTests)
    rimes_use_unicode_console_entrypoint(RimesPeripheralTests)
    add_test(NAME RimesPeripherals COMMAND RimesPeripheralTests)
    add_test(NAME RimesCapture COMMAND RimesPeripheralTests --capture-ui)
    set_tests_properties(RimesCapture PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    target_link_libraries(RimesRuntimeTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesRuntimeTests)
    add_test(NAME RimesRuntime COMMAND RimesRuntimeTests)
    add_executable(RimesOfficialPluginTests src/workbench/official_plugins_tests.cpp)
    target_link_libraries(RimesOfficialPluginTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesOfficialPluginTests)
    add_test(NAME RimesOfficialPlugins COMMAND RimesOfficialPluginTests)
    add_executable(RimesProviderProbe src/workbench/provider_probe.cpp)
    target_link_libraries(RimesProviderProbe PRIVATE RimesWorkbench)
    rimes_harden_target(RimesProviderProbe)
    rimes_use_unicode_console_entrypoint(RimesProviderProbe)
    add_executable(RimesSseTests src/workbench/sse_tests.cpp)
    add_executable(RimesCodexTests src/workbench/codex_cli_tests.cpp)
    add_executable(RimesLocalTranslationProbe src/workbench/local_translation_probe.cpp)
    target_link_libraries(RimesLocalTranslationProbe PRIVATE RimesWorkbench)
    rimes_harden_target(RimesLocalTranslationProbe)
    rimes_use_unicode_console_entrypoint(RimesLocalTranslationProbe)
    target_link_libraries(RimesCodexTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesCodexTests)
    add_executable(RimesCodexFixture src/workbench/codex_cli_fixture.cpp)
    target_link_libraries(RimesCodexFixture PRIVATE RimesCore)
    rimes_harden_target(RimesCodexFixture)
    rimes_use_unicode_console_entrypoint(RimesCodexFixture)
    add_test(NAME RimesCodexConnector COMMAND RimesCodexTests $<TARGET_FILE:RimesCodexFixture>)
    add_executable(RimesCodexProbe src/workbench/codex_cli_probe.cpp)
    add_executable(RimesConnectorProbe src/workbench/connector_probe.cpp)
    target_link_libraries(RimesConnectorProbe PRIVATE RimesWorkbench)
    rimes_harden_target(RimesConnectorProbe)
    rimes_use_unicode_console_entrypoint(RimesConnectorProbe)
    add_executable(RimesGatewayTests src/workbench/local_gateway_tests.cpp)
    target_link_libraries(RimesGatewayTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesGatewayTests)
    add_test(NAME RimesLocalGateway COMMAND RimesGatewayTests)
    target_link_libraries(RimesCodexProbe PRIVATE RimesWorkbench)
    rimes_harden_target(RimesCodexProbe)
    rimes_use_unicode_console_entrypoint(RimesCodexProbe)
    rimes_harden_target(RimesSseTests)
    add_test(NAME RimesSseControl COMMAND RimesSseTests)
    add_executable(RimesWorkbenchTests src/workbench/model_tests.cpp)
    target_link_libraries(RimesWorkbenchTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesWorkbenchTests)
    add_test(NAME RimesWorkbench COMMAND RimesWorkbenchTests)
    add_executable(RimesUserLexiconTests src/workbench/user_lexicon_tests.cpp)
    target_link_libraries(RimesUserLexiconTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesUserLexiconTests)
    rimes_use_unicode_console_entrypoint(RimesUserLexiconTests)
    rimes_use_utf8_paths(RimesUserLexiconTests)
    add_test(NAME RimesUserLexicon COMMAND RimesUserLexiconTests)
    set_tests_properties(RimesUserLexicon PROPERTIES TIMEOUT 30)
    add_executable(RimesBufferLayoutTests src/workbench/buffer_layout_tests.cpp)
    target_include_directories(RimesBufferLayoutTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    rimes_harden_target(RimesBufferLayoutTests)
    add_test(NAME RimesBufferLayout COMMAND RimesBufferLayoutTests)
    add_executable(RimesBufferPlacementTests src/workbench/buffer_placement_tests.cpp)
    rimes_harden_target(RimesBufferPlacementTests)
    add_test(NAME RimesBufferPlacement COMMAND RimesBufferPlacementTests)
    add_executable(RimesBrokerProtocolTests
        src/core/broker_protocol_tests.cpp
    )
    target_compile_definitions(RimesBrokerProtocolTests PRIVATE
        RIMES_BROKER_PROTOCOL_TEST_MAIN
    )
    target_link_libraries(RimesBrokerProtocolTests PRIVATE RimesCore)
    rimes_harden_target(RimesBrokerProtocolTests)

    add_executable(RimesEngineTests
        src/engine/rime_engine_tests.cpp
    )
    target_link_libraries(RimesEngineTests PRIVATE RimesEngine)
    rimes_harden_target(RimesEngineTests)

    add_executable(RimesEngineSmoke
        src/engine/rime_engine_smoke.cpp
    )
    target_link_libraries(RimesEngineSmoke PRIVATE RimesEngine)
    rimes_harden_target(RimesEngineSmoke)
    rimes_use_unicode_console_entrypoint(RimesEngineSmoke)
    rimes_use_utf8_paths(RimesEngineSmoke)

    add_executable(RimesBrokerKeyTranslationTests
        src/broker/key_translation.cpp
        src/broker/key_translation_tests.cpp
    )
    target_compile_definitions(RimesBrokerKeyTranslationTests PRIVATE
        RIMES_KEY_TRANSLATION_TEST_MAIN
    )
    target_link_libraries(RimesBrokerKeyTranslationTests PRIVATE RimesCore)
    rimes_harden_target(RimesBrokerKeyTranslationTests)

    add_executable(RimesBrokerOptionsTests
        src/broker/broker_options.cpp
        src/broker/broker_options_tests.cpp
        src/broker/default_paths.cpp
    )
    target_link_libraries(RimesBrokerOptionsTests PRIVATE RimesCore)
    rimes_harden_target(RimesBrokerOptionsTests)
    rimes_use_unicode_console_entrypoint(RimesBrokerOptionsTests)
    add_executable(RimesUiCommandTests
        src/broker/ui_command_tests.cpp
        src/broker/win32_security.cpp
        src/broker/broker_connection.cpp
        src/broker/key_translation.cpp
        src/broker/autostart.cpp
        src/broker/default_paths.cpp
    )
    target_link_libraries(RimesUiCommandTests PRIVATE RimesEngine RimesWorkbench)
    rimes_harden_target(RimesUiCommandTests)
    add_test(NAME RimesUiCommand COMMAND RimesUiCommandTests)
    set_tests_properties(RimesUiCommand PROPERTIES TIMEOUT 10)

    add_executable(RimesCandidateLayoutTests
        src/tsf/candidate_layout_tests.cpp
    )
    target_include_directories(RimesCandidateLayoutTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tsf"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    rimes_harden_target(RimesCandidateLayoutTests)

    # Own-window mouse/no-activation contracts; never touches an installed IME
    # or a user's input window and is safe in the SSH test session.
    add_executable(RimesCandidateWindowTests
        src/tsf/candidate_window_tests.cpp
        src/tsf/CandidateWindow.cpp
        src/tsf/ModuleState.cpp
    )
    target_link_libraries(RimesCandidateWindowTests PRIVATE
        user32 gdi32 msimg32
    )
    rimes_harden_target(RimesCandidateWindowTests)
    add_test(NAME RimesCandidateWindow COMMAND RimesCandidateWindowTests)

    add_executable(RimesSettingsTests src/workbench/settings_tests.cpp)
    target_link_libraries(RimesSettingsTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesSettingsTests)
    add_test(NAME RimesSettings COMMAND RimesSettingsTests)

    add_executable(RimesChordProfileTests src/workbench/chord_profile_tests.cpp src/workbench/chord_profile.cpp)
    target_link_libraries(RimesChordProfileTests PRIVATE RimesCore)
    rimes_harden_target(RimesChordProfileTests)
    add_test(NAME RimesChordProfile COMMAND RimesChordProfileTests)

    add_executable(RimesConnectionTests src/workbench/connection_tests.cpp)
    target_link_libraries(RimesConnectionTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesConnectionTests)
    add_test(NAME RimesConnections COMMAND RimesConnectionTests)

    add_executable(RimesSettingsUiTests
        src/workbench/settings_ui_tests.cpp
        src/broker/autostart.cpp
        src/broker/default_paths.cpp
    )
    target_link_libraries(RimesSettingsUiTests PRIVATE RimesWorkbench)
    rimes_harden_target(RimesSettingsUiTests)
    add_test(NAME RimesSettingsUi COMMAND RimesSettingsUiTests)
    # The suite creates many native hosts across pages, themes and DPI modes.
    # Individual interaction waits stay bounded inside the fixture; allow the
    # complete collection to finish on software-rendered developer desktops.
    set_tests_properties(RimesSettingsUi PROPERTIES TIMEOUT 120)

    add_test(NAME RimesBrokerHelp COMMAND RimesBroker --help)
    set_tests_properties(RimesBrokerHelp PROPERTIES
        TIMEOUT 10 PASS_REGULAR_EXPRESSION "--settings")

    # Fail-open contract for a disconnected BrokerClient. Does not launch a
    # Broker or touch Session 1; safe as a default CTest.
    add_executable(RimesBrokerClientContractTests
        src/tsf/broker_client_contract_tests.cpp
        src/tsf/BrokerClient.cpp
        src/tsf/ModuleState.cpp
    )
    target_include_directories(RimesBrokerClientContractTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tsf"
    )
    target_link_libraries(RimesBrokerClientContractTests PRIVATE
        RimesCore
        advapi32
        ole32
        user32
    )
    rimes_harden_target(RimesBrokerClientContractTests)
    rimes_use_unicode_console_entrypoint(RimesBrokerClientContractTests)

    # In-process TSF + live Broker typing host. Not a default CTest: it needs
    # a launched Broker, a pinned rime.dll, and the isolated e2e schema.
    add_executable(RimesTsfE2E
        src/e2e/main.cpp
        src/e2e/fake_tsf.cpp
        src/tsf/BrokerClient.cpp
        src/tsf/CandidateWindow.cpp
        src/tsf/DisplayAttribute.cpp
        src/tsf/ModuleState.cpp
        src/tsf/TextService.cpp
    )
    target_include_directories(RimesTsfE2E PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/e2e"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tsf"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(RimesTsfE2E PRIVATE
        RimesCore
        advapi32
        gdi32
        msimg32
        ole32
        oleaut32
        user32
        uuid
    )
    rimes_harden_target(RimesTsfE2E)
    target_compile_definitions(RimesTsfE2E PRIVATE RIMES_E2E_TEST_IDENTITY=1)
    add_dependencies(RimesTsfE2E RimesE2EBroker)
    rimes_use_unicode_console_entrypoint(RimesTsfE2E)

    add_executable(RimesVisualPreview WIN32
        src/visual_preview/main.cpp
        src/workbench/settings_ui.cpp src/workbench/application_update.cpp
        src/workbench/parity_settings.cpp
        src/workbench/connection_probe.cpp
        src/workbench/chord_profile.cpp
        src/workbench/provider.cpp
        src/workbench/codex_cli.cpp src/workbench/local_translation.cpp
        src/workbench/model.cpp
        src/tsf/CandidateWindow.cpp
        src/tsf/ModuleState.cpp
    )
    target_include_directories(RimesVisualPreview PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tsf"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/workbench"
    )
    target_link_libraries(RimesVisualPreview PRIVATE RimesWorkbench windowscodecs comctl32
        RimesCore
        dwmapi
        d2d1
        dwrite
        gdi32
        msimg32
        user32
        advapi32
        comctl32
        winhttp
        shell32
        ole32
        windowscodecs
    )
    rimes_harden_target(RimesVisualPreview)
    target_compile_definitions(RimesVisualPreview PRIVATE
        RIMES_BUILD_COMMIT="visual-preview"
        RIMES_VISUAL_PREVIEW=1
    )

    # Live end-to-end probe. It deliberately is not a default CTest because it
    # requires an already-running Broker configured with real librime data.
    add_executable(RimesBrokerWireSmoke
        src/broker/wire_smoke.cpp
        src/broker/win32_security.cpp
    )
    target_link_libraries(RimesBrokerWireSmoke PRIVATE
        RimesCore
        advapi32
    )
    rimes_harden_target(RimesBrokerWireSmoke)
    rimes_use_unicode_console_entrypoint(RimesBrokerWireSmoke)

    # Small native edit host used for interactive TSF verification without
    # touching a user's documents or depending on Notepad's process reuse.
    add_executable(RimesTsfTestHost WIN32
        src/test_host/main.cpp
    )
    target_link_libraries(RimesTsfTestHost PRIVATE user32)
    rimes_harden_target(RimesTsfTestHost)

    add_test(NAME RimesBrokerProtocol COMMAND RimesBrokerProtocolTests)
    add_executable(RimesBrokerLaunchTests src/tsf/broker_launch_tests.cpp)
    target_link_libraries(RimesBrokerLaunchTests PRIVATE advapi32)
    rimes_harden_target(RimesBrokerLaunchTests)
    add_test(NAME RimesBrokerLaunch COMMAND RimesBrokerLaunchTests)
    set_tests_properties(RimesBrokerLaunch PROPERTIES TIMEOUT 10)
    add_test(NAME RimesEngineUnit COMMAND RimesEngineTests)
    add_test(NAME RimesBrokerKeyTranslation
             COMMAND RimesBrokerKeyTranslationTests)
    add_test(NAME RimesBrokerOptions COMMAND RimesBrokerOptionsTests)
    add_test(NAME RimesCandidateLayout COMMAND RimesCandidateLayoutTests)
    add_test(NAME RimesBrokerClientContract
             COMMAND RimesBrokerClientContractTests)
    add_test(NAME RimesRegistrarMetadata COMMAND RimesRegistrar metadata)
    add_test(NAME RimesRegistrarDiagnostics COMMAND RimesRegistrar diagnose)
    add_test(NAME RimesBrokerEndpoint COMMAND RimesBroker --print-endpoint)
    add_test(NAME RimesBrokerPrintPaths COMMAND RimesBroker --print-paths)

    set_tests_properties(
        RimesBrokerProtocol
        RimesEngineUnit
        RimesBrokerKeyTranslation
        RimesBrokerOptions
        RimesCandidateLayout
        RimesCandidateWindow
        RimesSettings
        RimesBufferLayout
        RimesBrokerClientContract
        RimesRegistrarMetadata
        RimesRegistrarDiagnostics
        RimesBrokerEndpoint
        RimesBrokerPrintPaths
        PROPERTIES TIMEOUT 10
    )
endif()

# Source identity follows both Git checkouts and verified source archives.
get_filename_component(RIMES_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
if(EXISTS "${RIMES_REPOSITORY_ROOT}/SOURCE-COMMIT.txt")
    file(READ "${RIMES_REPOSITORY_ROOT}/SOURCE-COMMIT.txt" RIMES_BUILD_COMMIT)
    string(STRIP "${RIMES_BUILD_COMMIT}" RIMES_BUILD_COMMIT)
else()
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${RIMES_REPOSITORY_ROOT}" OUTPUT_VARIABLE RIMES_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT RIMES_BUILD_COMMIT)
    set(RIMES_BUILD_COMMIT "unknown")
endif()
target_compile_definitions(RimesWorkbench PRIVATE RIMES_BUILD_COMMIT="${RIMES_BUILD_COMMIT}")
set(RIMES_SOURCE_SNAPSHOT "")
set(RIMES_SOURCE_STATE "working-tree")
if(EXISTS "${RIMES_REPOSITORY_ROOT}/SOURCE-SNAPSHOT.txt")
    file(READ "${RIMES_REPOSITORY_ROOT}/SOURCE-SNAPSHOT.txt" RIMES_SOURCE_SNAPSHOT)
    string(STRIP "${RIMES_SOURCE_SNAPSHOT}" RIMES_SOURCE_SNAPSHOT)
    string(LENGTH "${RIMES_SOURCE_SNAPSHOT}" snapshot_length)
    if(NOT snapshot_length EQUAL 64 OR NOT RIMES_SOURCE_SNAPSHOT MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "Invalid source snapshot SHA-256")
    endif()
    set(RIMES_SOURCE_STATE "snapshot")
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/$<CONFIG>/build-identity.json" CONTENT "{\"commit\":\"${RIMES_BUILD_COMMIT}\",\"version\":\"${PROJECT_VERSION}\",\"protocol\":2,\"sourceState\":\"${RIMES_SOURCE_STATE}\",\"sourceSnapshot\":\"${RIMES_SOURCE_SNAPSHOT}\"}\n")

# Explorer and installer version checks must agree with the runtime version.
foreach(versioned_target RimesTsf RimesBroker RimesRegistrar RimesTsfTestHost RimesVisualPreview)
    if(TARGET ${versioned_target})
        if(versioned_target STREQUAL "RimesTsf")
            set(RIMES_RESOURCE_TYPE "VFT_DLL")
            set(RIMES_RESOURCE_FILENAME "${versioned_target}.dll")
        else()
            set(RIMES_RESOURCE_TYPE "VFT_APP")
            set(RIMES_RESOURCE_FILENAME "${versioned_target}.exe")
        endif()
        configure_file(src/product_version.rc.in "generated/${versioned_target}.rc" @ONLY)
        target_sources(${versioned_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/${versioned_target}.rc")
    endif()
endforeach()

# The same licensed vector glyphs are embedded in production and UI fixtures.
configure_file(src/fluent_icons.rc.in generated/fluent_icons.rc @ONLY)
foreach(icon_target RimesBroker RimesVisualPreview RimesSettingsUiTests RimesSurfaceTests RimesE2EBroker RimesMetricsUiTests RimesUiCommandTests RimesPeripheralTests)
    if(TARGET ${icon_target})
        target_sources(${icon_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/fluent_icons.rc")
    endif()
endforeach()
