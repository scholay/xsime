#pragma once
#include "capsule_protocol.hpp"
#include <functional>
#include <gtk/gtk.h>
#include <memory>
namespace rimes::capsule {
// Secrets live only in this focused GTK view, never in host snapshots or the IME.
class PasswordView {
  public:
    using Send = std::function<void(std::string)>;
    PasswordView(GtkWindow* window, Send send, bool preview = false);
    ~PasswordView();
    GtkWidget* Widget() const;
    void Apply(const Snapshot& state);
    void New();
    void PrepareMigration(std::string id, std::string revision);
    void Conceal();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rimes::capsule
