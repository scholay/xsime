#pragma once
#include "capsule_rich_text.hpp"
#include <functional>
#include <gtk/gtk.h>
#include <memory>
namespace rimes::capsule {
class NoteEditor {
  public:
    NoteEditor(GtkWidget* view, std::function<void()> changed);
    ~NoteEditor();
    void Load(const RichDocument& document);
    void LoadPlain(const std::string& text);
    RichDocument Capture() const;
    void SetFormat(bool rich, bool bullet);
    void Bold();
    void Italic();
    void AddBullet();
    void Link(GtkWindow* parent);
    void Image(GtkWindow* parent);
    void Undo(bool redo = false);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rimes::capsule
