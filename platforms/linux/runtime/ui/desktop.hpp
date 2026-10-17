#pragma once
#include "configuration.hpp"
#include <functional>
#include <gtk/gtk.h>

namespace rimes::desktop {
using Json = runtime::Json;
const Json &Visuals();
void Theme(const std::string &name, GdkScreen *screen = nullptr);
GtkWidget *Class(GtkWidget *widget, const char *name);
GtkWidget *Label(const std::string &text, const char *style = nullptr);
GtkWidget *Button(const std::string &title, std::function<void()> clicked,
                  const char *style = nullptr);
GtkWidget *Icon(const std::string &name, int size = 20);
GtkWidget *IconButton(const std::string &icon, const std::string &label,
                      std::function<void()> clicked);
GtkWidget *CompactSwitch(const std::string &label, std::function<void()> clicked);
GtkWidget *ThemeCard(const std::string &theme, const std::string &title,
                     bool selected, std::function<void()> clicked);
GtkWidget *Column(int spacing = 8);
GtkWidget *Row(int spacing = 8);
void Add(GtkWidget *box, GtkWidget *child, bool expand = false);
void Clear(GtkWidget *container);
bool Confirm(GtkWindow *parent, const std::string &title,
             const std::string &detail);
bool Render(GtkWidget *widget, const std::string &path, int scale = 2);
class Form {
public:
  Form(std::vector<runtime::Field> fields, const Json &values);
  GtkWidget *Widget() const { return box_; }
  Json Values() const;
  std::string Secret(const std::string &id) const;
  void ClearSecrets();

private:
  std::vector<runtime::Field> fields_;
  std::map<std::string, GtkWidget *> widgets_;
  GtkWidget *box_;
};
} // namespace rimes::desktop
