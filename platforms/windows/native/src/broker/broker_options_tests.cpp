#include "broker_options.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace rimes::windows::broker::tests {
namespace {

int g_failures = 0;

#if defined(_WIN32)
inline constexpr wchar_t kDllPath[] =
    L"C:\\Program Files\\RIMES\\rime.dll";
inline constexpr wchar_t kSharedPath[] =
    L"C:\\Program Files\\RIMES\\data";
inline constexpr wchar_t kUserPath[] =
    L"C:\\Users\\tester\\AppData\\Roaming\\RIMES";
inline constexpr wchar_t kLogPath[] =
    L"C:\\Users\\tester\\AppData\\Local\\RIMES\\Logs";
inline constexpr wchar_t kSecondDllPath[] = L"C:\\b\\rime.dll";
#else
inline constexpr wchar_t kDllPath[] = L"/opt/rimes/rime.dll";
inline constexpr wchar_t kSharedPath[] = L"/opt/rimes/data";
inline constexpr wchar_t kUserPath[] = L"/tmp/rimes/user";
inline constexpr wchar_t kLogPath[] = L"/tmp/rimes/logs";
inline constexpr wchar_t kSecondDllPath[] = L"/opt/rimes2/rime.dll";
#endif

#define EXPECT(condition)                                                     \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::cerr << __FILE__ << ':' << __LINE__ << ": expectation failed: "   \
                << #condition << '\n';                                        \
      ++g_failures;                                                           \
    }                                                                         \
  } while (false)

struct Arguments {
  explicit Arguments(std::initializer_list<const wchar_t*> values) {
    for (const wchar_t* value : values) {
      storage.emplace_back(value);
    }
    for (std::wstring& value : storage) {
      argv.push_back(value.data());
    }
  }

  int argc() const { return static_cast<int>(argv.size()); }

  std::vector<std::wstring> storage;
  std::vector<wchar_t*> argv;
};

bool Parse(Arguments* arguments,
           BrokerOptions* options,
           std::wstring* error) {
  return ParseBrokerOptions(arguments->argc(), arguments->argv.data(), options,
                            error);
}

void TestMissingEngineConfigurationUsesDefaults() {
  Arguments arguments{L"RimesBroker.exe"};
  BrokerOptions options;
  std::wstring error;
#if defined(_WIN32)
  EXPECT(Parse(&arguments, &options, &error));
  EXPECT(options.used_default_paths);
  EXPECT(options.engine.dll_path.is_absolute());
  EXPECT(options.engine.shared_data_dir.is_absolute());
  EXPECT(options.engine.user_data_dir.is_absolute());
  EXPECT(options.engine.log_dir.is_absolute());
#else
  EXPECT(!Parse(&arguments, &options, &error));
  EXPECT(!error.empty());
#endif
}

void TestDiagnosticsNeedNoEngine() {
  {
    Arguments arguments{L"RimesBroker.exe", L"--help"};
    BrokerOptions options;
    std::wstring error;
    EXPECT(Parse(&arguments, &options, &error));
    EXPECT(options.show_help);
  }
  {
    Arguments arguments{L"RimesBroker.exe", L"--print-endpoint"};
    BrokerOptions options;
    std::wstring error;
    EXPECT(Parse(&arguments, &options, &error));
    EXPECT(options.print_endpoint);
  }
  {
    Arguments arguments{L"RimesBroker.exe", L"--remove-autostart"};
    BrokerOptions options;
    std::wstring error;
    EXPECT(Parse(&arguments, &options, &error));
    EXPECT(options.remove_autostart);
  }
  {
    Arguments arguments{L"RimesBroker.exe", L"--print-paths"};
    BrokerOptions options;
    std::wstring error;
#if defined(_WIN32)
    EXPECT(Parse(&arguments, &options, &error));
    EXPECT(options.print_paths);
    EXPECT(options.used_default_paths);
#else
    EXPECT(!Parse(&arguments, &options, &error));
#endif
  }
}

void TestExplicitEngineConfiguration() {
  Arguments arguments{
      L"RimesBroker.exe",
      L"--once",
      L"--full-maintenance-check",
      L"--rime-dll",
      kDllPath,
      L"--shared-data-dir",
      kSharedPath,
      L"--user-data-dir",
      kUserPath,
      L"--log-dir",
      kLogPath,
  };
  BrokerOptions options;
  std::wstring error;
  EXPECT(Parse(&arguments, &options, &error));
  EXPECT(options.serve_once);
  EXPECT(options.engine.full_maintenance_check);
  EXPECT(options.engine.dll_path.filename() == L"rime.dll");
  EXPECT(options.engine.shared_data_dir.is_absolute());
  EXPECT(options.engine.user_data_dir.is_absolute());
  EXPECT(options.engine.log_dir.is_absolute());
}

void TestAmbiguousOrRelativeConfigurationFails() {
  {
    Arguments arguments{L"RimesBroker.exe", L"--print-endpoint", L"--once"};
    BrokerOptions options;
    std::wstring error;
    EXPECT(!Parse(&arguments, &options, &error));
  }
  {
    Arguments arguments{
        L"RimesBroker.exe", L"--rime-dll", L"rime.dll",
        L"--shared-data-dir", L"data", L"--user-data-dir", L"user",
        L"--log-dir", L"logs"};
    BrokerOptions options;
    std::wstring error;
    EXPECT(!Parse(&arguments, &options, &error));
  }
  {
    Arguments arguments{L"RimesBroker.exe", L"--rime-dll", kDllPath,
                        L"--rime-dll", kSecondDllPath};
    BrokerOptions options;
    std::wstring error;
    EXPECT(!Parse(&arguments, &options, &error));
  }
}

void TestSettingsLaunchAndConflictingCommands() {
  Arguments launch{L"RimesBroker.exe", L"--settings", L"--rime-dll", kDllPath,
                   L"--shared-data-dir", kSharedPath, L"--user-data-dir",
                   kUserPath, L"--log-dir", kLogPath};
  BrokerOptions options;
  std::wstring error;
  EXPECT(Parse(&launch, &options, &error));
  EXPECT(options.open_settings && !options.serve_once && !options.deploy_only);
  for (const auto* conflicting : {L"--settings", L"--once", L"--deploy-only",
                                 L"--print-endpoint", L"--print-paths",
                                 L"--install-autostart", L"--remove-autostart",
                                 L"--help"}) {
    Arguments arguments{L"RimesBroker.exe", L"--settings", conflicting};
    EXPECT(!Parse(&arguments, &options, &error));
    EXPECT(!error.empty());
  }
#if defined(_WIN32)
  Arguments defaults{L"RimesBroker.exe", L"--settings"};
  EXPECT(Parse(&defaults, &options, &error));
  EXPECT(options.open_settings && options.used_default_paths);
#endif
}

void TestRestartParentArguments() {
  BrokerOptions options; std::wstring error;
  const auto launch = [](const wchar_t* pid) {
    return Arguments{L"RimesBroker.exe", L"--settings", L"--restart-parent", pid,
        L"--rime-dll", kDllPath, L"--shared-data-dir", kSharedPath,
        L"--user-data-dir", kUserPath, L"--log-dir", kLogPath};
  };
  auto valid = launch(L"1234");
  EXPECT(Parse(&valid, &options, &error) && options.restart_parent == 1234 && options.open_settings);
  for (const auto* pid : {L"0", L"-1", L"+1", L" 1", L"1x", L"4294967296", L"99999999999", L""}) {
    auto invalid = launch(pid);
    EXPECT(!Parse(&invalid, &options, &error));
  }
  Arguments duplicate{L"RimesBroker.exe", L"--settings", L"--restart-parent", L"1234", L"--restart-parent", L"4321"};
  EXPECT(!Parse(&duplicate, &options, &error));
  Arguments missing{L"RimesBroker.exe", L"--settings", L"--restart-parent"};
  EXPECT(!Parse(&missing, &options, &error));
  for (const auto* command : {L"--print-endpoint", L"--print-paths", L"--once", L"--deploy-only", L"--help"}) {
    Arguments invalid{L"RimesBroker.exe", command, L"--restart-parent", L"1234"};
    EXPECT(!Parse(&invalid, &options, &error));
  }
}

}  // namespace

int RunBrokerOptionsTests() {
  TestMissingEngineConfigurationUsesDefaults();
  TestDiagnosticsNeedNoEngine();
  TestExplicitEngineConfiguration();
  TestAmbiguousOrRelativeConfigurationFails();
  TestSettingsLaunchAndConflictingCommands();
  TestRestartParentArguments();
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace rimes::windows::broker::tests

#if defined(_WIN32)
int wmain() {
#else
int main() {
#endif
  return rimes::windows::broker::tests::RunBrokerOptionsTests();
}
