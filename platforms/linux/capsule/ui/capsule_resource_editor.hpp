#pragma once
#include "capsule_resources.hpp"
#include <functional>
#include <gtk/gtk.h>
#include <memory>
namespace rimes::capsule {
class ResourceEditor {
  public:
    ResourceEditor(GtkWindow* owner, std::function<void()> changed,
                   std::function<void(const Record&)> copy_file,
                   std::function<void(const std::string&)> copy_text, bool fixture);
    ~ResourceEditor();
    GtkWidget* Widget() const;
    Record Capture() const;
    void Load(const Record& record);
    void Conceal();
    void SetEditable(bool enabled);
    bool ImportRequested() const;
    void SetImportRequested(bool value);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rimes::capsule
