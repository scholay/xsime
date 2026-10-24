#pragma once

#include <string>

#include "../engine/rime_engine.hpp"

namespace rimes::windows::broker {

struct BrokerOptions {
  bool deploy_only = false;
  bool serve_once = false;
  bool print_endpoint = false;
  bool print_paths = false;
  bool show_help = false;
  bool open_settings = false;
  bool install_autostart = false;
  bool remove_autostart = false;
  bool used_default_paths = false;
  unsigned long restart_parent = 0;
  engine::RimeEngineOptions engine;
};

// Serving input uses explicit --rime-dll/--shared-data-dir/--user-data-dir/
// --log-dir when present. Otherwise the parser fills the per-user defaults
// from ResolveDefaultBrokerPaths. --help, --print-endpoint, --print-paths,
// and the autostart commands may run without a live engine.
bool ParseBrokerOptions(int argc, wchar_t** argv, BrokerOptions* options,
                        std::wstring* error) noexcept;

}  // namespace rimes::windows::broker
