cmake_minimum_required(VERSION 3.27)

if(NOT DEFINED RIMES_PRODUCT_VERSION)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../../VERSION" RIMES_PRODUCT_VERSION)
    string(STRIP "${RIMES_PRODUCT_VERSION}" RIMES_PRODUCT_VERSION)
endif()
project(RIMESLinuxIme VERSION ${RIMES_PRODUCT_VERSION} LANGUAGES C CXX)

option(RIMES_BUILD_FCITX5 "Build the Fcitx5 RIMES addon" ON)
option(RIMES_BUILD_TESTS "Build unit tests and headless E2E hosts" ON)
option(RIMES_BUILD_QT_HOST "Build the Qt E2E host when Qt5 is available" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

include(GNUInstallDirs)
include(CTest)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../../LICENSES/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/rimes")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../../THIRD_PARTY_NOTICES.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/rimes")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../../OfficialPlugins/LICENSES/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/rimes/plugins")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../../OfficialPlugins/LICENSE"
              "${CMAKE_CURRENT_SOURCE_DIR}/../../../OfficialPlugins/NOTICE"
              "${CMAKE_CURRENT_SOURCE_DIR}/../../../OfficialPlugins/THIRD_PARTY_NOTICES.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/rimes/plugins")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
pkg_check_modules(Rime REQUIRED IMPORTED_TARGET rime>=1.7.0)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../Shared/SchemeImport" "scheme-import")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../runtime" "runtime")

set(RIMES_BUFFER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../buffer")
set(RIMES_CAPSULE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../capsule")

function(rimes_harden target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wno-missing-field-initializers
    )
endfunction()

add_library(RimesBufferCore STATIC
    "${RIMES_BUFFER_ROOT}/core/buffer_ids.cpp"
    "${RIMES_BUFFER_ROOT}/core/buffer_model.cpp"
    "${RIMES_BUFFER_ROOT}/core/buffer_placement.cpp"
    "${RIMES_BUFFER_ROOT}/core/buffer_process.cpp"
    "${RIMES_BUFFER_ROOT}/core/buffer_protocol.cpp"
    "${RIMES_BUFFER_ROOT}/core/buffer_return.cpp"
    "${RIMES_BUFFER_ROOT}/core/derived_workspace.cpp"
)
target_include_directories(RimesBufferCore PUBLIC
    "${RIMES_BUFFER_ROOT}/core"
)
set_target_properties(RimesBufferCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(RimesBufferCore PUBLIC RimesPluginCore RimesTranslationUnits)
rimes_harden(RimesBufferCore)

pkg_check_modules(CapsuleICU REQUIRED IMPORTED_TARGET icu-i18n icu-uc)
find_package(SQLite3 REQUIRED)
add_library(RimesCapsuleCore STATIC
    "${RIMES_CAPSULE_ROOT}/core/capsule_hash.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_kinds.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_modules.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_text.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_structure.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_rich_text.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_store.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_reference.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_resources.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_capture.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_password.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_secret_security.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capture_process.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_model.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_protocol.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_transport.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_clipboard.cpp"
    "${RIMES_CAPSULE_ROOT}/core/capsule_history.cpp"
)
target_include_directories(RimesCapsuleCore PUBLIC
    "${RIMES_CAPSULE_ROOT}/core"
    "${RIMES_BUFFER_ROOT}/core"
)
target_link_libraries(RimesCapsuleCore PUBLIC RimesBufferCore RimesProcess PkgConfig::CapsuleICU SQLite::SQLite3)
target_compile_definitions(RimesCapsuleCore PRIVATE RIMES_CAPSULE_PREVIEW_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rimes/rimes-capsule-preview")
target_compile_definitions(RimesCapsuleCore PRIVATE RIMES_CAPSULE_CAPTURE_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rimes/rimes-capsule-capture")
set_target_properties(RimesCapsuleCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
rimes_harden(RimesCapsuleCore)

set(RIMES_MAILBOX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../mailbox")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_MAILBOX_ROOT}/resources/native-gateway.json"
    "${RIMES_MAILBOX_ROOT}/resources/native-gateway-settings.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../Sources/RimeBuffer/SettingsWindow.swift"
    "${RIMES_MAILBOX_ROOT}/verify_gateway_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../Sources/RimeBuffer/Inbound/LocalGateway.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../Sources/RimeBuffer/Inbound/InboundBus.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../Sources/RimeBuffer/Inbound/GatewayToken.swift")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${RIMES_MAILBOX_ROOT}/verify_gateway_contract.py"
    --output "${CMAKE_BINARY_DIR}/generated/native_gateway_contract.hpp" RESULT_VARIABLE RIMES_GATEWAY_CONTRACT)
if(NOT RIMES_GATEWAY_CONTRACT EQUAL 0)
    message(FATAL_ERROR "Regenerate and review the native gateway contract")
endif()
add_custom_target(RimesGatewayContract COMMAND "${Python3_EXECUTABLE}" "${RIMES_MAILBOX_ROOT}/verify_gateway_contract.py")
add_library(RimesMailboxCore STATIC "${RIMES_MAILBOX_ROOT}/core/terminal_files.cpp" "${RIMES_MAILBOX_ROOT}/core/mailbox_store.cpp" "${RIMES_MAILBOX_ROOT}/core/inbox_actions.cpp" "${RIMES_MAILBOX_ROOT}/core/mailbox_buffer_client.cpp" "${RIMES_MAILBOX_ROOT}/core/gateway_protocol.cpp" "${RIMES_MAILBOX_ROOT}/core/gateway_server.cpp" "${RIMES_MAILBOX_ROOT}/core/codex_image_jobs.cpp")
target_include_directories(RimesMailboxCore PUBLIC "${RIMES_MAILBOX_ROOT}/core" PRIVATE "${CMAKE_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_MAILBOX_ROOT}/resources/native-terminal.json"
    "${RIMES_MAILBOX_ROOT}/verify_terminal_contract.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${RIMES_MAILBOX_ROOT}/verify_terminal_contract.py"
    --output "${CMAKE_BINARY_DIR}/generated/native_terminal_contract.hpp" RESULT_VARIABLE RIMES_TERMINAL_IMPORT_RESULT)
if(NOT RIMES_TERMINAL_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Re-export and review native Mailbox terminal contract")
endif()
add_custom_target(RimesMailboxTerminalContract COMMAND "${Python3_EXECUTABLE}" "${RIMES_MAILBOX_ROOT}/verify_terminal_contract.py")
add_dependencies(RimesMailboxCore RimesGatewayContract RimesMailboxTerminalContract)
target_link_libraries(RimesMailboxCore PUBLIC RimesCapsuleCore RimesCodex)
set_target_properties(RimesMailboxCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(RimesMailboxCore PUBLIC RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
target_compile_definitions(RimesMailboxCore PRIVATE RIMES_MAILBOX_FILE_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rimes/rimes-mailbox-file")
rimes_harden(RimesMailboxCore)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_MAILBOX_ROOT}/resources/native-chat.json"
    "${RIMES_MAILBOX_ROOT}/verify_chat_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../Sources/RimeBuffer/AITextGenerationOptions.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../Sources/RimeBuffer/Mailbox/AITextMailboxGenerationCoordinator.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../Sources/RimeBuffer/Mailbox/MailboxChatLinuxFixtures.swift")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${RIMES_MAILBOX_ROOT}/verify_chat_contract.py"
    --output "${CMAKE_BINARY_DIR}/generated/native_chat_contract.hpp" RESULT_VARIABLE RIMES_CHAT_IMPORT_RESULT)
if(NOT RIMES_CHAT_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Re-export and review native Mailbox conversation prompts")
endif()
add_custom_target(RimesMailboxChatContract COMMAND "${Python3_EXECUTABLE}" "${RIMES_MAILBOX_ROOT}/verify_chat_contract.py")
add_library(RimesMailboxChat STATIC "${RIMES_MAILBOX_ROOT}/core/chat_jobs.cpp")
target_link_libraries(RimesMailboxChat PUBLIC RimesMailboxCore RimesClaude)
target_include_directories(RimesMailboxChat PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(RimesMailboxChat RimesMailboxChatContract)
rimes_harden(RimesMailboxChat)

add_executable(rimes-gateway "${RIMES_MAILBOX_ROOT}/ctl/gateway_ctl.cpp")
target_link_libraries(rimes-gateway PRIVATE RimesMailboxCore)
rimes_harden(rimes-gateway)
install(TARGETS rimes-gateway DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
    add_executable(rimes-mailbox-chat-tests "${RIMES_MAILBOX_ROOT}/tests/chat_jobs_tests.cpp")
    target_link_libraries(rimes-mailbox-chat-tests PRIVATE RimesMailboxChat)
    rimes_harden(rimes-mailbox-chat-tests)
    add_test(NAME mailbox-chat COMMAND "${Python3_EXECUTABLE}" "${RIMES_MAILBOX_ROOT}/tests/chat_http_fixture.py" $<TARGET_FILE:rimes-mailbox-chat-tests> "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/codex_fixture.py" "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/claude_fixture.py")
    add_test(NAME mailbox-chat-native-contract COMMAND "${Python3_EXECUTABLE}" "${RIMES_MAILBOX_ROOT}/verify_chat_contract.py")
    add_executable(rimes-mailbox-store-tests "${RIMES_MAILBOX_ROOT}/tests/mailbox_store_tests.cpp")
    target_link_libraries(rimes-mailbox-store-tests PRIVATE RimesMailboxCore)
    target_compile_definitions(rimes-mailbox-store-tests PRIVATE RIMES_MAILBOX_FIXTURE="${RIMES_MAILBOX_ROOT}/tests/fixtures/native-mailbox.json")
    rimes_harden(rimes-mailbox-store-tests)
    add_test(NAME mailbox-store COMMAND rimes-mailbox-store-tests)
    add_executable(rimes-codex-image-tests "${RIMES_MAILBOX_ROOT}/tests/codex_image_jobs_tests.cpp")
    target_link_libraries(rimes-codex-image-tests PRIVATE RimesMailboxCore)
    rimes_harden(rimes-codex-image-tests)
    add_test(NAME codex-mailbox-images COMMAND rimes-codex-image-tests "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/codex_fixture.py")
    add_executable(rimes-gateway-tests "${RIMES_MAILBOX_ROOT}/tests/gateway_tests.cpp")
    target_link_libraries(rimes-gateway-tests PRIVATE RimesMailboxCore)
    rimes_harden(rimes-gateway-tests)
    add_test(NAME mailbox-gateway COMMAND rimes-gateway-tests)
    set_tests_properties(mailbox-gateway PROPERTIES TIMEOUT 35)
    add_test(NAME mailbox-gateway-native-contract COMMAND "${Python3_EXECUTABLE}" "${RIMES_MAILBOX_ROOT}/verify_gateway_contract.py")
endif()

add_library(RimesEngine STATIC
    src/engine/rime_engine.cpp
    src/engine/rime_key.cpp
    src/engine/rime_paths.cpp
    src/engine/rime_snapshot.cpp
    src/engine/rime_user_lock.cpp
    src/engine/deployment_process.cpp
)
target_include_directories(RimesEngine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(RimesEngine PUBLIC PkgConfig::Rime Threads::Threads RimesSchemeImport)
target_compile_definitions(RimesEngine PRIVATE RIMES_DISTRIBUTION_VERSION="${PROJECT_VERSION}")
target_compile_definitions(RimesEngine PRIVATE
    RIMES_INSTALLED_DEPLOY_HELPER="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rimes/rimes-deploy"
    RIMES_BUILD_DEPLOY_HELPER="${CMAKE_BINARY_DIR}/rimes-deploy")
set_target_properties(RimesEngine PROPERTIES POSITION_INDEPENDENT_CODE ON)
rimes_harden(RimesEngine)

add_executable(rimes-deploy src/engine/deploy_main.cpp)
target_link_libraries(rimes-deploy PRIVATE PkgConfig::Rime)
target_compile_definitions(rimes-deploy PRIVATE RIMES_DISTRIBUTION_VERSION="${PROJECT_VERSION}")
rimes_harden(rimes-deploy)
install(TARGETS rimes-deploy DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/rimes")
add_dependencies(RimesEngine rimes-deploy)

add_library(RimesChordEngine STATIC src/chord/chord_session.cpp)
target_link_libraries(RimesChordEngine PUBLIC RimesEngine RimesChordCore)
set_target_properties(RimesChordEngine PROPERTIES POSITION_INDEPENDENT_CODE ON)
rimes_harden(RimesChordEngine)

if(RIMES_BUILD_FCITX5)
    find_package(Gettext REQUIRED)
    find_package(Fcitx5Core REQUIRED)
    if(Fcitx5Core_VERSION VERSION_GREATER_EQUAL "5.1.9")
        add_compile_definitions(RIMES_FCITX_HAS_CANDIDATE_COMMENT=1)
    endif()
    find_package(Fcitx5Module REQUIRED COMPONENTS TestFrontend)
    include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

    add_library(rimes MODULE
        src/fcitx/candidate_matrix_window.cpp
        src/fcitx/rimes_candidate.cpp
        src/fcitx/rimes_ime.cpp
        src/fcitx/rimes_state.cpp
        src/chord/chord_service.cpp
        src/statistics/metrics_service.cpp
        src/statistics/metrics_keys.cpp
        src/typing-practice/practice_service.cpp
        src/session/session_monitor.cpp
        src/desktop/workspace_launcher.cpp
        src/desktop/desktop_shortcuts.cpp
        src/buffer/buffer_service.cpp src/buffer/buffer_mailbox.cpp
        src/buffer/buffer_translation.cpp
        src/buffer/buffer_delivery.cpp
        src/capsule/capsule_service.cpp
        src/capsule/capsule_history_service.cpp
        src/capsule/capsule_capture_service.cpp
    )
    target_link_libraries(rimes PRIVATE RimesPracticeCore RimesStatisticsCore RimesChordEngine yaml-cpp::yaml-cpp Fcitx5::Core RimesEngine RimesBufferCore RimesMailboxCore RimesCapsuleCore RimesPluginCore RimesLocalTranslation RimesAI RimesClaude)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
    pkg_check_modules(RimesXCB REQUIRED IMPORTED_TARGET xcb)
    pkg_check_modules(GTK_LAYER_SHELL IMPORTED_TARGET gtk-layer-shell-0)
    target_link_libraries(rimes PRIVATE PkgConfig::GTK3)
    target_link_libraries(rimes PRIVATE PkgConfig::RimesXCB)
    if(GTK_LAYER_SHELL_FOUND)
        target_link_libraries(rimes PRIVATE PkgConfig::GTK_LAYER_SHELL)
        target_compile_definitions(rimes PRIVATE HAVE_GTK_LAYER_SHELL=1)
    endif()
    target_compile_definitions(rimes PRIVATE
        RIMES_INSTALL_SHARED_DIR="${CMAKE_INSTALL_FULL_DATADIR}/rimes/data"
        RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}"
        RIMES_INSTALLED_SETTINGS="${CMAKE_INSTALL_FULL_BINDIR}/rimes-settings"
        RIMES_BUILD_SETTINGS="${CMAKE_BINARY_DIR}/runtime/rimes-settings"
        RIMES_INSTALLED_MAILBOX="${CMAKE_INSTALL_FULL_BINDIR}/rimes-mailbox"
        RIMES_BUILD_MAILBOX="${CMAKE_BINARY_DIR}/rimes-mailbox"
    )
    set_target_properties(rimes PROPERTIES
        PREFIX ""
        OUTPUT_NAME "rimes"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    rimes_harden(rimes)

    configure_file(
        data/rimes-addon.conf.in
        "${CMAKE_BINARY_DIR}/rimes-addon.conf"
        @ONLY
    )
    configure_file(
        data/rimes.conf.in
        "${CMAKE_BINARY_DIR}/rimes.conf"
        COPYONLY
    )
    configure_file(
        data/org.fcitx.Fcitx5.Addon.Rimes.metainfo.xml.in
        "${CMAKE_BINARY_DIR}/org.fcitx.Fcitx5.Addon.Rimes.metainfo.xml"
        @ONLY
    )

    install(TARGETS rimes DESTINATION "${FCITX_INSTALL_ADDONDIR}")
    install(FILES "${CMAKE_BINARY_DIR}/rimes-addon.conf"
            RENAME rimes.conf
            DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon")
    install(FILES "${CMAKE_BINARY_DIR}/rimes.conf"
            DESTINATION "${FCITX_INSTALL_PKGDATADIR}/inputmethod")
    install(FILES "${CMAKE_BINARY_DIR}/org.fcitx.Fcitx5.Addon.Rimes.metainfo.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
    # Matches Icon=rimes in the Fcitx input-method descriptor and GTK windows.
    install(DIRECTORY data/icons/hicolor
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons")

    # The Fcitx configuration entry launches the GTK scheme manager. Do not
    # silently build an addon whose advertised import entry is absent.
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    pkg_check_modules(MailboxVTE REQUIRED IMPORTED_TARGET vte-2.91>=0.76)
    if(GTK3_FOUND)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_custom_target(RimesMailboxContract ALL
            COMMAND "${Python3_EXECUTABLE}" "${RIMES_MAILBOX_ROOT}/verify_native_contract.py"
            VERBATIM)
        file(READ "${RIMES_MAILBOX_ROOT}/resources/native-mailbox.json" RIMES_MAILBOX_TOKENS)
        configure_file("${RIMES_MAILBOX_ROOT}/ui/mailbox_tokens.hpp.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/mailbox_tokens.hpp" @ONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RIMES_MAILBOX_ROOT}/resources/native-mailbox.json")
        add_library(RimesMailboxUI STATIC "${RIMES_MAILBOX_ROOT}/ui/terminal_session.cpp" "${RIMES_MAILBOX_ROOT}/ui/terminal_workspace.cpp" "${RIMES_MAILBOX_ROOT}/ui/mailbox_workspace.cpp" "${RIMES_MAILBOX_ROOT}/ui/mailbox_toast.cpp")
        target_include_directories(RimesMailboxUI PUBLIC "${RIMES_MAILBOX_ROOT}/ui" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_compile_definitions(RimesMailboxUI PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
        target_link_libraries(RimesMailboxUI PUBLIC RimesMailboxChat RimesDesktopUI PkgConfig::MailboxVTE)
        rimes_harden(RimesMailboxUI)
        add_dependencies(RimesMailboxUI RimesMailboxContract)
        add_executable(rimes-mailbox-file "${RIMES_MAILBOX_ROOT}/ui/terminal_file_main.cpp")
        target_link_libraries(rimes-mailbox-file PRIVATE RimesMailboxCore PkgConfig::MailboxVTE)
        rimes_harden(rimes-mailbox-file)
        install(TARGETS rimes-mailbox-file DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/rimes")
        add_dependencies(RimesMailboxUI rimes-mailbox-file)
        add_executable(rimes-mailbox "${RIMES_MAILBOX_ROOT}/ui/main.cpp" "${RIMES_MAILBOX_ROOT}/ui/terminal_fixtures.cpp" "${RIMES_MAILBOX_ROOT}/ui/mailbox_session.cpp" src/session/session_monitor.cpp)
        target_include_directories(rimes-mailbox PRIVATE src)
        target_compile_definitions(rimes-mailbox PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
        target_link_libraries(rimes-mailbox PRIVATE RimesMailboxUI Fcitx5::Utils Threads::Threads)
        rimes_harden(rimes-mailbox)
        add_dependencies(rimes rimes-mailbox)
        install(TARGETS rimes-mailbox DESTINATION "${CMAKE_INSTALL_BINDIR}")
        install(FILES "${RIMES_MAILBOX_ROOT}/ui/org.scholay.rimes.mailbox.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
        add_executable(rimes-schemes src/schemes/scheme_manager.cpp)
        target_include_directories(rimes-schemes PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(rimes-schemes PRIVATE RimesEngine RimesSchemeImport ${GTK3_LIBRARIES})
        target_compile_options(rimes-schemes PRIVATE ${GTK3_CFLAGS_OTHER})
        rimes_harden(rimes-schemes)
        install(TARGETS rimes-schemes DESTINATION "${CMAKE_INSTALL_BINDIR}")
        install(FILES data/org.scholay.rimes.schemes.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
        pkg_check_modules(GTK_LAYER_SHELL gtk-layer-shell-0)
        add_executable(rimes-buffer
            "${RIMES_BUFFER_ROOT}/ui/rimes_buffer_app.cpp"
        )
        target_include_directories(rimes-buffer PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(rimes-buffer PRIVATE RimesBufferCore RimesDesktopUI ${GTK3_LIBRARIES})
        target_compile_options(rimes-buffer PRIVATE ${GTK3_CFLAGS_OTHER})
        if(GTK_LAYER_SHELL_FOUND)
            target_include_directories(rimes-buffer PRIVATE ${GTK_LAYER_SHELL_INCLUDE_DIRS})
            target_link_libraries(rimes-buffer PRIVATE ${GTK_LAYER_SHELL_LIBRARIES})
            target_compile_options(rimes-buffer PRIVATE ${GTK_LAYER_SHELL_CFLAGS_OTHER})
            target_compile_definitions(rimes-buffer PRIVATE HAVE_GTK_LAYER_SHELL=1)
        endif()
        rimes_harden(rimes-buffer)
        set_target_properties(rimes-buffer PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
        install(TARGETS rimes-buffer DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/rimes")
        install(TARGETS rimes-buffer DESTINATION "${CMAKE_INSTALL_BINDIR}")
        find_program(RIMES_BUFFER_XVFB_RUN xvfb-run)
        if(RIMES_BUFFER_XVFB_RUN AND RIMES_BUILD_TESTS)
            add_test(NAME buffer-render COMMAND "${RIMES_BUFFER_XVFB_RUN}" -a $<TARGET_FILE:rimes-buffer>
                --render "${CMAKE_BINARY_DIR}/buffer-render")
        endif()


        pkg_check_modules(CapsuleWayland REQUIRED IMPORTED_TARGET wayland-client)
        pkg_check_modules(CapsuleXfixes REQUIRED IMPORTED_TARGET xcb xcb-xfixes)
        find_program(CAPSULE_WAYLAND_SCANNER wayland-scanner REQUIRED)
        set(CAPSULE_PROTOCOL_SOURCES)
        foreach(protocol ext-data-control-v1 wlr-data-control-unstable-v1)
            set(xml "${RIMES_CAPSULE_ROOT}/protocols/${protocol}.xml")
            set(header "${CMAKE_CURRENT_BINARY_DIR}/generated/${protocol}-client-protocol.h")
            set(source "${CMAKE_CURRENT_BINARY_DIR}/generated/${protocol}-protocol.c")
            add_custom_command(OUTPUT "${header}" "${source}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
                COMMAND "${CAPSULE_WAYLAND_SCANNER}" client-header "${xml}" "${header}"
                COMMAND "${CAPSULE_WAYLAND_SCANNER}" private-code "${xml}" "${source}"
                DEPENDS "${xml}" VERBATIM)
            list(APPEND CAPSULE_PROTOCOL_SOURCES "${header}" "${source}")
        endforeach()
        add_library(RimesClipboardDesktop STATIC ${CAPSULE_PROTOCOL_SOURCES}
            "${RIMES_CAPSULE_ROOT}/ui/clipboard_desktop.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/clipboard_x11.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/clipboard_wayland.cpp")
        target_include_directories(RimesClipboardDesktop PUBLIC "${RIMES_CAPSULE_ROOT}/ui"
            PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(RimesClipboardDesktop PUBLIC RimesCapsuleCore PkgConfig::CapsuleWayland PkgConfig::CapsuleXfixes)
        if(TARGET RimesMailboxUI)
            target_link_libraries(RimesMailboxUI PUBLIC RimesClipboardDesktop)
        endif()
        rimes_harden(RimesClipboardDesktop)
        target_link_libraries(rimes-settings PRIVATE RimesClipboardDesktop)
        target_compile_definitions(rimes-settings PRIVATE RIMES_GATEWAY_CLIPBOARD=1)

        pkg_check_modules(CapsuleCapture REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 libportal gio-unix-2.0 x11)
        add_executable(rimes-capsule-capture "${RIMES_CAPSULE_ROOT}/ui/capsule_capture_main.cpp")
        target_include_directories(rimes-capsule-capture PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(rimes-capsule-capture PRIVATE RimesCapsuleCore PkgConfig::CapsuleCapture ${GTK3_LIBRARIES})
        rimes_harden(rimes-capsule-capture)
        install(TARGETS rimes-capsule-capture DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/rimes")
        pkg_check_modules(CapsulePoppler REQUIRED IMPORTED_TARGET poppler-glib)
        add_executable(rimes-capsule-preview "${RIMES_CAPSULE_ROOT}/ui/capsule_preview_main.cpp")
        target_include_directories(rimes-capsule-preview PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(rimes-capsule-preview PRIVATE RimesCapsuleCore PkgConfig::CapsulePoppler PkgConfig::CapsuleCapture ${GTK3_LIBRARIES})
        rimes_harden(rimes-capsule-preview)
        install(TARGETS rimes-capsule-preview DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/rimes")
        add_executable(rimes-capsule
            "${RIMES_CAPSULE_ROOT}/ui/capture_controller.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/clipboard_controller.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_manager.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_password_view.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_note_editor.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_resource_editor.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/rimes_capsule_app.cpp"
        )
        target_include_directories(rimes-capsule PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(rimes-capsule PRIVATE RimesClipboardDesktop RimesCapsuleCore RimesDesktopUI ${GTK3_LIBRARIES})
        target_compile_definitions(rimes-capsule PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
        target_compile_options(rimes-capsule PRIVATE ${GTK3_CFLAGS_OTHER})
        if(GTK_LAYER_SHELL_FOUND)
            target_include_directories(rimes-capsule PRIVATE ${GTK_LAYER_SHELL_INCLUDE_DIRS})
            target_link_libraries(rimes-capsule PRIVATE ${GTK_LAYER_SHELL_LIBRARIES})
            target_compile_options(rimes-capsule PRIVATE ${GTK_LAYER_SHELL_CFLAGS_OTHER})
            target_compile_definitions(rimes-capsule PRIVATE HAVE_GTK_LAYER_SHELL=1)
        endif()
        rimes_harden(rimes-capsule)
        set_target_properties(rimes-capsule PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
        install(TARGETS rimes-capsule DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/rimes")
        install(TARGETS rimes-capsule DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()

    add_executable(rimes-buffer-ctl
        "${RIMES_BUFFER_ROOT}/ctl/rimes_buffer_ctl.cpp"
    )
    target_link_libraries(rimes-buffer-ctl PRIVATE RimesBufferCore)
    rimes_harden(rimes-buffer-ctl)
    set_target_properties(rimes-buffer-ctl PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    install(TARGETS rimes-buffer-ctl DESTINATION "${CMAKE_INSTALL_BINDIR}")

    add_executable(rimes-capsule-ctl
        "${RIMES_CAPSULE_ROOT}/ctl/rimes_capsule_ctl.cpp"
    )
    target_link_libraries(rimes-capsule-ctl PRIVATE RimesCapsuleCore)
    rimes_harden(rimes-capsule-ctl)
    set_target_properties(rimes-capsule-ctl PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    install(TARGETS rimes-capsule-ctl DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

if(RIMES_BUILD_TESTS)
    if(TARGET PkgConfig::GTK3)
        add_executable(rimes-gtk-window-retirement-tests tests/gtk_window_retirement_tests.cpp)
        target_include_directories(rimes-gtk-window-retirement-tests PRIVATE src)
        target_link_libraries(rimes-gtk-window-retirement-tests PRIVATE PkgConfig::GTK3)
        rimes_harden(rimes-gtk-window-retirement-tests)
        find_program(RIMES_XVFB_RUN xvfb-run)
        if(RIMES_XVFB_RUN)
            add_test(NAME RimesGtkWindowRetirement COMMAND ${RIMES_XVFB_RUN} -a $<TARGET_FILE:rimes-gtk-window-retirement-tests>)
        endif()
    endif()
    add_executable(rimes-candidate-matrix-tests tests/candidate_matrix_tests.cpp)
    target_include_directories(rimes-candidate-matrix-tests PRIVATE src)
    rimes_harden(rimes-candidate-matrix-tests)
    add_test(NAME RimesCandidateMatrix COMMAND rimes-candidate-matrix-tests)
    add_executable(rimes-key-tests tests/rime_key_tests.cpp)
    target_link_libraries(rimes-key-tests PRIVATE RimesEngine)
    rimes_harden(rimes-key-tests)

    add_executable(rimes-snapshot-tests tests/rime_snapshot_tests.cpp)
    target_link_libraries(rimes-snapshot-tests PRIVATE RimesEngine)
    rimes_harden(rimes-snapshot-tests)

    add_executable(rimes-paths-tests tests/rime_paths_tests.cpp)
    target_link_libraries(rimes-paths-tests PRIVATE RimesEngine)
    rimes_harden(rimes-paths-tests)

    add_executable(rimes-user-lock-tests tests/rime_user_lock_tests.cpp)
    target_link_libraries(rimes-user-lock-tests PRIVATE RimesEngine)
    rimes_harden(rimes-user-lock-tests)
    add_test(NAME RimesUserLock COMMAND rimes-user-lock-tests)

    add_executable(rimes-deployment-shutdown-tests tests/deployment_shutdown_tests.cpp)
    target_link_libraries(rimes-deployment-shutdown-tests PRIVATE RimesEngine)
    rimes_harden(rimes-deployment-shutdown-tests)
    add_test(NAME RimesDeploymentShutdown COMMAND rimes-deployment-shutdown-tests)
    set_tests_properties(RimesDeploymentShutdown PROPERTIES TIMEOUT 15)

    add_executable(rimes-chord-engine-tests tests/chord_engine_test.cpp)
    target_link_libraries(rimes-chord-engine-tests PRIVATE RimesChordEngine)
    rimes_harden(rimes-chord-engine-tests)

    add_executable(rimes-engine-smoke tests/rime_engine_smoke.cpp)
    target_link_libraries(rimes-engine-smoke PRIVATE RimesEngine)
    rimes_harden(rimes-engine-smoke)

    add_executable(rimes-buffer-model-tests
        "${RIMES_BUFFER_ROOT}/tests/buffer_model_tests.cpp"
    )
    target_link_libraries(rimes-buffer-model-tests PRIVATE RimesBufferCore)
    rimes_harden(rimes-buffer-model-tests)

    add_executable(rimes-derived-workspace-tests "${RIMES_BUFFER_ROOT}/tests/derived_workspace_tests.cpp")
    target_link_libraries(rimes-derived-workspace-tests PRIVATE RimesBufferCore)
    rimes_harden(rimes-derived-workspace-tests)
    add_test(NAME derived-workspace COMMAND rimes-derived-workspace-tests)

    add_executable(rimes-buffer-auto-send-tests "${RIMES_BUFFER_ROOT}/tests/buffer_auto_send_tests.cpp")
    target_link_libraries(rimes-buffer-auto-send-tests PRIVATE RimesBufferCore)
    rimes_harden(rimes-buffer-auto-send-tests)
    add_test(NAME buffer-auto-send COMMAND rimes-buffer-auto-send-tests)

    add_executable(rimes-buffer-return-tests
        "${RIMES_BUFFER_ROOT}/tests/buffer_return_tests.cpp"
    )
    target_link_libraries(rimes-buffer-return-tests PRIVATE RimesBufferCore)
    rimes_harden(rimes-buffer-return-tests)

    add_executable(rimes-buffer-protocol-tests
        "${RIMES_BUFFER_ROOT}/tests/buffer_protocol_tests.cpp"
    )
    target_link_libraries(rimes-buffer-protocol-tests PRIVATE RimesBufferCore)
    rimes_harden(rimes-buffer-protocol-tests)

    add_executable(rimes-buffer-process-tests
        "${RIMES_BUFFER_ROOT}/tests/buffer_process_tests.cpp"
    )
    target_link_libraries(rimes-buffer-process-tests PRIVATE RimesBufferCore)
    rimes_harden(rimes-buffer-process-tests)

    add_executable(rimes-buffer-placement-tests
        "${RIMES_BUFFER_ROOT}/tests/buffer_placement_tests.cpp"
    )
    target_link_libraries(rimes-buffer-placement-tests PRIVATE RimesBufferCore)
    rimes_harden(rimes-buffer-placement-tests)

    add_test(NAME RimesKey COMMAND rimes-key-tests)
    add_test(NAME RimesSnapshot COMMAND rimes-snapshot-tests)
    add_test(NAME RimesPaths COMMAND rimes-paths-tests)
    add_test(NAME RimesBufferModel COMMAND rimes-buffer-model-tests)
    add_test(NAME RimesBufferReturn COMMAND rimes-buffer-return-tests)
    add_test(NAME RimesBufferProtocol COMMAND rimes-buffer-protocol-tests)
    add_test(NAME RimesBufferProcess COMMAND rimes-buffer-process-tests)
    add_test(NAME RimesBufferPlacement COMMAND rimes-buffer-placement-tests)

    if(TARGET RimesClipboardDesktop)
        add_executable(rimes-clipboard-sensitive-tests "${RIMES_CAPSULE_ROOT}/tests/clipboard_sensitive_tests.cpp")
        target_include_directories(rimes-clipboard-sensitive-tests PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(rimes-clipboard-sensitive-tests PRIVATE RimesClipboardDesktop ${GTK3_LIBRARIES})
        rimes_harden(rimes-clipboard-sensitive-tests)
        add_test(NAME capsule-clipboard-sensitive-x11 COMMAND "${RIMES_BUFFER_XVFB_RUN}" -a $<TARGET_FILE:rimes-clipboard-sensitive-tests>)
        add_executable(rimes-clipboard-desktop-tests "${RIMES_CAPSULE_ROOT}/tests/clipboard_desktop_tests.cpp")
        target_include_directories(rimes-clipboard-desktop-tests PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(rimes-clipboard-desktop-tests PRIVATE RimesClipboardDesktop ${GTK3_LIBRARIES})
        rimes_harden(rimes-clipboard-desktop-tests)
        add_test(NAME capsule-clipboard-x11 COMMAND "${RIMES_BUFFER_XVFB_RUN}" -a $<TARGET_FILE:rimes-clipboard-desktop-tests>)
    endif()

    if(TARGET RimesClipboardDesktop)
        add_executable(rimes-clipboard-controller-tests "${RIMES_CAPSULE_ROOT}/tests/clipboard_controller_tests.cpp" "${RIMES_CAPSULE_ROOT}/ui/clipboard_controller.cpp")
        target_link_libraries(rimes-clipboard-controller-tests PRIVATE RimesClipboardDesktop RimesDesktopUI)
        target_compile_definitions(rimes-clipboard-controller-tests PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
        rimes_harden(rimes-clipboard-controller-tests)
        add_test(NAME capsule-clipboard-controller COMMAND "${RIMES_BUFFER_XVFB_RUN}" -a $<TARGET_FILE:rimes-clipboard-controller-tests>)
    endif()

    add_executable(rimes-capsule-history-tests "${RIMES_CAPSULE_ROOT}/tests/capsule_history_tests.cpp")
    target_link_libraries(rimes-capsule-history-tests PRIVATE RimesCapsuleCore)
    target_compile_definitions(rimes-capsule-history-tests PRIVATE NATIVE_CLIPBOARD_FIXTURES="${RIMES_CAPSULE_ROOT}/tests/fixtures/native-clipboard.json")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME capsule-native-clipboard-fixtures COMMAND "${Python3_EXECUTABLE}" "${RIMES_CAPSULE_ROOT}/tests/verify_native_clipboard.py")
    rimes_harden(rimes-capsule-history-tests)
    add_test(NAME RimesCapsuleHistory COMMAND rimes-capsule-history-tests)

    add_executable(rimes-capsule-notes-tests "${RIMES_CAPSULE_ROOT}/tests/capsule_notes_tests.cpp")
    target_link_libraries(rimes-capsule-notes-tests PRIVATE RimesCapsuleCore)
    target_compile_definitions(rimes-capsule-notes-tests PRIVATE NATIVE_NOTES_FIXTURES="${RIMES_CAPSULE_ROOT}/tests/fixtures/native-notes.json")
    rimes_harden(rimes-capsule-notes-tests)
    add_test(NAME RimesCapsuleNotes COMMAND rimes-capsule-notes-tests)
    add_test(NAME capsule-native-fixtures COMMAND ${Python3_EXECUTABLE} "${RIMES_CAPSULE_ROOT}/tests/verify_native_notes.py")
    add_executable(rimes-capsule-transport-tests "${RIMES_CAPSULE_ROOT}/tests/capsule_transport_tests.cpp")
    target_link_libraries(rimes-capsule-transport-tests PRIVATE RimesCapsuleCore)
    rimes_harden(rimes-capsule-transport-tests)
    add_test(NAME RimesCapsuleTransport COMMAND rimes-capsule-transport-tests)

    add_executable(rimes-capsule-store-tests
        "${RIMES_CAPSULE_ROOT}/tests/capsule_store_tests.cpp"
    )
    target_link_libraries(rimes-capsule-store-tests PRIVATE RimesCapsuleCore)
    rimes_harden(rimes-capsule-store-tests)

    add_executable(rimes-capsule-model-tests
        "${RIMES_CAPSULE_ROOT}/tests/capsule_model_tests.cpp"
    )
    target_link_libraries(rimes-capsule-model-tests PRIVATE RimesCapsuleCore)
    rimes_harden(rimes-capsule-model-tests)

    add_executable(rimes-capsule-protocol-tests
        "${RIMES_CAPSULE_ROOT}/tests/capsule_protocol_tests.cpp"
    )
    target_link_libraries(rimes-capsule-protocol-tests PRIVATE RimesCapsuleCore)
    rimes_harden(rimes-capsule-protocol-tests)

    add_executable(rimes-capsule-click-tests
        "${RIMES_CAPSULE_ROOT}/tests/capsule_click_tests.cpp"
    )
    target_link_libraries(rimes-capsule-click-tests PRIVATE RimesCapsuleCore)
    rimes_harden(rimes-capsule-click-tests)

    add_executable(rimes-capsule-clipboard-tests
        "${RIMES_CAPSULE_ROOT}/tests/capsule_clipboard_tests.cpp"
    )
    target_link_libraries(rimes-capsule-clipboard-tests PRIVATE RimesCapsuleCore)
    rimes_harden(rimes-capsule-clipboard-tests)

    add_test(NAME RimesCapsuleStore COMMAND rimes-capsule-store-tests)
    add_test(NAME RimesCapsuleModel COMMAND rimes-capsule-model-tests)
    add_test(NAME RimesCapsuleProtocol COMMAND rimes-capsule-protocol-tests)
    add_test(NAME RimesCapsuleClick COMMAND rimes-capsule-click-tests)
    add_test(NAME RimesCapsuleClipboard COMMAND rimes-capsule-clipboard-tests)
    add_executable(rimes-capsule-password-tests "${RIMES_CAPSULE_ROOT}/tests/capsule_password_tests.cpp")
    target_compile_definitions(rimes-capsule-password-tests PRIVATE NATIVE_PASSWORD_FIXTURES="${RIMES_CAPSULE_ROOT}/tests/fixtures/native-password.json")
    target_link_libraries(rimes-capsule-password-tests PRIVATE RimesCapsuleCore)
    rimes_harden(rimes-capsule-password-tests)
    add_test(NAME RimesCapsulePassword COMMAND rimes-capsule-password-tests)
    add_test(NAME capsule-native-password-fixtures COMMAND "${Python3_EXECUTABLE}" "${RIMES_CAPSULE_ROOT}/tests/verify_native_password.py")
    add_executable(rimes-capsule-modules-tests "${RIMES_CAPSULE_ROOT}/tests/capsule_modules_tests.cpp")
    target_link_libraries(rimes-capsule-modules-tests PRIVATE RimesCapsuleCore)
    target_compile_definitions(rimes-capsule-modules-tests PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
    rimes_harden(rimes-capsule-modules-tests)
    add_test(NAME RimesCapsuleModules COMMAND rimes-capsule-modules-tests)
    if(TARGET rimes-capsule)
        set(pointer_protocol wlr-virtual-pointer-unstable-v1)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/${pointer_protocol}-client-protocol.h"
                                  "${CMAKE_CURRENT_BINARY_DIR}/generated/${pointer_protocol}-protocol.c"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
            COMMAND "${CAPSULE_WAYLAND_SCANNER}" client-header "${RIMES_CAPSULE_ROOT}/protocols/${pointer_protocol}.xml" "${CMAKE_CURRENT_BINARY_DIR}/generated/${pointer_protocol}-client-protocol.h"
            COMMAND "${CAPSULE_WAYLAND_SCANNER}" private-code "${RIMES_CAPSULE_ROOT}/protocols/${pointer_protocol}.xml" "${CMAKE_CURRENT_BINARY_DIR}/generated/${pointer_protocol}-protocol.c"
            DEPENDS "${RIMES_CAPSULE_ROOT}/protocols/${pointer_protocol}.xml" VERBATIM)
        add_executable(rimes-capture-pointer-fixture "${RIMES_CAPSULE_ROOT}/tests/capture_pointer_fixture.c"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/${pointer_protocol}-client-protocol.h"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/${pointer_protocol}-protocol.c")
        target_include_directories(rimes-capture-pointer-fixture PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(rimes-capture-pointer-fixture PRIVATE PkgConfig::CapsuleWayland)
        find_program(RIMES_DESKTOP_TEST_PYTHON NAMES python3 PATHS /usr/bin NO_DEFAULT_PATH REQUIRED)
        add_executable(rimes-capture-controller-tests "${RIMES_CAPSULE_ROOT}/tests/capture_controller_tests.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capture_controller.cpp" "${RIMES_CAPSULE_ROOT}/ui/capsule_manager.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_password_view.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_note_editor.cpp" "${RIMES_CAPSULE_ROOT}/ui/capsule_resource_editor.cpp")
        target_include_directories(rimes-capture-controller-tests PRIVATE "${RIMES_CAPSULE_ROOT}/ui")
        target_link_libraries(rimes-capture-controller-tests PRIVATE RimesCapsuleCore RimesDesktopUI RimesClipboardDesktop)
        target_compile_definitions(rimes-capture-controller-tests PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
        add_dependencies(rimes-capture-controller-tests rimes-capsule-capture rimes-capsule-preview)
        rimes_harden(rimes-capture-controller-tests)
        add_test(NAME capsule-capture-controller COMMAND xvfb-run -a $<TARGET_FILE:rimes-capture-controller-tests>)
        set_tests_properties(capsule-capture-controller PROPERTIES ENVIRONMENT "G_DEBUG=fatal-criticals")
        add_executable(rimes-capsule-capture-tests "${RIMES_CAPSULE_ROOT}/tests/capsule_capture_tests.cpp")
        target_compile_definitions(rimes-capsule-capture-tests PRIVATE NATIVE_CAPTURE_FIXTURES="${RIMES_CAPSULE_ROOT}/tests/fixtures/native-capture.json")
        add_test(NAME capsule-native-capture-fixtures COMMAND "${Python3_EXECUTABLE}" "${RIMES_CAPSULE_ROOT}/tests/verify_native_capture.py")
        target_include_directories(rimes-capsule-capture-tests PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(rimes-capsule-capture-tests PRIVATE RimesCapsuleCore ${GTK3_LIBRARIES})
        rimes_harden(rimes-capsule-capture-tests)
        add_dependencies(rimes-capsule-capture-tests rimes-capsule-preview rimes-capsule-capture)
        add_test(NAME RimesCapsuleCapture COMMAND rimes-capsule-capture-tests)
        add_test(NAME capsule-capture-x11 COMMAND dbus-run-session -- xvfb-run -a -s "-screen 0 800x600x24" "${RIMES_DESKTOP_TEST_PYTHON}" "${RIMES_CAPSULE_ROOT}/tests/capture_backend_test.py" "$<TARGET_FILE:rimes-capsule-capture>")
        add_test(NAME capsule-capture-portal COMMAND dbus-run-session -- "${RIMES_DESKTOP_TEST_PYTHON}" "${RIMES_CAPSULE_ROOT}/tests/capture_portal_test.py" "$<TARGET_FILE:rimes-capsule-capture>")
        add_test(NAME capsule-capture-pipewire COMMAND dbus-run-session -- "${RIMES_DESKTOP_TEST_PYTHON}" "${RIMES_CAPSULE_ROOT}/tests/capture_pipewire_test.py" "$<TARGET_FILE:rimes-capsule-capture>")
        add_executable(rimes-capsule-resources-tests "${RIMES_CAPSULE_ROOT}/tests/capsule_resources_tests.cpp")
        target_include_directories(rimes-capsule-resources-tests PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(rimes-capsule-resources-tests PRIVATE RimesCapsuleCore ${GTK3_LIBRARIES})
        target_compile_definitions(rimes-capsule-resources-tests PRIVATE NATIVE_REFERENCE_FIXTURES="${RIMES_CAPSULE_ROOT}/tests/fixtures/native-reference.json")
        rimes_harden(rimes-capsule-resources-tests)
        add_dependencies(rimes-capsule-resources-tests rimes-capsule-preview)
        add_test(NAME RimesCapsuleResources COMMAND rimes-capsule-resources-tests)
        add_test(NAME capsule-native-reference-fixtures COMMAND "${Python3_EXECUTABLE}" "${RIMES_CAPSULE_ROOT}/tests/verify_native_reference.py")
        add_executable(rimes-capsule-note-editor-tests "${RIMES_CAPSULE_ROOT}/tests/capsule_note_editor_tests.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_note_editor.cpp")
        target_include_directories(rimes-capsule-note-editor-tests PRIVATE "${RIMES_CAPSULE_ROOT}/ui")
        target_link_libraries(rimes-capsule-note-editor-tests PRIVATE RimesCapsuleCore RimesDesktopUI)
        target_compile_definitions(rimes-capsule-note-editor-tests PRIVATE NATIVE_NOTES_FIXTURES="${RIMES_CAPSULE_ROOT}/tests/fixtures/native-notes.json")
        rimes_harden(rimes-capsule-note-editor-tests)
        add_executable(rimes-mailbox-session-tests "${RIMES_MAILBOX_ROOT}/tests/mailbox_session_tests.cpp" "${RIMES_MAILBOX_ROOT}/ui/mailbox_session.cpp" src/session/session_monitor.cpp)
        target_include_directories(rimes-mailbox-session-tests PRIVATE src tests)
        target_link_libraries(rimes-mailbox-session-tests PRIVATE RimesMailboxUI Fcitx5::Utils Threads::Threads)
        target_compile_definitions(rimes-mailbox-session-tests PRIVATE RIMES_MAILBOX_FIXTURE="${RIMES_MAILBOX_ROOT}/tests/fixtures/native-mailbox.json")
        rimes_harden(rimes-mailbox-session-tests)
        add_test(NAME mailbox-session-protection COMMAND dbus-run-session -- xvfb-run -a "${RIMES_DESKTOP_TEST_PYTHON}" "${RIMES_CAPSULE_ROOT}/tests/password_ui_test.py" $<TARGET_FILE:rimes-mailbox-session-tests>)
        set_tests_properties(mailbox-session-protection PROPERTIES ENVIRONMENT "RIMES_PRIVATE_SESSION_TEST=1")
        add_test(NAME mailbox-native-contract COMMAND "${Python3_EXECUTABLE}" "${RIMES_MAILBOX_ROOT}/verify_native_contract.py")
        add_executable(rimes-mailbox-terminal-tests "${RIMES_MAILBOX_ROOT}/tests/terminal_tests.cpp")
        target_link_libraries(rimes-mailbox-terminal-tests PRIVATE RimesMailboxUI)
        rimes_harden(rimes-mailbox-terminal-tests)
        add_test(NAME mailbox-terminal COMMAND dbus-run-session -- xvfb-run -a "${RIMES_DESKTOP_TEST_PYTHON}" "${RIMES_CAPSULE_ROOT}/tests/password_ui_test.py" $<TARGET_FILE:rimes-mailbox-terminal-tests> "${RIMES_MAILBOX_ROOT}/tests/terminal_fixture.py" "${RIMES_MAILBOX_ROOT}/tests/fixtures/public-pixel.png")
        set_tests_properties(mailbox-terminal PROPERTIES ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(rimes-mailbox-chat-ui-tests "${RIMES_MAILBOX_ROOT}/tests/chat_ui_tests.cpp")
        target_link_libraries(rimes-mailbox-chat-ui-tests PRIVATE RimesMailboxUI)
        rimes_harden(rimes-mailbox-chat-ui-tests)
        add_test(NAME mailbox-chat-ui COMMAND dbus-run-session -- xvfb-run -a "${RIMES_DESKTOP_TEST_PYTHON}" "${RIMES_CAPSULE_ROOT}/tests/password_ui_test.py" $<TARGET_FILE:rimes-mailbox-chat-ui-tests> "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/claude_fixture.py")
        add_executable(rimes-mailbox-ui-tests "${RIMES_MAILBOX_ROOT}/tests/mailbox_ui_tests.cpp")
        target_link_libraries(rimes-mailbox-ui-tests PRIVATE RimesMailboxUI)
        target_compile_definitions(rimes-mailbox-ui-tests PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}" RIMES_MAILBOX_FIXTURE="${RIMES_MAILBOX_ROOT}/tests/fixtures/native-mailbox.json")
        rimes_harden(rimes-mailbox-ui-tests)
        add_test(NAME mailbox-ui COMMAND dbus-run-session -- xvfb-run -a "${RIMES_DESKTOP_TEST_PYTHON}" "${RIMES_CAPSULE_ROOT}/tests/password_ui_test.py" $<TARGET_FILE:rimes-mailbox-ui-tests>)
        add_test(NAME mailbox-primary-process COMMAND dbus-run-session -- xvfb-run -a "${RIMES_DESKTOP_TEST_PYTHON}" "${RIMES_CAPSULE_ROOT}/tests/password_ui_test.py" "${RIMES_DESKTOP_TEST_PYTHON}" "${RIMES_MAILBOX_ROOT}/tests/mailbox_process_test.py" $<TARGET_FILE:rimes-mailbox> "${RIMES_MAILBOX_ROOT}/tests/fixtures/native-mailbox.json")
        add_executable(rimes-capsule-password-ui-tests "${RIMES_CAPSULE_ROOT}/tests/capsule_password_ui_tests.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_password_view.cpp")
        target_include_directories(rimes-capsule-password-ui-tests PRIVATE "${RIMES_CAPSULE_ROOT}/ui")
        target_link_libraries(rimes-capsule-password-ui-tests PRIVATE RimesCapsuleCore RimesDesktopUI RimesClipboardDesktop)
        target_compile_definitions(rimes-capsule-password-ui-tests PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
        rimes_harden(rimes-capsule-password-ui-tests)
        add_test(NAME capsule-password-ui COMMAND dbus-run-session -- xvfb-run -a "${RIMES_DESKTOP_TEST_PYTHON}" "${RIMES_CAPSULE_ROOT}/tests/password_ui_test.py" $<TARGET_FILE:rimes-capsule-password-ui-tests>)
        set_tests_properties(capsule-password-ui PROPERTIES ENVIRONMENT "G_DEBUG=fatal-criticals")
        add_executable(rimes-capsule-manager-tests "${RIMES_CAPSULE_ROOT}/tests/capsule_manager_tests.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_manager.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_password_view.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_note_editor.cpp"
            "${RIMES_CAPSULE_ROOT}/ui/capsule_resource_editor.cpp")
        target_include_directories(rimes-capsule-manager-tests PRIVATE "${RIMES_CAPSULE_ROOT}/ui")
        target_link_libraries(rimes-capsule-manager-tests PRIVATE RimesCapsuleCore RimesDesktopUI RimesClipboardDesktop)
        target_compile_definitions(rimes-capsule-manager-tests PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}" NATIVE_NOTES_FIXTURES="${RIMES_CAPSULE_ROOT}/tests/fixtures/native-notes.json")
        rimes_harden(rimes-capsule-manager-tests)
        if(RIMES_XVFB_RUN)
            add_test(NAME capsule-note-editor-ui COMMAND ${RIMES_XVFB_RUN} -a $<TARGET_FILE:rimes-capsule-note-editor-tests>)
            set_tests_properties(capsule-note-editor-ui PROPERTIES ENVIRONMENT "G_DEBUG=fatal-criticals")
            add_test(NAME capsule-manager-ui COMMAND ${RIMES_XVFB_RUN} -a $<TARGET_FILE:rimes-capsule-manager-tests>)
            set_tests_properties(capsule-manager-ui PROPERTIES ENVIRONMENT "G_DEBUG=fatal-criticals")
        endif()
    endif()

    pkg_check_modules(GTK3 gtk+-3.0)
    if(GTK3_FOUND)
        add_executable(rimes-gtk-host tests/e2e/gtk_host.c)
        target_include_directories(rimes-gtk-host PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(rimes-gtk-host PRIVATE ${GTK3_LIBRARIES})
        target_compile_options(rimes-gtk-host PRIVATE ${GTK3_CFLAGS_OTHER})
    endif()

    if(RIMES_BUILD_QT_HOST)
        find_package(Qt5 QUIET COMPONENTS Widgets)
        if(Qt5Widgets_FOUND)
            add_executable(rimes-qt-host tests/e2e/qt_host.cpp)
            target_link_libraries(rimes-qt-host PRIVATE Qt5::Widgets)
            set_target_properties(rimes-qt-host PROPERTIES
                AUTOMOC OFF
            )
        endif()
    endif()

    if(RIMES_BUILD_FCITX5)
        add_executable(rimes-fcitx-e2e tests/fcitx_e2e.cpp)
        target_link_libraries(rimes-fcitx-e2e PRIVATE
            Fcitx5::Core
            Fcitx5::Module::TestFrontend
            RimesEngine
        )
        rimes_harden(rimes-fcitx-e2e)

        add_executable(rimes-buffer-derived-host-tests tests/buffer_derived_host_tests.cpp src/buffer/buffer_service.cpp src/buffer/buffer_mailbox.cpp src/buffer/buffer_translation.cpp src/buffer/buffer_delivery.cpp)
        target_include_directories(rimes-buffer-derived-host-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(rimes-buffer-derived-host-tests PRIVATE Fcitx5::Core RimesMailboxCore RimesBufferCore Threads::Threads)
        rimes_harden(rimes-buffer-derived-host-tests)
        add_test(NAME buffer-derived-host COMMAND rimes-buffer-derived-host-tests)

        add_executable(rimes-buffer-translation-host-tests tests/buffer_translation_host_tests.cpp src/buffer/buffer_service.cpp src/buffer/buffer_mailbox.cpp src/buffer/buffer_translation.cpp src/buffer/buffer_delivery.cpp)
        target_include_directories(rimes-buffer-translation-host-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(rimes-buffer-translation-host-tests PRIVATE Fcitx5::Core RimesMailboxCore RimesBufferCore Threads::Threads)
        rimes_harden(rimes-buffer-translation-host-tests)
        add_test(NAME buffer-translation-host COMMAND rimes-buffer-translation-host-tests)

        add_executable(rimes-mailbox-buffer-host-tests tests/mailbox_buffer_host_tests.cpp src/buffer/buffer_service.cpp src/buffer/buffer_mailbox.cpp src/buffer/buffer_translation.cpp src/buffer/buffer_delivery.cpp)
        target_include_directories(rimes-mailbox-buffer-host-tests PRIVATE src)
        target_link_libraries(rimes-mailbox-buffer-host-tests PRIVATE Fcitx5::Core RimesMailboxCore Threads::Threads)
        rimes_harden(rimes-mailbox-buffer-host-tests)
        add_test(NAME mailbox-buffer-host COMMAND rimes-mailbox-buffer-host-tests)
        set_tests_properties(mailbox-buffer-host PROPERTIES TIMEOUT 60)
        add_executable(rimes-buffer-delivery-host-tests tests/buffer_delivery_host_tests.cpp src/buffer/buffer_service.cpp src/buffer/buffer_mailbox.cpp src/buffer/buffer_translation.cpp src/buffer/buffer_delivery.cpp)
        target_include_directories(rimes-buffer-delivery-host-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(rimes-buffer-delivery-host-tests PRIVATE Fcitx5::Core RimesMailboxCore RimesBufferCore Threads::Threads)
        rimes_harden(rimes-buffer-delivery-host-tests)
        add_test(NAME buffer-delivery-host COMMAND rimes-buffer-delivery-host-tests)

        add_executable(rimes-session-monitor-tests tests/session_monitor_tests.cpp src/session/session_monitor.cpp)
        target_include_directories(rimes-session-monitor-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(rimes-session-monitor-tests PRIVATE Fcitx5::Core)
        rimes_harden(rimes-session-monitor-tests)
        find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
        add_test(NAME session-protection-dbus COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:rimes-session-monitor-tests>)
        set_tests_properties(session-protection-dbus PROPERTIES ENVIRONMENT "RIMES_PRIVATE_SESSION_TEST=1")

        add_executable(rimes-desktop-shortcuts-tests tests/desktop_shortcuts_tests.cpp src/desktop/desktop_shortcuts.cpp)
        target_include_directories(rimes-desktop-shortcuts-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(rimes-desktop-shortcuts-tests PRIVATE Fcitx5::Core RimesPluginCore PkgConfig::RimesXCB)
        rimes_harden(rimes-desktop-shortcuts-tests)
        add_dependencies(rimes-desktop-shortcuts-tests rimes-launch)
        add_test(NAME desktop-shortcuts-portal COMMAND ${DBUS_RUN_SESSION} --config-file=${CMAKE_CURRENT_SOURCE_DIR}/tests/portal_fixture_bus.conf -- $<TARGET_FILE:rimes-desktop-shortcuts-tests> --portal $<TARGET_FILE:rimes-launch>)
        set_tests_properties(desktop-shortcuts-portal PROPERTIES ENVIRONMENT "RIMES_PRIVATE_SESSION_TEST=1")
        find_program(RIMES_XDOTOOL xdotool)
        if(RIMES_BUFFER_XVFB_RUN AND RIMES_XDOTOOL)
            add_test(NAME desktop-shortcuts-x11 COMMAND ${RIMES_BUFFER_XVFB_RUN} -a ${DBUS_RUN_SESSION} -- $<TARGET_FILE:rimes-desktop-shortcuts-tests> --x11)
        endif()

        add_executable(rimes-buffer-fcitx-e2e tests/buffer_fcitx_e2e.cpp)
        target_link_libraries(rimes-buffer-fcitx-e2e PRIVATE
            RimesPluginCore
            Fcitx5::Core
            Fcitx5::Module::TestFrontend
            RimesEngine
            RimesBufferCore
        )
        rimes_harden(rimes-buffer-fcitx-e2e)

        add_executable(rimes-buffer-ui-stub tests/buffer_ui_stub.cpp)
        target_link_libraries(rimes-buffer-ui-stub PRIVATE RimesBufferCore)
        rimes_harden(rimes-buffer-ui-stub)
        set_target_properties(rimes-buffer-ui-stub PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )

        add_executable(rimes-buffer-respawn-e2e tests/buffer_respawn_e2e.cpp)
        target_link_libraries(rimes-buffer-respawn-e2e PRIVATE
            Fcitx5::Core
            Fcitx5::Module::TestFrontend
            RimesEngine
            RimesBufferCore
        )
        rimes_harden(rimes-buffer-respawn-e2e)

        add_executable(rimes-claude-fcitx-e2e tests/claude_fcitx_e2e.cpp)
        target_link_libraries(rimes-claude-fcitx-e2e PRIVATE Fcitx5::Core Fcitx5::Module::TestFrontend RimesEngine RimesBufferCore)
        target_compile_definitions(rimes-claude-fcitx-e2e PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
        rimes_harden(rimes-claude-fcitx-e2e)

        add_executable(rimes-codex-fcitx-e2e tests/codex_fcitx_e2e.cpp)
        target_link_libraries(rimes-codex-fcitx-e2e PRIVATE Fcitx5::Core Fcitx5::Module::TestFrontend RimesEngine RimesMailboxCore)
        rimes_harden(rimes-codex-fcitx-e2e)
        add_executable(rimes-chord-fcitx-e2e tests/chord_fcitx_e2e.cpp)
        target_link_libraries(rimes-chord-fcitx-e2e PRIVATE Fcitx5::Core Fcitx5::Module::TestFrontend RimesChordCore RimesDesktopUI)
        target_compile_definitions(rimes-chord-fcitx-e2e PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
        rimes_harden(rimes-chord-fcitx-e2e)

        add_executable(rimes-metrics-fcitx-e2e tests/metrics_fcitx_e2e.cpp src/statistics/metrics_keys.cpp)
        target_include_directories(rimes-metrics-fcitx-e2e PRIVATE src)
        target_link_libraries(rimes-metrics-fcitx-e2e PRIVATE Fcitx5::Core Fcitx5::Module::TestFrontend RimesStatisticsCore RimesChordCore RimesDesktopUI)
        target_compile_definitions(rimes-metrics-fcitx-e2e PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
        rimes_harden(rimes-metrics-fcitx-e2e)

        add_executable(rimes-practice-fcitx-e2e tests/practice_fcitx_e2e.cpp)
        target_link_libraries(rimes-practice-fcitx-e2e PRIVATE Fcitx5::Core Fcitx5::Module::TestFrontend RimesPracticeCore RimesChordCore RimesPracticeUI)
        target_compile_definitions(rimes-practice-fcitx-e2e PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
        # The service verifies both D-Bus credentials and the settings process
        # name. Keep the isolated probe in its own directory, never install it.
        set_target_properties(rimes-practice-fcitx-e2e PROPERTIES OUTPUT_NAME rimes-settings RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/practice-probe")
        rimes_harden(rimes-practice-fcitx-e2e)

        add_executable(rimes-gateway-fcitx-e2e tests/gateway_fcitx_e2e.cpp)
        target_include_directories(rimes-gateway-fcitx-e2e PRIVATE "${RIMES_MAILBOX_ROOT}/tests")
        target_link_libraries(rimes-gateway-fcitx-e2e PRIVATE Fcitx5::Core Fcitx5::Module::TestFrontend RimesEngine RimesMailboxCore)
        rimes_harden(rimes-gateway-fcitx-e2e)
        add_executable(rimes-capsule-fcitx-e2e tests/capsule_fcitx_e2e.cpp)
        target_link_libraries(rimes-capsule-fcitx-e2e PRIVATE
            Fcitx5::Core
            Fcitx5::Module::TestFrontend
            RimesEngine
            RimesCapsuleCore
        )
        target_compile_definitions(rimes-capsule-fcitx-e2e PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
        rimes_harden(rimes-capsule-fcitx-e2e)

        add_executable(rimes-capsule-ui-stub tests/capsule_ui_stub.cpp)
        target_link_libraries(rimes-capsule-ui-stub PRIVATE RimesCapsuleCore)
        rimes_harden(rimes-capsule-ui-stub)
        set_target_properties(rimes-capsule-ui-stub PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )

        add_executable(rimes-capsule-respawn-e2e tests/capsule_respawn_e2e.cpp)
        target_link_libraries(rimes-capsule-respawn-e2e PRIVATE
            Fcitx5::Core
            Fcitx5::Module::TestFrontend
            RimesEngine
            RimesCapsuleCore
        )
        rimes_harden(rimes-capsule-respawn-e2e)

        # Layout expected by fcitx::setupTestingEnvironment.
        # setupTestingEnvironment treats each data dir as an fcitx5 data root,
        # so addon/ and inputmethod/ sit directly under test-data/.
        file(MAKE_DIRECTORY
            "${CMAKE_BINARY_DIR}/test-data/addon"
            "${CMAKE_BINARY_DIR}/test-data/inputmethod"
        )
        add_custom_command(TARGET rimes POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy
                "${CMAKE_BINARY_DIR}/rimes-addon.conf"
                "${CMAKE_BINARY_DIR}/test-data/addon/rimes.conf"
            COMMAND "${CMAKE_COMMAND}" -E copy
                "${CMAKE_BINARY_DIR}/rimes.conf"
                "${CMAKE_BINARY_DIR}/test-data/inputmethod/rimes.conf"
        )
    endif()
endif()
