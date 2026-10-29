cmake_minimum_required(VERSION 3.27)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RimesLinuxRuntime LANGUAGES CXX)
    include(CTest)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
pkg_check_modules(RuntimeCurl REQUIRED IMPORTED_TARGET libcurl)
get_filename_component(RIMES_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${RIMES_REPOSITORY_ROOT}/scripts/prepare-official-plugins.py"
    WORKING_DIRECTORY "${RIMES_REPOSITORY_ROOT}" RESULT_VARIABLE RIMES_PLUGIN_IMPORT_RESULT)
if(NOT RIMES_PLUGIN_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Initialize the pinned OfficialPlugins dependency and resolve import errors before building")
endif()
add_subdirectory("${RIMES_REPOSITORY_ROOT}/Shared/ProductDocuments" "product-documents")
add_library(RimesRuntimeStorage STATIC core/private_files.cpp)
target_compile_features(RimesRuntimeStorage PUBLIC cxx_std_17)
target_include_directories(RimesRuntimeStorage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core")
target_link_libraries(RimesRuntimeStorage PUBLIC OpenSSL::Crypto)
set_target_properties(RimesRuntimeStorage PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(RimesRuntimeStorage PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(RimesProcess STATIC core/process.cpp core/background_jobs.cpp core/cli_session.cpp)
target_compile_features(RimesProcess PUBLIC cxx_std_17)
target_include_directories(RimesProcess PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core")
target_link_libraries(RimesProcess PUBLIC Threads::Threads)
set_target_properties(RimesProcess PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(RimesProcess PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(RIMES_BUILD_TESTS)
    add_executable(rimes-background-jobs-tests tests/background_jobs_tests.cpp)
    target_link_libraries(rimes-background-jobs-tests PRIVATE RimesProcess)
    add_test(NAME background-jobs COMMAND rimes-background-jobs-tests)
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/ShortcutPreferences.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_shortcut_contract.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_shortcut_contract.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/generated/native_shortcuts.hpp"
    RESULT_VARIABLE RIMES_SHORTCUT_IMPORT_RESULT)
if(NOT RIMES_SHORTCUT_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Review the macOS shortcut contract before building Linux")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_REPOSITORY_ROOT}/OfficialPlugins/native/macos/CapsuleModules.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/CapsuleWindowController.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_capsule_contract.py"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/CapsuleRailSmoke.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/CapsuleRailDetailView.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/CapsuleInteractionSmoke.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/ClipboardHistoryModel.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/ClipboardPasteboardArchive.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/ClipboardHistorySmoke.swift")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_capsule_contract.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/generated/native_capsule_contract.hpp"
    RESULT_VARIABLE RIMES_CAPSULE_IMPORT_RESULT)
if(NOT RIMES_CAPSULE_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Review the native macOS Capsule contract")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/Inbound/LocalGateway.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/Inbound/InboundBus.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/Inbound/GatewayToken.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/SettingsWindow.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mailbox/resources/native-gateway.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mailbox/resources/native-gateway-settings.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mailbox/verify_gateway_contract.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../mailbox/verify_gateway_contract.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/generated/native_gateway_contract.hpp" RESULT_VARIABLE RIMES_GATEWAY_IMPORT_RESULT)
if(NOT RIMES_GATEWAY_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Regenerate the actual native gateway and settings contracts")
endif()
add_library(RimesPluginCore STATIC core/plugin_registry.cpp core/plugin_store.cpp core/package_binding.cpp core/official_catalog.cpp core/http_download.cpp core/configuration.cpp core/shortcuts.cpp core/gateway_access.cpp)
target_compile_features(RimesPluginCore PUBLIC cxx_std_17)
target_include_directories(RimesPluginCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(RimesPluginCore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/generated")
# Reuse the repository's pinned, licensed header-only JSON dependency.
target_include_directories(RimesPluginCore SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../windows/native/third_party")
target_link_libraries(RimesPluginCore PUBLIC RimesRuntimeStorage PRIVATE PkgConfig::RuntimeCurl)
set_target_properties(RimesPluginCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(RimesPluginCore PRIVATE -Wall -Wextra -Wpedantic -Werror)

# The editor and input adapter share the actual native keymap contract.
pkg_check_modules(ChordICU REQUIRED IMPORTED_TARGET icu-i18n icu-uc)
set(RIMES_CHORD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../chord")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_CHORD_ROOT}/verify_contract.py" "${RIMES_CHORD_ROOT}/resources/native-chord.json")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${RIMES_CHORD_ROOT}/verify_contract.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/generated/native_chord_contract.hpp"
    RESULT_VARIABLE RIMES_CHORD_IMPORT_RESULT)
if(NOT RIMES_CHORD_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Export and review the actual macOS chord contract")
endif()
add_custom_target(RimesChordContract
    COMMAND "${Python3_EXECUTABLE}" "${RIMES_CHORD_ROOT}/verify_contract.py"
    COMMENT "Checking the actual macOS chord contract")
add_library(RimesChordCore STATIC "${RIMES_CHORD_ROOT}/core/chord_core.cpp"
    "${RIMES_CHORD_ROOT}/core/chord_store.cpp" "${RIMES_CHORD_ROOT}/core/chord_deployment.cpp"
    "${RIMES_CHORD_ROOT}/core/chord_learning.cpp")
add_dependencies(RimesChordCore RimesChordContract)
target_include_directories(RimesChordCore PUBLIC "${RIMES_CHORD_ROOT}/core")
target_link_libraries(RimesChordCore PUBLIC RimesPluginCore PRIVATE PkgConfig::ChordICU yaml-cpp::yaml-cpp)
target_compile_options(RimesChordCore PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(RimesChordCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(RIMES_BUILD_TESTS)
    add_executable(rimes-chord-core-tests "${RIMES_CHORD_ROOT}/tests/chord_core_tests.cpp")
    target_link_libraries(rimes-chord-core-tests PRIVATE RimesChordCore)
    add_test(NAME chord-core COMMAND rimes-chord-core-tests)
    add_executable(rimes-chord-storage-tests "${RIMES_CHORD_ROOT}/tests/chord_storage_tests.cpp")
    target_link_libraries(rimes-chord-storage-tests PRIVATE RimesChordCore)
    add_test(NAME chord-storage COMMAND rimes-chord-storage-tests)
endif()

set(RIMES_METRICS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../statistics")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_METRICS_ROOT}/verify_contract.py" "${RIMES_METRICS_ROOT}/resources/native-statistics.json")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${RIMES_METRICS_ROOT}/verify_contract.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/generated/native_metrics_contract.hpp"
    RESULT_VARIABLE RIMES_METRICS_IMPORT_RESULT)
if(NOT RIMES_METRICS_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Export and review the actual macOS statistics contract")
endif()
add_custom_target(RimesMetricsContract
    COMMAND "${Python3_EXECUTABLE}" "${RIMES_METRICS_ROOT}/verify_contract.py")
add_library(RimesStatisticsCore STATIC "${RIMES_METRICS_ROOT}/core/metrics_core.cpp"
    "${RIMES_METRICS_ROOT}/core/metrics_store.cpp" "${RIMES_METRICS_ROOT}/core/metrics_worker.cpp")
add_dependencies(RimesStatisticsCore RimesMetricsContract)
target_include_directories(RimesStatisticsCore PUBLIC "${RIMES_METRICS_ROOT}/core")
target_link_libraries(RimesStatisticsCore PUBLIC RimesPluginCore Threads::Threads)
target_compile_options(RimesStatisticsCore PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(RimesStatisticsCore PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(RIMES_PRACTICE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../typing-practice")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_PRACTICE_ROOT}/verify_contract.py" "${RIMES_PRACTICE_ROOT}/resources/native-practice.json")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${RIMES_PRACTICE_ROOT}/verify_contract.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/generated/native_practice_articles.hpp"
    RESULT_VARIABLE RIMES_PRACTICE_IMPORT_RESULT)
if(NOT RIMES_PRACTICE_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Export and review the actual macOS typing practice contract")
endif()
add_custom_target(RimesPracticeContract
    COMMAND "${Python3_EXECUTABLE}" "${RIMES_PRACTICE_ROOT}/verify_contract.py"
    COMMENT "Checking actual native practice, editor and history observations")
add_library(RimesPracticeCore STATIC "${RIMES_PRACTICE_ROOT}/core/typing_practice_core.cpp"
    "${RIMES_PRACTICE_ROOT}/core/practice_store.cpp" "${RIMES_PRACTICE_ROOT}/core/practice_worker.cpp")
add_dependencies(RimesPracticeCore RimesPracticeContract)
target_include_directories(RimesPracticeCore PUBLIC "${RIMES_PRACTICE_ROOT}/core")
target_link_libraries(RimesPracticeCore PUBLIC RimesPluginCore Threads::Threads PRIVATE PkgConfig::ChordICU)
target_compile_options(RimesPracticeCore PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(RimesPracticeCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(RIMES_BUILD_TESTS)
    add_executable(rimes-practice-core-tests "${RIMES_PRACTICE_ROOT}/tests/practice_core_tests.cpp")
    target_link_libraries(rimes-practice-core-tests PRIVATE RimesPracticeCore)
    add_test(NAME practice-native-contract COMMAND rimes-practice-core-tests "${RIMES_PRACTICE_ROOT}/resources/native-practice.json")
    add_executable(rimes-practice-storage-tests "${RIMES_PRACTICE_ROOT}/tests/practice_store_tests.cpp")
    target_link_libraries(rimes-practice-storage-tests PRIVATE RimesPracticeCore)
    add_test(NAME practice-storage COMMAND rimes-practice-storage-tests "${RIMES_PRACTICE_ROOT}/resources/native-practice.json")
endif()
if(RIMES_BUILD_TESTS)
    add_executable(rimes-metrics-core-tests "${RIMES_METRICS_ROOT}/tests/metrics_core_tests.cpp")
    target_link_libraries(rimes-metrics-core-tests PRIVATE RimesStatisticsCore)
    add_test(NAME metrics-native-contract COMMAND rimes-metrics-core-tests "${RIMES_METRICS_ROOT}/resources/native-statistics.json")
    add_executable(rimes-metrics-storage-tests "${RIMES_METRICS_ROOT}/tests/metrics_storage_tests.cpp")
    target_link_libraries(rimes-metrics-storage-tests PRIVATE RimesStatisticsCore)
    add_test(NAME metrics-private-storage COMMAND rimes-metrics-storage-tests)
endif()

if(NOT DEFINED RIMES_PRODUCT_VERSION)
    file(READ "${RIMES_REPOSITORY_ROOT}/VERSION" RIMES_PRODUCT_VERSION)
    string(STRIP "${RIMES_PRODUCT_VERSION}" RIMES_PRODUCT_VERSION)
endif()
add_executable(rimes-plugins ctl/plugin_ctl.cpp)
target_link_libraries(rimes-plugins PRIVATE RimesPluginCore)
target_compile_definitions(rimes-plugins PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
include(GNUInstallDirs)
install(TARGETS rimes-plugins DESTINATION "${CMAKE_INSTALL_BINDIR}")
add_executable(rimes-translation ctl/translation_ctl.cpp)
target_link_libraries(rimes-translation PRIVATE RimesPluginCore RimesProcess)
target_compile_options(rimes-translation PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(RimesTranslationLocations INTERFACE)
target_compile_definitions(RimesTranslationLocations INTERFACE
    RIMES_INSTALLED_TRANSLATION_RESOURCES="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rimes/translation"
    RIMES_BUILD_TRANSLATION_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/../translation"
    RIMES_TRANSLATION_PYTHON="${Python3_EXECUTABLE}")
target_link_libraries(rimes-translation PRIVATE RimesTranslationLocations)
install(TARGETS rimes-translation DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES ../translation/runtime.py ../translation/translation_core.py ../translation/resources.lock.json ../translation/NOTICE.md
    DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/rimes/translation")

add_library(RimesTranslationUnits STATIC ../translation/units.cpp)
target_compile_features(RimesTranslationUnits PUBLIC cxx_std_17)
target_include_directories(RimesTranslationUnits PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../translation")
target_compile_options(RimesTranslationUnits PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(RimesTranslationUnits PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(RimesLocalTranslation STATIC core/local_translation.cpp)
target_link_libraries(RimesLocalTranslation PUBLIC RimesPluginCore RimesProcess PRIVATE RimesTranslationLocations)
target_compile_options(RimesLocalTranslation PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(RimesLocalTranslation PROPERTIES POSITION_INDEPENDENT_CODE ON)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/AITextPlugins.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_ai_contract.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_ai_contract.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/generated/native_ai_contract.hpp"
    RESULT_VARIABLE RIMES_AI_IMPORT_RESULT)
if(NOT RIMES_AI_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Review the native macOS AI API request contract")
endif()
add_library(RimesAI STATIC core/ai_api.cpp ../ai/chat_codec.cpp)
target_compile_features(RimesAI PUBLIC cxx_std_17)
target_include_directories(RimesAI PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../ai")
target_link_libraries(RimesAI PUBLIC RimesPluginCore RimesProcess PRIVATE PkgConfig::RuntimeCurl)
target_compile_options(RimesAI PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(RimesAI PROPERTIES POSITION_INDEPENDENT_CODE ON)


set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/AITextPlugins.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/AITextCodexAppServer.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/AITextCodexLogin.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/AITextSkills.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/native-codex.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/native-codex-settings.json"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/SettingsWindow.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/verify_codex_contract.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_codex_contract.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/generated/native_codex_contract.hpp"
    RESULT_VARIABLE RIMES_CODEX_IMPORT_RESULT)
if(NOT RIMES_CODEX_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Export and review the actual macOS Codex connector contract")
endif()
add_custom_target(RimesCodexContract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_codex_contract.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/AITextClaudeLogin.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/AITextClaudeLinuxFixtures.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/native-claude.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/native-claude-settings.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/verify_claude_contract.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_claude_contract.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/generated/native_claude_contract.hpp"
    RESULT_VARIABLE RIMES_CLAUDE_IMPORT_RESULT)
if(NOT RIMES_CLAUDE_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Export and review the actual native Claude channel")
endif()
add_custom_target(RimesClaudeContract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_claude_contract.py")
add_library(RimesClaude STATIC core/claude_channel.cpp core/claude_protocol.cpp)
target_link_libraries(RimesClaude PUBLIC RimesAI)
target_include_directories(RimesClaude PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(RimesClaude PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(RimesClaude PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_dependencies(RimesClaude RimesClaudeContract)
if(RIMES_BUILD_TESTS)
    add_executable(rimes-claude-tests tests/claude_tests.cpp)
    target_link_libraries(rimes-claude-tests PRIVATE RimesClaude)
    target_compile_options(rimes-claude-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME claude-native-contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_claude_contract.py")
    add_test(NAME claude-channel COMMAND rimes-claude-tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_fixture.py")
endif()
add_library(RimesCodex STATIC core/codex_channel.cpp core/codex_protocol.cpp)
target_link_libraries(RimesCodex PUBLIC RimesAI)
target_include_directories(RimesCodex PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(RimesCodex PRIVATE
    RIMES_INSTALLED_CODEX_SKILL="${CMAKE_INSTALL_FULL_DATADIR}/rimes/skills/imagegen"
    RIMES_BUILD_CODEX_SKILL="${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/Resources/Skills/imagegen")
target_compile_options(RimesCodex PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(RimesCodex PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_dependencies(RimesCodex RimesCodexContract)
install(DIRECTORY "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/Resources/Skills/imagegen"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rimes/skills")
if(RIMES_BUILD_TESTS)
    add_test(NAME codex-native-contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_codex_contract.py")
    add_executable(rimes-codex-tests tests/codex_tests.cpp)
    target_link_libraries(rimes-codex-tests PRIVATE RimesCodex)
    target_compile_options(rimes-codex-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME codex-channel COMMAND rimes-codex-tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/codex_fixture.py")
endif()

pkg_check_modules(RuntimeGtk REQUIRED IMPORTED_TARGET gtk+-3.0)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/RimeUI.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/SettingsWindow.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/SettingsRouting.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/BufferWindowController.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/BufferInlineView.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/RimeFixedAccentSwitch.swift"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/ShortcutPreferences.swift"
    "${RIMES_REPOSITORY_ROOT}/Catalog/buffer-plugins.json"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/Resources/PluginIcons/chatgpt.png"
    "${RIMES_REPOSITORY_ROOT}/Sources/RimeBuffer/Resources/PluginIcons/claude.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_visual_tokens.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_visual_tokens.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/generated/native_visual_tokens.hpp"
    RESULT_VARIABLE RIMES_VISUAL_IMPORT_RESULT)
if(NOT RIMES_VISUAL_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Review the native macOS visual contract before building Linux UI")
endif()
add_library(RimesWindowGeometry STATIC core/window_geometry.cpp)
target_link_libraries(RimesWindowGeometry PUBLIC RimesPluginCore)
target_compile_options(RimesWindowGeometry PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(RimesWindowGeometry PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(RimesDesktopUI STATIC ui/desktop.cpp ui/translation_resources.cpp ui/launcher_client.cpp ui/panel_window.cpp)
target_include_directories(RimesDesktopUI PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ui" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(RimesDesktopUI PUBLIC RimesWindowGeometry)
pkg_check_modules(DesktopLayerShell IMPORTED_TARGET gtk-layer-shell-0)
if(DesktopLayerShell_FOUND)
    target_link_libraries(RimesDesktopUI PRIVATE PkgConfig::DesktopLayerShell)
    target_compile_definitions(RimesDesktopUI PRIVATE HAVE_GTK_LAYER_SHELL=1)
endif()
target_link_libraries(RimesDesktopUI PUBLIC RimesPluginCore PkgConfig::RuntimeGtk)
target_compile_options(RimesDesktopUI PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(RimesChordUI STATIC ui/chord_editor.cpp)
target_link_libraries(RimesChordUI PUBLIC RimesDesktopUI RimesChordCore)
target_compile_options(RimesChordUI PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(RimesStatisticsUI STATIC ../statistics/ui/metrics_views.cpp ../statistics/ui/statistics_pane.cpp)
target_include_directories(RimesStatisticsUI PUBLIC ../statistics/ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(RimesStatisticsUI PUBLIC RimesDesktopUI RimesStatisticsCore)
target_compile_options(RimesStatisticsUI PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(RimesPracticeUI STATIC ../typing-practice/ui/practice_editor.cpp ../typing-practice/ui/practice_pane.cpp ../typing-practice/ui/practice_telemetry.cpp)
target_include_directories(RimesPracticeUI PUBLIC ../typing-practice/ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(RimesPracticeUI PUBLIC RimesStatisticsUI RimesPracticeCore RimesTypingMaterials)
target_compile_options(RimesPracticeUI PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(rimes-settings ui/settings.cpp ui/application_update.cpp)
target_link_libraries(rimes-settings PRIVATE RimesProductDocuments PkgConfig::RuntimeCurl RimesDesktopUI RimesCodex RimesClaude RimesChordUI RimesStatisticsUI RimesPracticeUI RimesSchemeImport yaml-cpp::yaml-cpp)
target_compile_definitions(rimes-settings PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}")
add_dependencies(rimes-settings rimes-translation)
target_compile_options(rimes-settings PRIVATE -Wall -Wextra -Wpedantic -Werror)
install(TARGETS rimes-settings DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES ui/org.scholay.rimes.settings.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
add_executable(rimes-launch ctl/launch_ctl.cpp)
target_link_libraries(rimes-launch PRIVATE RimesDesktopUI)
target_compile_options(rimes-launch PRIVATE -Wall -Wextra -Wpedantic -Werror)
install(TARGETS rimes-launch DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES ui/org.scholay.rimes.buffer.desktop ui/org.scholay.rimes.capsule.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

if(BUILD_TESTING OR RIMES_BUILD_TESTS)
    add_executable(rimes-update-manifest-tests "${RIMES_REPOSITORY_ROOT}/Shared/AppUpdate/manifest_tests.cpp")
    target_include_directories(rimes-update-manifest-tests SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../windows/native/third_party")
    target_compile_features(rimes-update-manifest-tests PRIVATE cxx_std_17)
    target_link_libraries(rimes-update-manifest-tests PRIVATE OpenSSL::Crypto)
    target_compile_options(rimes-update-manifest-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME update-manifest COMMAND rimes-update-manifest-tests
        "${RIMES_REPOSITORY_ROOT}/Shared/AppUpdate/fixtures/signed_manifest.json")
    add_executable(rimes-update-operation-tests tests/update_operation_tests.cpp)
    target_link_libraries(rimes-update-operation-tests PRIVATE RimesRuntimeStorage Threads::Threads)
    target_compile_options(rimes-update-operation-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME update-operation COMMAND rimes-update-operation-tests)
    set_tests_properties(update-manifest update-operation PROPERTIES TIMEOUT 20)
    add_executable(rimes-window-geometry-tests tests/window_geometry_tests.cpp)
    target_link_libraries(rimes-window-geometry-tests PRIVATE RimesWindowGeometry)
    add_test(NAME window-geometry COMMAND rimes-window-geometry-tests)
    add_executable(rimes-ai-codec-tests ../ai/chat_codec_tests.cpp)
    target_link_libraries(rimes-ai-codec-tests PRIVATE RimesAI)
    add_test(NAME ai-codec COMMAND rimes-ai-codec-tests)
    add_executable(rimes-ai-api-tests tests/ai_api_tests.cpp)
    target_link_libraries(rimes-ai-api-tests PRIVATE RimesAI)
    add_test(NAME ai-http COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_http_fixture.py" $<TARGET_FILE:rimes-ai-api-tests>)
    add_executable(rimes-translation-units-tests ../translation/units_tests.cpp)
    target_link_libraries(rimes-translation-units-tests PRIVATE RimesTranslationUnits)
    add_test(NAME translation-units COMMAND rimes-translation-units-tests)
    add_executable(rimes-process-tests tests/process_tests.cpp)
    target_link_libraries(rimes-process-tests PRIVATE RimesProcess)
    add_test(NAME runtime-process COMMAND rimes-process-tests)
    add_test(NAME translation-resources COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../translation/test_runtime.py")
    add_executable(rimes-plugin-registry-tests tests/plugin_registry_tests.cpp)
    target_link_libraries(rimes-plugin-registry-tests PRIVATE RimesPluginCore)
    add_test(NAME plugin-registry COMMAND rimes-plugin-registry-tests)
    add_executable(rimes-plugin-store-tests tests/plugin_store_tests.cpp)
    target_link_libraries(rimes-plugin-store-tests PRIVATE RimesPluginCore Threads::Threads)
    add_test(NAME plugin-store COMMAND rimes-plugin-store-tests)
    add_executable(rimes-configuration-tests tests/configuration_tests.cpp)
    target_link_libraries(rimes-configuration-tests PRIVATE RimesPluginCore)
    add_test(NAME configuration COMMAND rimes-configuration-tests)
    find_program(RIMES_XVFB_RUN xvfb-run)
    if(RIMES_XVFB_RUN)
        add_executable(rimes-practice-desktop-probe "${RIMES_PRACTICE_ROOT}/tests/practice_desktop_probe.cpp")
        target_link_libraries(rimes-practice-desktop-probe PRIVATE RimesPracticeUI)
        set_target_properties(rimes-practice-desktop-probe PROPERTIES OUTPUT_NAME rimes-settings RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/practice-desktop")
        add_executable(rimes-practice-ui-tests "${RIMES_PRACTICE_ROOT}/tests/practice_ui_tests.cpp")
        target_link_libraries(rimes-practice-ui-tests PRIVATE RimesPracticeUI)
        target_compile_definitions(rimes-practice-ui-tests PRIVATE
            PRACTICE_DASHBOARD="${RIMES_PRACTICE_ROOT}/resources/dashboard"
            PRACTICE_CONTRACT="${RIMES_PRACTICE_ROOT}/resources/native-practice.json")
        add_test(NAME practice-ui COMMAND dbus-run-session -- "${RIMES_XVFB_RUN}" -a $<TARGET_FILE:rimes-practice-ui-tests>)
        set_tests_properties(practice-ui PROPERTIES ENVIRONMENT "NO_AT_BRIDGE=1;G_DEBUG=fatal-criticals" TIMEOUT 60)
        add_executable(rimes-metrics-ui-tests "${RIMES_METRICS_ROOT}/tests/metrics_ui_tests.cpp")
        target_link_libraries(rimes-metrics-ui-tests PRIVATE RimesStatisticsUI)
        target_compile_definitions(rimes-metrics-ui-tests PRIVATE RIMES_PRODUCT_VERSION="${RIMES_PRODUCT_VERSION}"
            NATIVE_DASHBOARD_FIXTURE="${RIMES_METRICS_ROOT}/resources/native-dashboard.json")
        add_test(NAME metrics-ui COMMAND dbus-run-session -- "${RIMES_XVFB_RUN}" -a $<TARGET_FILE:rimes-metrics-ui-tests>)
        set_tests_properties(metrics-ui PROPERTIES ENVIRONMENT "NO_AT_BRIDGE=1;G_DEBUG=fatal-criticals" TIMEOUT 60)
        add_executable(rimes-chord-ui-tests "${RIMES_CHORD_ROOT}/tests/chord_ui_tests.cpp")
        target_link_libraries(rimes-chord-ui-tests PRIVATE RimesChordUI)
        add_test(NAME chord-ui COMMAND "${RIMES_XVFB_RUN}" -a $<TARGET_FILE:rimes-chord-ui-tests>)
        set_tests_properties(chord-ui PROPERTIES ENVIRONMENT "NO_AT_BRIDGE=1;G_DEBUG=fatal-warnings")
        add_executable(rimes-panel-window-tests tests/panel_window_tests.cpp)
        target_link_libraries(rimes-panel-window-tests PRIVATE RimesDesktopUI)
        target_compile_options(rimes-panel-window-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME panel-window-x11 COMMAND "${RIMES_XVFB_RUN}" -a $<TARGET_FILE:rimes-panel-window-tests>)
        add_test(NAME panel-window-hidpi COMMAND "${RIMES_XVFB_RUN}" -a -s "-screen 0 2560x1600x24"
            "${CMAKE_COMMAND}" -E env GDK_SCALE=2 $<TARGET_FILE:rimes-panel-window-tests>)
        add_executable(rimes-translation-ui-tests tests/translation_ui_tests.cpp)
        target_link_libraries(rimes-translation-ui-tests PRIVATE RimesDesktopUI)
        add_dependencies(rimes-translation-ui-tests rimes-translation)
        add_test(NAME translation-ui COMMAND "${RIMES_XVFB_RUN}" -a $<TARGET_FILE:rimes-translation-ui-tests>)
        add_test(NAME settings-ui COMMAND "${RIMES_XVFB_RUN}" -a $<TARGET_FILE:rimes-settings> --smoke)
        add_test(NAME claude-settings-ui COMMAND "${RIMES_XVFB_RUN}" -a $<TARGET_FILE:rimes-settings> --claude-smoke "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_fixture.py")
        add_test(NAME codex-settings-ui COMMAND "${RIMES_XVFB_RUN}" -a $<TARGET_FILE:rimes-settings> --codex-smoke "${CMAKE_CURRENT_SOURCE_DIR}/tests/codex_fixture.py")
    endif()
endif()
