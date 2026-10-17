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
add_library(RimesRuntimeStorage STATIC core/private_files.cpp)
target_compile_features(RimesRuntimeStorage PUBLIC cxx_std_17)
target_include_directories(RimesRuntimeStorage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core")
target_link_libraries(RimesRuntimeStorage PUBLIC OpenSSL::Crypto)
set_target_properties(RimesRuntimeStorage PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(RimesRuntimeStorage PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(RimesProcess STATIC core/process.cpp core/background_jobs.cpp)
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
add_library(RimesPluginCore STATIC core/plugin_registry.cpp core/plugin_store.cpp core/package_binding.cpp core/official_catalog.cpp core/http_download.cpp core/configuration.cpp core/shortcuts.cpp)
target_compile_features(RimesPluginCore PUBLIC cxx_std_17)
target_include_directories(RimesPluginCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(RimesPluginCore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/generated")
# Reuse the repository's pinned, licensed header-only JSON dependency.
target_include_directories(RimesPluginCore SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../windows/native/third_party")
target_link_libraries(RimesPluginCore PUBLIC RimesRuntimeStorage PRIVATE PkgConfig::RuntimeCurl)
set_target_properties(RimesPluginCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(RimesPluginCore PRIVATE -Wall -Wextra -Wpedantic -Werror)

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
add_executable(rimes-settings ui/settings.cpp)
target_link_libraries(rimes-settings PRIVATE RimesDesktopUI)
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
    target_link_libraries(rimes-plugin-store-tests PRIVATE RimesPluginCore)
    add_test(NAME plugin-store COMMAND rimes-plugin-store-tests)
    add_executable(rimes-configuration-tests tests/configuration_tests.cpp)
    target_link_libraries(rimes-configuration-tests PRIVATE RimesPluginCore)
    add_test(NAME configuration COMMAND rimes-configuration-tests)
    find_program(RIMES_XVFB_RUN xvfb-run)
    if(RIMES_XVFB_RUN)
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
    endif()
endif()
