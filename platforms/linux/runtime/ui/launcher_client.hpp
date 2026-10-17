#pragma once
#include "plugin_store.hpp"
#include <functional>
#include <gio/gio.h>
namespace rimes::desktop {
// Completion always runs on the caller's GLib context. Callers own cancellation
// and must guard their UI lifetime before using a returned result.
void LauncherCall(const std::string& method, const std::string& action,
                  GCancellable* cancel, std::function<void(plugins::Json)> completion);
}
