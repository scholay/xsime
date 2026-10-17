#pragma once
#include "mailbox_store.hpp"
#include <gtk/gtk.h>
#include <memory>
namespace rimes::mailbox {
class Workspace {
  public:
    explicit Workspace(GtkApplication* application = nullptr, bool fixture = false);
    ~Workspace();
    GtkWidget* Window() const;
    GtkWidget* Content() const;
    bool Open(const std::string& module = "", const std::string& thread = "");
    void Hide();
    void Refresh();
    void Protect(bool protected_session);
    bool Protected() const;
    void Fixture(Snapshot state, const std::string& theme, const std::string& module,
                 const std::string& thread);
    Json Inspect() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rimes::mailbox
