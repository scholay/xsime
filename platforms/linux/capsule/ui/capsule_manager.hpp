#pragma once
#include "capsule_protocol.hpp"
#include <functional>
#include <memory>
#include <gtk/gtk.h>
namespace rimes::capsule {
class ManagerWindow {
public:
    using Send = std::function<void(std::string)>;
    using CopyFile = std::function<void(const Record&)>;
    using CopyText = std::function<void(const std::string&)>;
    explicit ManagerWindow(Send send, bool preview = false, CopyFile file = {}, CopyText text = {}, std::function<void()> capture = {});
    ~ManagerWindow();
    void Apply(const Snapshot& snapshot);
    void ClipboardStatus(const std::string& status);
    GtkWidget* Window() const;
    GtkWidget* Root() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
