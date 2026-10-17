#include "desktop.hpp"
#include "native_visual_tokens.hpp"
#include "native_buffer_metrics.hpp"
#include <cmath>
#include <memory>

namespace rimes::desktop {
const Json &Visuals() {
  static const auto data = Json::parse(kNativeVisuals);
  return data;
}
GtkWidget *Class(GtkWidget *widget, const char *name) {
  gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
  return widget;
}
void Theme(const std::string &name, GdkScreen *screen) {
  if (!screen) screen = gdk_screen_get_default();
  const auto theme = name == "liquidGlass" ? "day" : name;
  const auto &colors = Visuals().at("palettes").at(theme);
  auto c = [&](const char *key) { return colors.at(key).get<std::string>(); };
  g_object_set(gtk_settings_get_for_screen(screen), "gtk-application-prefer-dark-theme",
               theme != "day", nullptr);
  static GtkCssProvider *provider = nullptr;
  if (!provider) {
    provider = gtk_css_provider_new();
  }
  // A passive panel may explicitly use XWayland on a Wayland session. Its
  // screen is not necessarily the process default screen.
  gtk_style_context_remove_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider));
  gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider),
                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  std::string css =
      "@define-color xsime_switch_on " + c("accentGreen") + ";"
      "@define-color xsime_switch_off " + c("surfaceTertiary") + ";"
      "@define-color xsime_switch_border " + c("border") + ";"
      "@define-color xsime_switch_accent " + c("accentText") + ";"
      "button.compact-switch, button.compact-switch:hover, button.compact-switch:active, button.compact-switch:checked, button.compact-switch:disabled { background: transparent; border: 0; padding: 0; min-width: 0; min-height: 0; box-shadow: none; outline: 0; opacity: 1; }"
      "* { font-family: 'SF Pro Text', Inter, 'Noto Sans CJK SC', sans-serif; "
      "font-size: 11px; -gtk-icon-shadow: none; text-shadow: none; }"
      "window, .settings { background: " +
      c("settingsBackground") + "; color: " + c("textPrimary") +
      "; }"
      ".settings viewport { background: transparent; } drawingarea { color: " +
      c("textSecondary") +
      "; }"
      "label { color: " +
      c("textPrimary") +
      "; }"
      "button { background-image: none; background-color: #626366; color: "
      "white; border: 0; border-radius: 5px; box-shadow: none; padding: 2px "
      "8px; min-height: 14px; min-width: 0; }"
      "button:hover { background-color: #727376; } button:active { "
      "background-color: #515255; } button label { color: inherit; }"
      "button:disabled, entry:disabled { opacity: 0.45; }"
      "button:focus { outline: 2px solid " +
      c("accentGreen") +
      "; outline-offset: 1px; }"
      "entry, combobox button, spinbutton { background: " +
      c("surfaceTertiary") + "; color: " + c("textPrimary") +
      "; border: 1px solid " + c("borderStrong") +
      "; border-radius: 5px; min-height: 20px; padding: 1px 5px; box-shadow: "
      "none; }"
      "entry:focus { border-color: " +
      c("accentGreen") +
      "; }"
      ".sidebar { background: " +
      c("settingsBackground") + "; border-right: 1px solid " +
      c("settingsSeparator") +
      "; }"
      ".navigation { padding: 0; min-height: 32px; background: transparent; "
      "border-radius: 7px; }"
      ".navigation label { font-size: 12px; font-weight: 500; color: " +
      c("textSecondary") +
      "; } .navigation .glyph, .icon-button .glyph, .glyph { color: " +
      c("textSecondary") +
      "; }"
      ".navigation.selected { background: mix(" +
      c("settingsBackground") + ", " + c("accentGreen") +
      ", 0.16); }"
      ".navigation.selected label, .navigation.selected .glyph { color: " +
      c("textPrimary") +
      "; }"
      ".navigation:hover:not(.selected) { background: alpha(" +
      c("accentGreen") +
      ", 0.08); }"
      ".tabs { border-bottom: 1px solid " +
      c("settingsSeparator") +
      "; }"
      ".segment { padding: 0 5px; min-height: 16px; border-radius: 0; "
      "font-weight: 600; background: #686868; }"
      ".segment:first-child { border-radius: 4px 0 0 4px; } "
      ".segment:last-child { border-radius: 0 4px 4px 0; }"
      ".segment.selected { color: #101010; background: #A6A6A6; }"
      ".heading { font-size: 20px; font-weight: bold; } .section { font-size: "
      "12px; font-weight: 600; color: " +
      c("textSecondary") +
      "; }"
      ".sidebar-heading { font-size: 10px; font-weight: 600; color: " +
      c("textMuted") +
      "; }"
      ".caption { font-size: 10px; color: " +
      c("textMuted") + "; } .tiny { font-size: 9px; color: " + c("textMuted") +
      "; }"
      ".name { font-size: 11px; font-weight: 600; } .card { background: " +
      c("surfaceSecondary") + "; border: 0.5px solid " + c("border") +
      "; border-radius: 8px; }"
      ".shortcut-row { background: " + c("surfaceSecondary") + "; border: 0.5px solid " + c("border") +
      "; border-radius: " + std::to_string(Visuals().at("metrics").at(name == "liquidGlass" ? "settingsGlassCardRadius" : "settingsCardRadius").get<int>()) +
      "px; } .shortcut-title { font-size: " + std::to_string(Visuals().at("metrics").at("shortcutTitleFont").get<int>()) +
      "px; font-weight: 600; } .shortcut-recorder { font-family: monospace; font-size: " +
      std::to_string(Visuals().at("metrics").at("shortcutRecorderFont").get<int>()) + "px; font-weight: 500; }"
      ".panel { background: " +
      c("surfaceSecondary") + "; border: 1px solid " + c("border") +
      "; border-radius: 10px; padding: 11px; }"
      ".choice { background: " +
      c("surfaceSecondary") + "; border: 1px solid " + c("border") +
      "; border-radius: 8px; padding: 0; }"
      ".choice.selected { background: mix(" +
      c("surfaceSecondary") + ", " + c("accentGreen") +
      ", 0.10); border-color: " + c("accentGreen") +
      "; }"
      ".icon-button { padding: 0; min-width: 24px; min-height: 24px; "
      "background: transparent; }"
      ".icon-button:hover { background: alpha(" +
      c("accentGreen") +
      ", 0.12); }"
      ".icon-tile { background: " +
      c("surfaceTertiary") +
      "; border-radius: 8px; }"
      "switch { min-width: 34px; min-height: 19px; border-radius: 10px; "
      "background: #4B4B4B; border: 0; padding: 1px; }"
      "switch:checked { background: " +
      c("accentGreen") +
      "; } switch slider { min-width: 17px; min-height: 17px; border-radius: "
      "9px; background: white; border: 0; box-shadow: none; }"
      "scrolledwindow { border: 0; } scrollbar { background: transparent; } "
      "scrollbar slider { background: #777; border-radius: 4px; min-width: "
      "5px; min-height: 20px; }"
      ".error { color: " +
      c("dangerText") + "; } .success { color: " + c("accentGreen") + "; }";
  GError *error = nullptr;
  if (!gtk_css_provider_load_from_data(provider, css.c_str(), css.size(),
                                       &error)) {
    g_clear_error(&error);
    throw std::runtime_error("界面主题无法加载");
  }
}
GtkWidget *Label(const std::string &text, const char *style) {
  auto *label = gtk_label_new(text.c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  if (style)
    Class(label, style);
  return label;
}
GtkWidget *Button(const std::string &title, std::function<void()> clicked,
                  const char *style) {
  auto *button = gtk_button_new_with_label(title.c_str());
  if (style)
    Class(button, style);
  auto *action = new std::function<void()>(std::move(clicked));
  g_signal_connect_data(
      button, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
        auto work = *static_cast<std::function<void()> *>(p);
        work();
      }),
      action,
      +[](gpointer p, GClosure *) {
        delete static_cast<std::function<void()> *>(p);
      },
      GConnectFlags(0));
  return button;
}
GtkWidget *Column(int spacing) {
  return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
}
GtkWidget *CompactSwitch(const std::string &label, std::function<void()> clicked) {
  auto *button = Class(gtk_toggle_button_new(), "compact-switch");
  gtk_widget_set_can_focus(button, FALSE);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(button, label.c_str());
  atk_object_set_name(gtk_widget_get_accessible(button), label.c_str());
  auto *drawing = gtk_drawing_area_new();
  gtk_widget_set_size_request(drawing,
      native::compactSwitchTrackWidth + native::switchControlInset,
      native::compactSwitchTrackHeight + native::switchControlInset);
  gtk_container_add(GTK_CONTAINER(button), drawing);
  g_signal_connect(drawing, "draw", G_CALLBACK((+[](GtkWidget *widget, cairo_t *cr, gpointer data) -> gboolean {
    auto *control = GTK_WIDGET(data);
    const bool on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control));
    const bool enabled = gtk_widget_is_sensitive(control);
    const bool pressed = gtk_widget_get_state_flags(control) & GTK_STATE_FLAG_ACTIVE;
    auto color = [&](const char *name, double alpha) {
      GdkRGBA rgba{};
      gtk_style_context_lookup_color(gtk_widget_get_style_context(control), name, &rgba);
      rgba.alpha = alpha; gdk_cairo_set_source_rgba(cr, &rgba);
    };
    const double width = native::compactSwitchTrackWidth, height = native::compactSwitchTrackHeight;
    const double x = (gtk_widget_get_allocated_width(widget) - width) / 2.0;
    const double y = (gtk_widget_get_allocated_height(widget) - height) / 2.0;
    const double radius = height / 2;
    cairo_arc(cr, x + width - radius, y + radius, radius, -M_PI / 2, M_PI / 2);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI / 2, 3 * M_PI / 2);
    cairo_close_path(cr);
    color(on ? "xsime_switch_on" : "xsime_switch_off", enabled ? (pressed ? 0.78 : 1) : 0.48);
    cairo_fill_preserve(cr);
    color(on ? "xsime_switch_accent" : "xsime_switch_border", enabled ? 0.78 : 0.30);
    cairo_set_line_width(cr, 1); cairo_stroke(cr);
    const double inset = native::switchKnobInset, knob = height - inset * 2;
    const double knob_x = on ? x + width - inset - knob : x + inset;
    cairo_arc(cr, knob_x + knob / 2, y + inset + knob / 2, knob / 2, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 1, 1, 1, enabled ? 1 : 0.70); cairo_fill(cr);
    return FALSE;
  })), button);
  auto *action = new std::function<void()>(std::move(clicked));
  g_signal_connect_data(button, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    (*static_cast<std::function<void()> *>(data))();
  }), action, +[](gpointer data, GClosure *) { delete static_cast<std::function<void()> *>(data); }, GConnectFlags(0));
  return button;
}
GtkWidget *Row(int spacing) {
  return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
}
void Add(GtkWidget *box, GtkWidget *child, bool expand) {
  gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0);
}
void Clear(GtkWidget *container) {
  auto *children = gtk_container_get_children(GTK_CONTAINER(container));
  for (auto *child = children; child; child = child->next)
    gtk_widget_destroy(GTK_WIDGET(child->data));
  g_list_free(children);
}
GtkWidget *Icon(const std::string &name, int size) {
  auto *area = Class(gtk_drawing_area_new(), "glyph");
  gtk_widget_set_size_request(area, size, size);
  gtk_widget_set_halign(area, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(area, GTK_ALIGN_CENTER);
  auto *identity = new std::string(name);
  g_signal_connect_data(
      area, "draw",
      G_CALLBACK((+[](GtkWidget *widget, cairo_t *cr,
                      gpointer data) -> gboolean {
        const auto &icon = *static_cast<std::string *>(data);
        GdkRGBA color;
        gtk_style_context_get_color(gtk_widget_get_style_context(widget),
                                    gtk_widget_get_state_flags(widget), &color);
        cairo_set_source_rgba(cr, color.red, color.green, color.blue,
                              color.alpha);
        if (icon == "rimes.brand.chatgpt" || icon == "rimes.brand.claude") {
          static std::map<std::string, GdkPixbuf *> images;
          if (!images.count(icon)) {
            const auto encoded =
                Visuals().at("brands").at(icon.substr(12)).get<std::string>();
            gsize count = 0;
            auto *bytes = g_base64_decode(encoded.c_str(), &count);
            auto *loader = gdk_pixbuf_loader_new();
            if (gdk_pixbuf_loader_write(loader, bytes, count, nullptr) &&
                gdk_pixbuf_loader_close(loader, nullptr))
              images[icon] = GDK_PIXBUF(
                  g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader)));
            g_object_unref(loader);
            g_free(bytes);
          }
          if (images.count(icon)) {
            auto *pixbuf = images.at(icon);
            auto *surface =
                gdk_cairo_surface_create_from_pixbuf(pixbuf, 1, nullptr);
            cairo_scale(cr,
                        double(gtk_widget_get_allocated_width(widget)) /
                            gdk_pixbuf_get_width(pixbuf),
                        double(gtk_widget_get_allocated_height(widget)) /
                            gdk_pixbuf_get_height(pixbuf));
            if (icon == "rimes.brand.chatgpt")
              cairo_mask_surface(cr, surface, 0, 0);
            else {
              cairo_set_source_surface(cr, surface, 0, 0);
              cairo_paint(cr);
            }
            cairo_surface_destroy(surface);
          }
          return FALSE;
        }
        cairo_scale(cr, gtk_widget_get_allocated_width(widget) / 24.0,
                    gtk_widget_get_allocated_height(widget) / 24.0);
        cairo_set_line_width(cr, 1.55);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        auto line = [&](double x, double y, double a, double b) {
          cairo_move_to(cr, x, y);
          cairo_line_to(cr, a, b);
        };
        if (icon == "house") {
          line(2, 11, 12, 3);
          cairo_line_to(cr, 22, 11);
          line(5, 9, 5, 21);
          cairo_line_to(cr, 10, 21);
          cairo_line_to(cr, 10, 14);
          cairo_line_to(cr, 15, 14);
          cairo_line_to(cr, 15, 21);
          cairo_line_to(cr, 20, 21);
          cairo_line_to(cr, 20, 9);
        } else if (icon == "square.grid.2x2") {
          for (int y : {3, 14})
            for (int x : {3, 14})
              cairo_rectangle(cr, x, y, 7, 7);
        } else if (icon == "keyboard") {
          cairo_rectangle(cr, 2, 6, 20, 13);
          for (int y : {9, 12})
            for (int x : {5, 9, 13, 17}) {
              cairo_move_to(cr, x, y);
              cairo_line_to(cr, x + 1, y);
            }
          line(7, 16, 17, 16);
        } else if (icon == "info.circle") {
          cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
          line(12, 11, 12, 17);
          line(12, 7, 12, 7.2);
        } else if (icon == "arrow.down.circle") {
          cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
          line(12, 6, 12, 17);
          line(7, 12, 12, 17);
          cairo_line_to(cr, 17, 12);
        } else if (icon == "archivebox") {
          cairo_rectangle(cr, 3, 4, 18, 4);
          cairo_rectangle(cr, 5, 8, 14, 13);
          line(9, 12, 15, 12);
        } else if (icon == "tray.full" || icon == "tray") {
          line(3, 12, 7, 5);
          cairo_line_to(cr, 17, 5);
          cairo_line_to(cr, 21, 12);
          cairo_line_to(cr, 21, 19);
          cairo_line_to(cr, 3, 19);
          cairo_close_path(cr);
          line(3, 12, 8, 12);
          cairo_curve_to(cr, 8, 17, 16, 17, 16, 12);
          cairo_line_to(cr, 21, 12);
          line(8, 8, 16, 8);
        } else if (icon == "globe" || icon == "network") {
          cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
          line(3, 12, 21, 12);
          cairo_move_to(cr, 12, 3);
          cairo_curve_to(cr, 5, 7, 5, 17, 12, 21);
          cairo_curve_to(cr, 19, 17, 19, 7, 12, 3);
        } else if (icon == "link") {
          cairo_save(cr);
          cairo_translate(cr, 12, 12);
          cairo_rotate(cr, -M_PI / 4);
          cairo_rectangle(cr, -3, -10, 6, 9);
          cairo_rectangle(cr, -3, 1, 6, 9);
          line(0, -4, 0, 4);
          cairo_restore(cr);
        } else if (icon == "gearshape") {
          cairo_arc(cr, 12, 12, 4, 0, 2 * M_PI);
          for (int i = 0; i < 8; ++i) {
            const auto a = i * M_PI / 4;
            line(12 + 7 * cos(a), 12 + 7 * sin(a), 12 + 10 * cos(a),
                 12 + 10 * sin(a));
          }
          cairo_arc(cr, 12, 12, 7, 0, 2 * M_PI);
        } else if (icon == "ellipsis") {
          for (int x : {5, 12, 19}) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, x, 12, 1.35, 0, 2 * M_PI);
            cairo_fill(cr);
          }
        } else if (icon == "magnifyingglass") {
          cairo_arc(cr, 10, 10, 6.5, 0, 2 * M_PI);
          line(15, 15, 21, 21);
        } else if (icon == "xmark") {
          line(6, 6, 18, 18);
          line(18, 6, 6, 18);
        } else if (icon == "chevron.left" || icon == "chevron.right") {
          const double a = icon == "chevron.left" ? 15 : 9;
          const double b = 24 - a;
          line(a, 5, b, 12);
          cairo_line_to(cr, a, 19);
        } else if (icon == "arrow.uturn.backward") {
          line(9, 4, 3, 9);
          cairo_line_to(cr, 9, 14);
          line(3, 9, 15, 9);
          cairo_curve_to(cr, 24, 9, 24, 21, 15, 21);
        } else if (icon == "checkmark") {
          line(5, 12, 10, 18);
          cairo_line_to(cr, 20, 5);
        } else if (icon == "doc.on.doc") {
          cairo_rectangle(cr, 3, 3, 12, 14);
          cairo_rectangle(cr, 8, 8, 13, 14);
        } else if (icon == "doc.on.clipboard") {
          cairo_rectangle(cr, 4, 4, 16, 18);
          cairo_rectangle(cr, 8, 2, 8, 4);
          line(8, 11, 16, 11);
          line(8, 15, 16, 15);
        } else if (icon == "paperplane.fill") {
          line(2, 10, 22, 2);
          cairo_line_to(cr, 15, 22);
          cairo_line_to(cr, 10, 14);
          cairo_close_path(cr);
          cairo_fill(cr);
          line(10, 14, 18, 6);
        } else if (icon == "sparkles") {
          line(12, 2, 14, 9);
          cairo_line_to(cr, 22, 12);
          cairo_line_to(cr, 14, 15);
          cairo_line_to(cr, 12, 22);
          cairo_line_to(cr, 9, 15);
          cairo_line_to(cr, 2, 12);
          cairo_line_to(cr, 9, 9);
          cairo_close_path(cr);
        } else if (icon == "chart.bar" || icon == "chart.bar.xaxis") {
          for (int i = 0; i < 4; ++i)
            cairo_rectangle(cr, 3 + i * 5, 18 - (i % 3) * 4, 2,
                            3 + (i % 3) * 4);
          line(2, 22, 22, 22);
        } else if (icon == "waveform") {
          for (int i = 0; i < 5; ++i) {
            const double h = (i == 2 ? 19 : i % 2 ? 11 : 6);
            line(4 + i * 4, 12 - h / 2, 4 + i * 4, 12 + h / 2);
          }
        } else if (icon == "clock.arrow.circlepath") {
          cairo_arc(cr, 12, 12, 9, -2.5, 2.7);
          line(3, 6, 2, 12); cairo_line_to(cr, 7, 10);
          line(12, 6, 12, 12); cairo_line_to(cr, 16, 16);
        } else if (icon == "camera.viewfinder") {
          for (int x : {3, 21}) for (int y : {3, 21}) {
            line(x, y + (y == 3 ? 5 : -5), x, y); cairo_line_to(cr, x + (x == 3 ? 5 : -5), y);
          }
          cairo_new_sub_path(cr); cairo_arc(cr, 12, 12, 4, 0, 2 * M_PI);
        } else if (icon == "lock") {
          cairo_rectangle(cr, 5, 10, 14, 12);
          cairo_move_to(cr, 8, 10); cairo_line_to(cr, 8, 6);
          cairo_arc(cr, 12, 6, 4, M_PI, 2 * M_PI); cairo_line_to(cr, 16, 10);
        } else if (icon == "note.text") {
          cairo_rectangle(cr, 3, 4, 18, 16);
          line(6, 8, 18, 8); line(6, 12, 18, 12); line(6, 16, 13, 16);
        } else if (icon == "books.vertical") {
          cairo_rectangle(cr, 2, 5, 4, 16); cairo_rectangle(cr, 8, 2, 4, 19);
          line(14, 4, 18, 3); cairo_line_to(cr, 22, 20); cairo_line_to(cr, 18, 21); cairo_close_path(cr);
        } else if (icon == "character.book.closed") {
          cairo_rectangle(cr, 5, 3, 15, 18);
          line(8, 3, 8, 21);
          line(11, 7, 16, 7);
          line(11, 11, 16, 11);
          line(11, 15, 16, 15);
        } else if (icon == "rimes.brand.scholay") {
          for (int i = 0; i < 12; ++i) {
            const auto a = i * M_PI / 6 - M_PI / 2, r = (i % 2 ? 6.1 : 11.3);
            const auto x = 12 + cos(a) * r, y = 12 + sin(a) * r;
            if (i == 0)
              cairo_move_to(cr, x, y);
            else
              cairo_line_to(cr, x, y);
          }
          cairo_close_path(cr);
          cairo_fill(cr);
        } else if (icon == "puzzlepiece.extension") {
          line(3, 4, 9, 4);
          cairo_curve_to(cr, 7, -1, 17, -1, 15, 4);
          cairo_line_to(cr, 20, 4);
          cairo_line_to(cr, 20, 10);
          cairo_curve_to(cr, 25, 8, 25, 18, 20, 16);
          cairo_line_to(cr, 20, 21);
          cairo_line_to(cr, 3, 21);
          cairo_line_to(cr, 3, 15);
          cairo_curve_to(cr, 8, 18, 8, 8, 3, 10);
          cairo_close_path(cr);
        } else if (icon == "wrench.and.screwdriver") {
          line(5, 2, 20, 18);
          cairo_line_to(cr, 17, 21);
          cairo_line_to(cr, 2, 5);
          cairo_close_path(cr);
          line(5, 19, 17, 7);
          line(15, 4, 20, 1);
          cairo_curve_to(cr, 25, 6, 20, 13, 15, 9);
          line(3, 17, 7, 21);
        } else if (icon == "dot.radiowaves.left.and.right") {
          for (int i = 0; i < 3; ++i) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, 12, 12, 5 + i * 3, -0.8, 0.8);
            cairo_new_sub_path(cr);
            cairo_arc(cr, 12, 12, 5 + i * 3, M_PI - 0.8, M_PI + 0.8);
          }
          cairo_new_sub_path(cr);
          cairo_arc(cr, 12, 12, 1, 0, 2 * M_PI);
        } else if (icon == "paintpalette") {
          cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
          for (int i = 0; i < 4; ++i) {
            auto a = 2 + i;
            cairo_move_to(cr, 12 + 5 * cos(a), 12 + 5 * sin(a));
            cairo_arc(cr, 12 + 5 * cos(a), 12 + 5 * sin(a), 1, 0, 2 * M_PI);
          }
        } else {
          cairo_rectangle(cr, 4, 4, 16, 16);
          line(8, 12, 16, 12);
          line(12, 8, 12, 16);
        }
        cairo_stroke(cr);
        return FALSE;
      })),
      identity,
      +[](gpointer p, GClosure *) { delete static_cast<std::string *>(p); },
      GConnectFlags(0));
  return area;
}
GtkWidget *IconButton(const std::string &icon, const std::string &label,
                      std::function<void()> clicked) {
  auto *button = Button("", std::move(clicked), "icon-button");
  gtk_container_remove(GTK_CONTAINER(button),
                       gtk_bin_get_child(GTK_BIN(button)));
  gtk_container_add(GTK_CONTAINER(button), Icon(icon, 16));
  gtk_widget_set_tooltip_text(button, label.c_str());
  atk_object_set_name(gtk_widget_get_accessible(button), label.c_str());
  return button;
}
bool Confirm(GtkWindow *parent, const std::string &title,
             const std::string &detail) {
  auto *dialog =
      gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                             GTK_BUTTONS_NONE, "%s", title.c_str());
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                           detail.c_str());
  gtk_dialog_add_buttons(GTK_DIALOG(dialog), "取消", GTK_RESPONSE_CANCEL,
                         "确认", GTK_RESPONSE_ACCEPT, nullptr);
  const bool result = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
  gtk_widget_destroy(dialog);
  return result;
}
GtkWidget *ThemeCard(const std::string &theme, const std::string &title,
                     bool selected, std::function<void()> clicked) {
  auto *button = Button("", std::move(clicked), "choice");
  gtk_container_remove(GTK_CONTAINER(button),
                       gtk_bin_get_child(GTK_BIN(button)));
  auto *area = gtk_drawing_area_new();
  gtk_widget_set_size_request(area, 319, 114);
  struct Preview {
    std::string theme, title;
    bool selected;
  };
  auto *state = new Preview{theme, title, selected};
  g_signal_connect_data(
      area, "draw",
      G_CALLBACK((+[](GtkWidget *widget, cairo_t *cr,
                      gpointer data) -> gboolean {
        const auto &preview = *static_cast<Preview *>(data);
        const auto &palette =
            Visuals()
                .at("palettes")
                .at(preview.theme == "liquidGlass" ? "day" : preview.theme);
        const double width = gtk_widget_get_allocated_width(widget),
                     height = gtk_widget_get_allocated_height(widget);
        auto color = [&](const char *key) {
          GdkRGBA value;
          gdk_rgba_parse(&value, palette.at(key).get<std::string>().c_str());
          gdk_cairo_set_source_rgba(cr, &value);
        };
        auto rounded = [&](double x, double y, double w, double h, double r) {
          cairo_new_sub_path(cr);
          cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
          cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
          cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
          cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
          cairo_close_path(cr);
        };
        color("surfaceSecondary");
        rounded(0, 0, width, height, 8);
        cairo_fill(cr);
        color("surface");
        rounded(9, 9, width - 18, 68, 6);
        cairo_fill_preserve(cr);
        color("border");
        cairo_set_line_width(cr, 0.5);
        cairo_stroke(cr);
        const std::vector<const char *> swatches =
            preview.theme == "rasta"
                ? std::vector<const char *>{"brandRed", "brandYellow",
                                            "brandGreen"}
                : std::vector<const char *>{"surfaceSecondary",
                                            "selectedCandidateBackground",
                                            "accentGreen"};
        const double swatch_width = (width - 54) / 3;
        for (int i = 0; i < 3; ++i) {
          color(swatches[i]);
          rounded(21 + i * (swatch_width + 6), 28, swatch_width, 30, 4);
          cairo_fill(cr);
        }
        auto text = [&](const std::string &value, int size, bool bold, double y,
                        const char *tint) {
          auto *layout = gtk_widget_create_pango_layout(widget, value.c_str());
          auto *font =
              pango_font_description_copy(pango_context_get_font_description(
                  gtk_widget_get_pango_context(widget)));
          pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_SEMIBOLD
                                                       : PANGO_WEIGHT_NORMAL);
          pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
          pango_layout_set_font_description(layout, font);
          pango_font_description_free(font);
          color(tint);
          cairo_move_to(cr, 10, y);
          pango_cairo_show_layout(cr, layout);
          g_object_unref(layout);
        };
        text(preview.title, 11, true, 81, "textPrimary");
        text(preview.theme == "liquidGlass" ? "Apple"
             : preview.theme == "rasta"     ? "拉斯塔"
                                            : "经典",
             9, false, 96, "textMuted");
        if (preview.selected) {
          color("accentGreen");
          cairo_arc(cr, width - 21, 22, 6.5, 0, 2 * M_PI);
          cairo_fill(cr);
          color("surfaceSecondary");
          cairo_set_line_width(cr, 1.5);
          cairo_move_to(cr, width - 24, 22);
          cairo_line_to(cr, width - 22, 24);
          cairo_line_to(cr, width - 18, 20);
          cairo_stroke(cr);
        }
        return FALSE;
      })),
      state, +[](gpointer p, GClosure *) { delete static_cast<Preview *>(p); },
      GConnectFlags(0));
  gtk_container_add(GTK_CONTAINER(button), area);
  atk_object_set_name(gtk_widget_get_accessible(button), title.c_str());
  return button;
}
bool Render(GtkWidget *widget, const std::string &path, int scale) {
  const auto width = gtk_widget_get_allocated_width(widget),
             height = gtk_widget_get_allocated_height(widget);
  auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width * scale,
                                             height * scale);
  auto *cr = cairo_create(surface);
  cairo_scale(cr, scale, scale);
  gtk_widget_draw(widget, cr);
  cairo_destroy(cr);
  const auto result = cairo_surface_write_to_png(surface, path.c_str());
  cairo_surface_destroy(surface);
  return result == CAIRO_STATUS_SUCCESS;
}
Form::Form(std::vector<runtime::Field> fields, const Json &values)
    : fields_(std::move(fields)), box_(Column(10)) {
  for (const auto &f : fields_) {
    auto *row = Row(12);
    auto *title = Label(f.title);
    gtk_widget_set_size_request(
        title, f.kind == runtime::FieldKind::Toggle ? 460 : 110, -1);
    Add(row, title);
    GtkWidget *input = nullptr;
    const auto value = values.value(f.id, f.default_value);
    if (f.kind == runtime::FieldKind::Choice) {
      input = gtk_combo_box_text_new();
      for (const auto &c : f.choices)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(input), c.first.c_str(),
                                  c.second.c_str());
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(input),
                                  value.get<std::string>().c_str());
    } else if (f.kind == runtime::FieldKind::Toggle) {
      input = gtk_switch_new();
      gtk_switch_set_active(GTK_SWITCH(input), value.get<bool>());
      gtk_widget_set_halign(input, GTK_ALIGN_START);
    } else if (f.kind == runtime::FieldKind::Number) {
      input = gtk_spin_button_new_with_range(f.minimum, f.maximum, 1);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(input), value.get<int>());
    } else {
      input = gtk_entry_new();
      gtk_entry_set_max_length(GTK_ENTRY(input), f.maximum);
      gtk_entry_set_text(GTK_ENTRY(input),
                         f.kind == runtime::FieldKind::Secret
                             ? ""
                             : value.get<std::string>().c_str());
      gtk_entry_set_visibility(GTK_ENTRY(input),
                               f.kind != runtime::FieldKind::Secret);
      gtk_entry_set_placeholder_text(GTK_ENTRY(input), f.help.c_str());
    }
    gtk_widget_set_tooltip_text(input, f.help.c_str());
    atk_object_set_name(gtk_widget_get_accessible(input), f.title.c_str());
    gtk_widget_set_size_request(
        input, f.kind == runtime::FieldKind::Toggle ? 36 : 420, -1);
    Add(row, input);
    Add(box_, row);
    widgets_[f.id] = input;
  }
}
Json Form::Values() const {
  auto result = Json::object();
  for (const auto &f : fields_) {
    auto *w = widgets_.at(f.id);
    if (f.kind == runtime::FieldKind::Secret)
      continue;
    if (f.kind == runtime::FieldKind::Choice) {
      const auto *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(w));
      result[f.id] = id ? id : "";
    } else if (f.kind == runtime::FieldKind::Toggle)
      result[f.id] = bool(gtk_switch_get_active(GTK_SWITCH(w)));
    else if (f.kind == runtime::FieldKind::Number)
      result[f.id] = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w));
    else
      result[f.id] = gtk_entry_get_text(GTK_ENTRY(w));
  }
  return runtime::ValidateFields(fields_, result);
}
std::string Form::Secret(const std::string &id) const {
  return gtk_entry_get_text(GTK_ENTRY(widgets_.at(id)));
}
void Form::ClearSecrets() {
  for (const auto &f : fields_)
    if (f.kind == runtime::FieldKind::Secret)
      gtk_entry_set_text(GTK_ENTRY(widgets_.at(f.id)), "");
}
} // namespace rimes::desktop
