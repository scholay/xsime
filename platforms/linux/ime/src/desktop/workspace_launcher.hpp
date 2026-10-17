#pragma once
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>
#include "shortcuts.hpp"

namespace fcitx {
class BufferService;
class CapsuleService;
class WorkspaceLauncher {
public:
    using Json = rimes::plugins::Json;
    WorkspaceLauncher(Instance& instance, BufferService& buffer, CapsuleService& capsule,
                      std::function<bool()> open_settings);
    ~WorkspaceLauncher();
    void Configure(const Json& shortcuts, bool global);
    bool HandleKey(KeyEvent& event);
    Json Open(const std::string& action);
    Json LastResult() const { return result_; }
    bool GlobalEnabled() const { return global_; }
    void Revoke() { Invalidate(); }
    void SetDesktopStatus(std::function<Json()> status) { desktop_status_ = std::move(status); }
private:
    void Invalidate();
    void Show(const std::string& action, InputContext* ic);
    Instance& instance_;
    BufferService& buffer_;
    CapsuleService& capsule_;
    std::function<bool()> open_settings_;
    std::function<Json()> desktop_status_;
    std::map<std::string, Key> bindings_;
    std::set<KeySym> down_;
    std::vector<std::unique_ptr<HandlerTableEntry<EventHandler>>> watchers_;
    std::unique_ptr<EventSourceTime> pending_;
    uint64_t epoch_ = 0, request_id_ = 0;
    bool global_ = false;
    Json result_ = {{"ok", true}, {"status", "idle"}};
};
} // namespace fcitx
