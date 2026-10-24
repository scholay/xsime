#include "broker_options.hpp"

#include <filesystem>
#include <string_view>

#include "default_paths.hpp"

namespace rimes::windows::broker {
namespace {

void SetError(std::wstring* error, std::wstring_view message) noexcept {
  if (error == nullptr) {
    return;
  }
  try {
    error->assign(message);
  } catch (...) {
  }
}

bool ReadUniquePath(int argc, wchar_t** argv, int* index, bool* seen,
                    std::filesystem::path* output, std::wstring_view option,
                    std::wstring* error) {
  if (*seen) {
    SetError(error, std::wstring(L"duplicate option: ") + std::wstring(option));
    return false;
  }
  if (*index + 1 >= argc || argv[*index + 1] == nullptr ||
      argv[*index + 1][0] == L'\0') {
    SetError(error, std::wstring(L"missing value for ") + std::wstring(option));
    return false;
  }
  const std::wstring_view value(argv[*index + 1]);
  if (value.starts_with(L"--")) {
    SetError(error, std::wstring(L"missing value for ") + std::wstring(option));
    return false;
  }
  *output = std::filesystem::path(value);
  *seen = true;
  ++*index;
  return true;
}

bool RequireAbsolute(const std::filesystem::path& path,
                     std::wstring_view option, std::wstring* error) {
  if (!path.empty() && path.is_absolute()) {
    return true;
  }
  SetError(error, std::wstring(option) + L" requires an absolute local path");
  return false;
}

}  // namespace

bool ParseBrokerOptions(int argc, wchar_t** argv, BrokerOptions* options,
                        std::wstring* error) noexcept {
  if (options == nullptr || argc < 1 || argv == nullptr) {
    SetError(error, L"broker option output or process arguments are invalid");
    return false;
  }

  try {
    *options = BrokerOptions{};
    BrokerOptions parsed;
    bool saw_once = false;
    bool saw_endpoint = false;
    bool saw_paths = false;
    bool saw_help = false;
    bool saw_install_autostart = false;
    bool saw_remove_autostart = false;
    bool saw_full_maintenance = false;
    bool saw_dll = false;
    bool saw_shared = false;
    bool saw_user = false;
    bool saw_log = false;

    for (int index = 1; index < argc; ++index) {
      if (argv[index] == nullptr) {
        SetError(error, L"broker argument is null");
        return false;
      }
      const std::wstring_view argument(argv[index]);
      if (argument == L"--once") {
        if (saw_once) {
          SetError(error, L"duplicate option: --once");
          return false;
        }
        saw_once = true;
        parsed.serve_once = true;
      } else if (argument == L"--deploy-only") {
        if (parsed.deploy_only) {
          SetError(error, L"duplicate --deploy-only");
          return false;
        }
        parsed.deploy_only = true;
      } else if (argument == L"--print-endpoint") {
        if (saw_endpoint) {
          SetError(error, L"duplicate option: --print-endpoint");
          return false;
        }
        saw_endpoint = true;
        parsed.print_endpoint = true;
      } else if (argument == L"--print-paths") {
        if (saw_paths) {
          SetError(error, L"duplicate option: --print-paths");
          return false;
        }
        saw_paths = true;
        parsed.print_paths = true;
      } else if (argument == L"--restart-parent") {
        if (parsed.restart_parent || index + 1 >= argc || !argv[index + 1]) return false;
        const std::wstring_view value(argv[++index]);
        if (value.empty() || value.size() > 10 || value.find_first_not_of(L"0123456789") != value.npos) return false;
        const auto pid = std::stoull(std::wstring(value));
        if (!pid || pid > 0xffffffffULL) return false;
        parsed.restart_parent = static_cast<unsigned long>(pid);
      } else if (argument == L"--settings") {
        if (parsed.open_settings) {
          SetError(error, L"duplicate option: --settings");
          return false;
        }
        parsed.open_settings = true;
      } else if (argument == L"--install-autostart") {
        if (saw_install_autostart) {
          SetError(error, L"duplicate option: --install-autostart");
          return false;
        }
        saw_install_autostart = true;
        parsed.install_autostart = true;
      } else if (argument == L"--remove-autostart") {
        if (saw_remove_autostart) {
          SetError(error, L"duplicate option: --remove-autostart");
          return false;
        }
        saw_remove_autostart = true;
        parsed.remove_autostart = true;
      } else if (argument == L"--help" || argument == L"-h" ||
                 argument == L"/?") {
        if (saw_help) {
          SetError(error, L"duplicate help option");
          return false;
        }
        saw_help = true;
        parsed.show_help = true;
      } else if (argument == L"--full-maintenance-check") {
        if (saw_full_maintenance) {
          SetError(error, L"duplicate option: --full-maintenance-check");
          return false;
        }
        saw_full_maintenance = true;
        parsed.engine.full_maintenance_check = true;
      } else if (argument == L"--rime-dll") {
        if (!ReadUniquePath(argc, argv, &index, &saw_dll,
                            &parsed.engine.dll_path, argument, error)) {
          return false;
        }
      } else if (argument == L"--shared-data-dir") {
        if (!ReadUniquePath(argc, argv, &index, &saw_shared,
                            &parsed.engine.shared_data_dir, argument, error)) {
          return false;
        }
      } else if (argument == L"--user-data-dir") {
        if (!ReadUniquePath(argc, argv, &index, &saw_user,
                            &parsed.engine.user_data_dir, argument, error)) {
          return false;
        }
      } else if (argument == L"--log-dir") {
        if (!ReadUniquePath(argc, argv, &index, &saw_log,
                            &parsed.engine.log_dir, argument, error)) {
          return false;
        }
      } else {
        SetError(error,
                 std::wstring(L"unknown option: ") + std::wstring(argument));
        return false;
      }
    }

    if (parsed.restart_parent && !parsed.open_settings) {
      SetError(error, L"--restart-parent requires --settings");
      return false;
    }
    if (parsed.deploy_only && (parsed.serve_once || parsed.print_paths)) {
      SetError(
          error,
          L"--deploy-only must not be combined with serving or inspection");
      return false;
    }
    if (parsed.show_help) {
      if (argc != 2) {
        SetError(error, L"the help option must be used by itself");
        return false;
      }
      *options = std::move(parsed);
      return true;
    }

    const bool has_any_engine_option = saw_dll || saw_shared || saw_user ||
                                       saw_log || saw_full_maintenance ||
                                       parsed.deploy_only;
    if (parsed.open_settings &&
        (parsed.serve_once || parsed.print_endpoint || parsed.print_paths ||
         parsed.install_autostart || parsed.remove_autostart ||
         parsed.deploy_only)) {
      SetError(error, L"--settings cannot be combined with headless or diagnostic commands");
      return false;
    }
    if (parsed.print_endpoint) {
      if (parsed.serve_once || parsed.print_paths || parsed.install_autostart ||
          parsed.remove_autostart || has_any_engine_option) {
        SetError(error,
                 L"--print-endpoint cannot be combined with serving or engine "
                 L"options");
        return false;
      }
      *options = std::move(parsed);
      return true;
    }
    if (parsed.install_autostart && parsed.remove_autostart) {
      SetError(
          error,
          L"--install-autostart and --remove-autostart cannot be combined");
      return false;
    }
    if ((parsed.install_autostart || parsed.remove_autostart) &&
        (parsed.serve_once || parsed.print_paths || has_any_engine_option)) {
      SetError(error,
               L"autostart commands cannot be combined with serving or engine "
               L"options");
      return false;
    }
    if (parsed.remove_autostart || parsed.install_autostart) {
      *options = std::move(parsed);
      return true;
    }

    if (!(saw_dll && saw_shared && saw_user && saw_log)) {
      DefaultBrokerPaths defaults;
      if (!ResolveDefaultBrokerPaths(&defaults, error)) {
        return false;
      }
      if (!saw_dll) {
        parsed.engine.dll_path = defaults.dll_path;
      }
      if (!saw_shared) {
        parsed.engine.shared_data_dir = defaults.shared_data_dir;
      }
      if (!saw_user) {
        parsed.engine.user_data_dir = defaults.user_data_dir;
      }
      if (!saw_log) {
        parsed.engine.log_dir = defaults.log_dir;
      }
      parsed.used_default_paths =
          !saw_dll || !saw_shared || !saw_user || !saw_log;
    }
    if (!RequireAbsolute(parsed.engine.dll_path, L"--rime-dll", error) ||
        !RequireAbsolute(parsed.engine.shared_data_dir, L"--shared-data-dir",
                         error) ||
        !RequireAbsolute(parsed.engine.user_data_dir, L"--user-data-dir",
                         error) ||
        !RequireAbsolute(parsed.engine.log_dir, L"--log-dir", error)) {
      return false;
    }

    *options = std::move(parsed);
    return true;
  } catch (...) {
    SetError(error, L"exception while parsing broker options");
    return false;
  }
}

}  // namespace rimes::windows::broker
