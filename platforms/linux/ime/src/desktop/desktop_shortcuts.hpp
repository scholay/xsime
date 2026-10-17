#pragma once
#include <functional>
#include <memory>
#include "shortcuts.hpp"
#include "launcher_protocol.hpp"

namespace fcitx {
class EventLoop;
class DesktopShortcuts {
public:
    using Json = rimes::plugins::Json;
    using Open = std::function<Json(const std::string&)>;
    DesktopShortcuts(EventLoop& loop, Open open, std::function<Json()> last_result);
    ~DesktopShortcuts();
    void Configure(const Json& bindings, bool enabled);
    Json Status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using rimes::runtime::kLauncherName;
using rimes::runtime::kLauncherPath;
using rimes::runtime::kLauncherInterface;
} // namespace fcitx
