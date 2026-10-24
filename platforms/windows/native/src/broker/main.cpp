#include "../core/product_brand.hpp"
#include "rimes_version.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "../engine/rime_engine.hpp"
#include "../workbench/window.hpp"
#include "../workbench/local_translation.hpp"
#include "../workbench/local_gateway.hpp"
#include "../workbench/user_lexicon.hpp"
#include "autostart.hpp"
#include "../schemes/scheme_library.hpp"
#include "broker_connection.hpp"
#include "broker_options.hpp"
#include "default_paths.hpp"
#include "named_pipe_server.hpp"
#include "single_instance.hpp"
#include "ui_command.hpp"
#include "win32_security.hpp"
#include "restart.hpp"
#include "../workbench/codex_cli.hpp"

namespace rimes::windows::broker {
namespace {

void ReportBackgroundFailure(const BrokerOptions& options, const char* stage,
                             const std::wstring& detail) noexcept {
  if (options.serve_once || options.deploy_only || options.print_endpoint ||
      options.print_paths || options.install_autostart || options.remove_autostart)
    return;
  try {
    // Startup can fail before librime opens its own log. Keep a stage-only
    // record; no input, model request, credentials or untrusted error text.
    std::error_code ignored;
    std::filesystem::create_directories(options.engine.log_dir, ignored);
    std::ofstream(options.engine.log_dir / L"broker-startup.log", std::ios::app)
        << "RIMES " << kProductVersion << ": " << stage << '\n';
  } catch (...) {}
  DWORD session = 0;
  if (options.open_settings &&
      ProcessIdToSessionId(GetCurrentProcessId(), &session) && session != 0)
    MessageBoxW(nullptr, detail.c_str(), rimes::branding::Text(L"{brand} 设置").c_str(), MB_OK | MB_ICONERROR);
}

void PrintUsage() {
  std::wcout
      // The installer probes this output through a pipe in the default C
      // locale. A localized non-ASCII prefix can fail wcout before --settings.
      << L"XSIME Windows Broker " << kProductVersionWide << L"\n\n"
      << L"Usage:\n"
      << L"  RimesBroker --print-endpoint\n"
      << L"  RimesBroker --print-paths\n"
      << L"  RimesBroker --deploy-only\n"
      << L"  RimesBroker --settings\n"
      << L"  RimesBroker --schemes\n"
      << L"  RimesBroker --install-autostart | --remove-autostart\n"
      << L"  RimesBroker [--once] [--rime-dll <absolute-path>]\n"
      << L"      [--shared-data-dir <absolute-path>]\n"
      << L"      [--user-data-dir <absolute-path>]\n"
      << L"      [--log-dir <absolute-path>] [--full-maintenance-check]\n\n"
      << L"  --once                  Serve one verified client, then exit.\n"
      << L"  --settings              Open settings in the current desktop Broker.\n"
      << L"  --print-endpoint        Print this user's pipe name, then exit.\n"
      << L"  --print-paths           Print resolved engine paths, then exit.\n"
      << L"  --install-autostart     Register a current-user logon Run key.\n"
      << L"  --remove-autostart      Remove the current-user logon Run key.\n"
      << L"  --full-maintenance-check  Ask librime for a full maintenance "
         L"pass.\n"
      << L"  Missing engine paths default to %%LOCALAPPDATA%%\\RIMES and\n"
      << L"  %%APPDATA%%\\RIMES, or files next to this executable.\n";
}

}  // namespace
}  // namespace rimes::windows::broker

int wmain(const int argc, wchar_t** argv) {
  using namespace rimes::windows;
  using namespace rimes::windows::broker;

  if (argc >= 2 && (std::wstring(argv[1]) == L"--schemes" || std::wstring(argv[1]) == L"--scheme-deploy" || std::wstring(argv[1]) == L"--scheme-import-smoke"))
    return schemes::RunManager(argc, argv);

  BrokerOptions options;
  std::wstring error;
  if (!ParseBrokerOptions(argc, argv, &options, &error)) {
    std::wcerr << L"Invalid broker options: " << error << L'\n';
    PrintUsage();
    return 2;
  }
  if (options.show_help) {
    PrintUsage();
    return 0;
  }
  if (!WaitForRestartParent(options.restart_parent)) {
    std::wcerr << L"The previous Broker could not be verified or did not exit.\n";
    return 2;
  }
  // Ordinary desktop serving is a background application. A console opened
  // by double-click/logon must not remain as a black window. Keep the console
  // for explicit diagnostics, deployment and --once integration runs so their
  // existing stdout/exit-code contracts remain intact.
  if (!options.serve_once && !options.deploy_only && !options.print_endpoint &&
      !options.print_paths && !options.install_autostart &&
      !options.remove_autostart)
    FreeConsole();

  UserSecurityContext security;
  if (options.print_endpoint) {
    if (!security.Initialize(&error)) {
      std::wcerr << error << L'\n';
      return 3;
    }
    std::wcout << security.pipe_name() << L'\n';
    return 0;
  }
  if (options.remove_autostart) {
    if (!RemoveBrokerAutostart(&error)) {
      std::wcerr << L"Failed to remove broker autostart: " << error << L'\n';
      return 7;
    }
    std::wcout << rimes::branding::Text(L"Removed the current-user {brand} broker autostart entry.\n").c_str();
    return 0;
  }
  if (options.install_autostart) {
    DefaultBrokerPaths defaults;
    if (!ResolveDefaultBrokerPaths(&defaults, &error)) {
      std::wcerr << L"Failed to resolve the broker path: " << error << L'\n';
      return 7;
    }
    if (!InstallBrokerAutostart(defaults.broker_exe.wstring(), &error)) {
      std::wcerr << L"Failed to install broker autostart: " << error << L'\n';
      return 7;
    }
    std::wcout << L"Installed current-user autostart for "
               << defaults.broker_exe.wstring() << L'\n';
    return 0;
  }
  if (options.print_paths) {
    std::wcout << L"rime-dll=" << options.engine.dll_path.wstring() << L'\n'
               << L"shared-data-dir="
               << options.engine.shared_data_dir.wstring() << L'\n'
               << L"user-data-dir=" << options.engine.user_data_dir.wstring()
               << L'\n' << L"log-dir=" << options.engine.log_dir.wstring()
               << L'\n' << L"used-defaults="
               << (options.used_default_paths ? L"yes" : L"no") << L'\n';
    return 0;
  }

  DefaultBrokerPaths created_dirs;
  created_dirs.shared_data_dir = options.engine.shared_data_dir;
  created_dirs.user_data_dir = options.engine.user_data_dir;
  created_dirs.log_dir = options.engine.log_dir;
  if (!EnsureBrokerDataDirectories(created_dirs, &error)) {
    std::wcerr << L"Failed to create broker data directories: " << error
               << L'\n';
    ReportBackgroundFailure(options, "data directories unavailable", error);
    return 5;
  }

  if (options.deploy_only) {
    options.engine.full_maintenance_check = true;
    options.engine.verify_input_session = false;
    engine::RimeEngine deploy;
    std::string failure;
    if (!deploy.Start(options.engine, &failure)) {
      std::cerr << "Deployment failed: " << failure << '\n';
      return 5;
    }
    deploy.Stop();
    std::cout << "Dictionary deployment finished. User data retained.\n";
    return 0;
  }
  if (!security.Initialize(&error)) {
    std::wcerr << error << L'\n';
    ReportBackgroundFailure(options, "user security initialization failed", error);
    return 3;
  }
  if (options.open_settings && security.session_id() == 0) {
    std::wcerr << L"Settings require an interactive Windows session.\n";
    return 8;
  }
  SingleInstance instance;
  if (!instance.Acquire(security.mutex_name(), security.attributes(), &error)) {
    std::wcerr << L"Failed to acquire the broker mutex: " << error << L'\n';
    ReportBackgroundFailure(options, "single instance initialization failed", error);
    return 4;
  }
  if (instance.already_running()) {
    if (options.open_settings) {
      if (RequestSettings(security, &error)) return 0;
      std::wcerr << error << L'\n';
      ReportBackgroundFailure(options, "settings forwarding failed", error);
      return 8;
    }
    std::wcerr << rimes::branding::Text(L"The per-user {brand} broker is already running.\n").c_str();
    return 0;
  }

  std::optional<rimes::schemes::Selection> imported;
  if (options.used_default_paths) {
    imported = schemes::Library().Active();
    if (imported) {
      options.engine.shared_data_dir = imported->resources;
      options.engine.user_data_dir = imported->user;
    }
  }
  std::unique_ptr<schemes::UsageLock> imported_usage;
  if (imported) {
    try { imported_usage = std::make_unique<schemes::UsageLock>(imported->user, false); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
  }
  engine::RimeEngine engine;
  std::string engine_error;
  if (!engine.Start(options.engine, &engine_error)) {
    std::cerr << "Failed to start the RIME engine: " << engine_error << '\n';
    ReportBackgroundFailure(options, "engine startup failed",
                            L"输入引擎无法启动：\n" + workbench::Wide(engine_error));
    return 5;
  }

  NamedPipeServer server(&security);
  NamedPipeServer input_server(&security, true);
  workbench::Runtime runtime(workbench::GenerateText,
      {workbench::LocalTranslationReady, workbench::PrepareLocalTranslation});
  runtime.input_option_provider = [&engine](const std::string& schema) { return engine.InputOptions(schema); };
  runtime.log_directory = options.engine.log_dir;
  runtime.user_data_directory = options.engine.user_data_dir;
  runtime.user_lexicon = std::make_shared<workbench::UserLexiconService>(
      engine, options.engine.user_data_dir / L".lexicon-transfer");
  if (imported) runtime.SetImportedSchema(imported->schema, imported->package.id);
  workbench::UiCommands commands;
  runtime.restart_broker = [&options, &commands](std::string* failure) {
    wchar_t exe[32768]{};
    if (!GetModuleFileNameW(nullptr, exe, 32768)) return false;
    auto command = workbench::QuoteWindowsArgument(exe) + L" --settings --restart-parent " + std::to_wstring(GetCurrentProcessId());
    for (const auto& item : std::initializer_list<std::pair<const wchar_t*, std::filesystem::path>>{
        {L"--rime-dll", options.engine.dll_path}, {L"--shared-data-dir", options.engine.shared_data_dir},
        {L"--user-data-dir", options.engine.user_data_dir}, {L"--log-dir", options.engine.log_dir}})
      command += L" " + std::wstring(item.first) + L" " + workbench::QuoteWindowsArgument(item.second.wstring());
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
      if (failure) *failure = "无法启动新的后台进程，当前进程继续运行。";
      return false;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return commands.RequestExit();
  };
  const bool interactive = security.session_id() != 0;
#ifndef RIMES_E2E_TEST_IDENTITY
  std::unique_ptr<workbench::LocalGateway> gateway;
  if (interactive && !options.serve_once)
    gateway = std::make_unique<workbench::LocalGateway>([&] {
      return !runtime.Stopping() && runtime.Configuration().gateway_enabled;
    });
#endif
  struct StopEvent {
    HANDLE handle = nullptr;
    ~StopEvent() { if (handle) CloseHandle(handle); }
  } maintenance;
  const auto stop_name = security.mutex_name() + L".shutdown-" +
                         std::to_wstring(GetCurrentProcessId());
  maintenance.handle = CreateEventW(security.attributes(), TRUE, FALSE,
                                    stop_name.c_str());
  if (!maintenance.handle) return 4;
  std::jthread ui;
  if (interactive && !options.serve_once)
    ui = std::jthread([&] {
      workbench::RunWindow(
          runtime, [&] { server.RequestStop(); input_server.RequestStop(); },
          [&] { engine.RunMaintenance(true, nullptr); }, &commands,
          options.open_settings);
    });
  std::jthread maintenance_stop([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      if (WaitForSingleObject(maintenance.handle, 100) != WAIT_OBJECT_0)
        continue;
      if (interactive && !options.serve_once) {
        // UI attachment may still be starting. Keep the request pending until
        // it can run on that thread; never read or save Buffer text here.
        if (!commands.RequestExit()) { Sleep(25); continue; }
      } else {
        server.RequestStop();
        input_server.RequestStop();
      }
      break;
    }
  });
  error.clear();
  // A deliberately smaller authority for sandboxed hosts: ordinary engine
  // sessions only. No workbench target, clipboard, AI, settings or delivery.
  std::jthread input_worker;
  if (!options.serve_once) input_worker = std::jthread([&] {
    std::wstring input_error;
    const auto input_result = input_server.ServeClients(
        [&](DWORD) {
          auto connection = std::make_shared<BrokerConnection>(
              security.session_id(), &engine, interactive ? &runtime : nullptr,
              std::function<bool()>{}, true);
          return [connection](const core::Frame& request, DWORD peer,
                              core::Frame* response) {
            return connection->Handle(request, peer, response);
          };
        }, false, &input_error);
    if (input_result == ServeResult::kFatalError)
      std::wcerr << L"Input-only endpoint unavailable: " << input_error << L'\n';
  });
  const ServeResult result = server.ServeClients(
      [&security, &engine, &runtime, &commands, interactive, &options](DWORD) {
        auto connection = std::make_shared<BrokerConnection>(
            security.session_id(), &engine, interactive ? &runtime : nullptr,
            interactive && !options.serve_once ? std::function<bool()>([&commands] {
              return commands.RequestSettings();
            }) : std::function<bool()>{});
        return
            [connection](const core::Frame& request,
                         const DWORD client_process_id, core::Frame* response) {
              return connection->Handle(request, client_process_id, response);
            };
      },
      options.serve_once, &error);
  input_server.RequestStop();
  if (input_worker.joinable()) input_worker.join();
  runtime.Stop();
  if (ui.joinable()) ui.join();
  if (result == ServeResult::kFatalError) {
    std::wcerr << L"Broker pipe failure: " << error << L'\n';
    ReportBackgroundFailure(options, "named pipe server failed", error);
    return 6;
  }
  if (result == ServeResult::kClientRejected && !error.empty()) {
    std::wcerr << L"Rejected broker client: " << error << L'\n';
  }
  return 0;
}
