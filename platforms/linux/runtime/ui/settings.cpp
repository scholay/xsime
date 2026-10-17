#include "translation_resources.hpp"
#include "native_buffer_metrics.hpp"
#include "desktop.hpp"
#include "official_catalog.hpp"
#include "shortcuts.hpp"
#include "launcher_client.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>

namespace {
using namespace rimes::desktop;
using namespace rimes::runtime;
using namespace rimes::plugins;
struct Settings {
  static int Metric(const char *key) { return Visuals().at("metrics").at(key); }
  ConfigurationStore config{UserRoot() / "configuration"};
  PackageStore packages{UserRoot() / "plugins", OfficialCatalog(),
                        BundledPackages(), CompiledAdapters(),
                        RIMES_PRODUCT_VERSION};
  Json saved = config.Load();
  GtkWidget *window = nullptr, *root = nullptr, *content = nullptr,
            *sidebar = nullptr, *feedback = nullptr;
  std::string route = "core.rimes", subpage = "about", message;
  std::map<std::string, GtkWidget *> navigation;
  std::map<std::string, GtkWidget *> shortcut_buttons;
  GtkWidget *shortcut_status = nullptr;
  std::string recording_action, recording_token;
  bool recording_ready = false;
  uint64_t page_revision = 0;
  guint shortcut_poll = 0;
  bool rendering = false, busy = false;
  bool application_window = false;
  GSubprocess *operation = nullptr;
  GCancellable *cancel = g_cancellable_new();
  std::shared_ptr<std::atomic<bool>> alive =
      std::make_shared<std::atomic<bool>>(true);
  ~Settings() {
    alive->store(false);
    if (window) {
      g_signal_handlers_disconnect_by_data(window, this);
      gtk_widget_destroy(window);
      window = nullptr;
    }
    if (shortcut_poll) g_source_remove(shortcut_poll);
    if (operation) {
      g_subprocess_force_exit(operation);
      g_object_unref(operation);
    }
    g_cancellable_cancel(cancel);
    g_object_unref(cancel);
  }
  void Message(const std::string &text) {
    message = text;
    if (feedback)
      gtk_label_set_text(GTK_LABEL(feedback), text.c_str());
  }
  template <class F> void Try(F action) {
    try {
      action();
    } catch (const std::exception &) {
      Message("操作未完成，请检查设置、权限或网络后重试；原数据已保留。");
    }
  }
  void Save(Json next, const std::map<std::string, std::string> &secrets = {}) {
    const bool cleaned = config.Save(next, saved.at("revision"), secrets);
    saved = config.Load();
    Theme(saved.at("theme"));
    Message(cleaned ? "已保存" : "已保存；旧密钥清理未完成，请检查目录权限。");
  }
  void Build(bool offscreen, GtkApplication *application = nullptr) {
    rendering = offscreen;
    application_window = application != nullptr;
    if (offscreen)
      g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE,
                   nullptr);
    Theme(saved.at("theme"));
    window = offscreen ? gtk_offscreen_window_new()
                       : application ? gtk_application_window_new(application) : gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "XSIME 设置");
    gtk_window_set_default_size(GTK_WINDOW(window), Metric("settingsWidth"),
                                Metric("settingsHeight"));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    // GtkOffscreenWindow can allocate its minimum instead of the requested
    // default after a page switch. A render fixture has an exact canvas;
    // interactive windows retain their independent resizable minimum.
    gtk_widget_set_size_request(window, offscreen ? Metric("settingsWidth") : 860,
                                offscreen ? Metric("settingsHeight") : 600);
    GdkGeometry geometry{};
    geometry.min_width = 860;
    geometry.min_height = 600;
    gtk_window_set_geometry_hints(GTK_WINDOW(window), nullptr, &geometry,
                                  GDK_HINT_MIN_SIZE);
    g_signal_connect(window, "destroy",
                     G_CALLBACK(+[](GtkWidget *, gpointer p) {
                       auto *self = static_cast<Settings *>(p);
                       self->alive->store(false);
                       self->window = nullptr;
                       if (!self->rendering && !self->application_window)
                         gtk_main_quit();
                     }),
                     this);
    root = Class(Column(0), "settings");
    gtk_container_add(GTK_CONTAINER(window), root);
    auto *titlebar = gtk_event_box_new();
    gtk_widget_set_size_request(titlebar, -1, 28);
    gtk_widget_add_events(titlebar, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(
        titlebar, "button-press-event",
        G_CALLBACK(+[](GtkWidget *, GdkEventButton *e, gpointer p) -> gboolean {
          if (e->button == 1)
            gtk_window_begin_move_drag(
                GTK_WINDOW(static_cast<Settings *>(p)->window), 1, e->x_root,
                e->y_root, e->time);
          return TRUE;
        }),
        this);
    if (!offscreen) {
      auto *bar = Row(6);
      gtk_widget_set_margin_start(bar, 10);
      gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
      Add(bar, IconButton("xmark", "关闭设置",
                          [this] { gtk_widget_destroy(window); }));
      Add(bar, Label("XSIME 设置", "tiny"), true);
      gtk_container_add(GTK_CONTAINER(titlebar), bar);
    }
    Add(root, titlebar);
    auto *body = Row(0);
    Add(root, body, true);
    auto *scroll = Class(gtk_scrolled_window_new(nullptr, nullptr), "sidebar");
    gtk_widget_set_size_request(scroll, Metric("sidebarWidth") + 1, -1);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    Add(body, scroll);
    sidebar = Column(4);
    gtk_widget_set_margin_top(sidebar, 16);
    gtk_widget_set_margin_start(sidebar, 12);
    gtk_widget_set_margin_end(sidebar, 12);
    gtk_widget_set_margin_bottom(sidebar, 16);
    gtk_container_add(GTK_CONTAINER(scroll), sidebar);
    auto *group = Label("设置", "sidebar-heading");
    gtk_widget_set_margin_start(group, 8);
    gtk_widget_set_size_request(group, -1, 19);
    Add(sidebar, group);
    for (const auto &r : Visuals().at("routes")) {
      const std::string id = r.at("id"), title = r.at("title"),
                        icon = r.at("icon");
      auto *button = Button("", [this, id] { Select(id); }, "navigation");
      gtk_container_remove(GTK_CONTAINER(button),
                           gtk_bin_get_child(GTK_BIN(button)));
      auto *row = Row(8);
      gtk_widget_set_margin_start(row, 10);
      gtk_widget_set_margin_end(row, 10);
      auto *glyph = Icon(icon);
      gtk_widget_set_valign(glyph, GTK_ALIGN_CENTER);
      Add(row, glyph);
      Add(row, Label(title), true);
      gtk_container_add(GTK_CONTAINER(button), row);
      atk_object_set_name(gtk_widget_get_accessible(button), title.c_str());
      Add(sidebar, button);
      navigation[id] = button;
    }
    content = Column(0);
    Add(body, content, true);
    Select(route, subpage);
    if (!offscreen) shortcut_poll = g_timeout_add_seconds(2, +[](gpointer data) -> gboolean {
      auto* self = static_cast<Settings*>(data);
      if (self->alive->load() && self->route == "core.buffer") self->ShortcutStatus();
      return G_SOURCE_CONTINUE;
    }, this);
  }
  const Json &Route() const {
    for (const auto &r : Visuals().at("routes"))
      if (r.at("id") == route)
        return r;
    throw std::runtime_error("Unknown route");
  }
  void Select(std::string next, const std::string &page = "") {
    StopShortcutRecording();
    ++page_revision;
    shortcut_buttons.clear(); shortcut_status = nullptr;
    route = std::move(next);
    const auto &r = Route();
    subpage = page.empty() ? r.at("subpages")[0][0].get<std::string>() : page;
    for (const auto &[id, button] : navigation) {
      auto *style = gtk_widget_get_style_context(button);
      if (id == route)
        gtk_style_context_add_class(style, "selected");
      else
        gtk_style_context_remove_class(style, "selected");
    }
    Clear(content);
    feedback = nullptr;
    auto *tabs = Class(Row(0), "tabs");
    gtk_widget_set_size_request(tabs, -1, Metric("tabsHeight"));
    auto *segments = Row(0);
    gtk_widget_set_margin_start(segments, 24);
    gtk_widget_set_valign(segments, GTK_ALIGN_CENTER);
    Add(tabs, segments);
    for (const auto &p : r.at("subpages")) {
      const std::string id = p[0], title = p[1];
      auto *b = Button(title, [this, id] { Select(route, id); }, "segment");
      if (id == subpage)
        Class(b, "selected");
      Add(segments, b);
    }
    feedback = Label(message, "tiny");
    // A long validation message must not change the native window geometry.
    gtk_label_set_ellipsize(GTK_LABEL(feedback), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(feedback, message.c_str());
    gtk_widget_set_margin_start(feedback, 16);
    gtk_widget_set_margin_end(feedback, 24);
    Add(tabs, feedback, true);
    Add(content, tabs);
    auto *heading = Label(
        route == "core.rimes" ? "关于 XSIME" : r.at("title").get<std::string>(),
        "heading");
    gtk_widget_set_size_request(heading, -1, Metric("headingHeight"));
    gtk_widget_set_margin_start(heading, 24);
    Add(content, heading);
    auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    Add(content, scroll, true);
    auto *body = Column(8);
    gtk_widget_set_halign(body, GTK_ALIGN_START);
    gtk_widget_set_size_request(body, Metric("contentWidth"), -1);
    gtk_widget_set_margin_start(body, 24);
    gtk_widget_set_margin_end(body, 24);
    gtk_widget_set_margin_bottom(body, 22);
    gtk_container_add(GTK_CONTAINER(scroll), body);
    if (route == "core.plugins")
      Plugins(body);
    else if (route == "core.connectors" && subpage == "ai-model")
      Connectors(body);
    else if (route == "core.appearance")
      Appearance(body);
    else if (route == "core.capsule")
        Capsule(body);
    else if (route == "core.buffer")
      Buffer(body);
    else if (route == "core.input-method") {
      Add(body, Label(subpage == "encoding" ? "输入方案" : "词库", "section"));
      Add(body, Button("导入与管理…", [this] { Launch("rimes-schemes"); }));
      Add(body, Label("导入方案和移除方案均保留学习记录。", "caption"));
    } else if (route == "core.rimes") {
      Add(body, Label("XSIME", "heading"));
      Add(body, Label(std::string("版本 ") + RIMES_PRODUCT_VERSION, "caption"));
      Add(body, Label("学术海 · 原生中文输入与文本工作台", "section"));
    } else if (route == "core.maintenance" && subpage == "logs-data") {
      Add(body, Label("用户词库、学习记录和工作台数据", "section"));
      Add(body, Button("打开数据目录…", [this] { OpenPath(UserRoot()); }));
      Add(body, Button("重新加载设置", [this] {
            Try([&] {
              saved = config.Load();
              Select(route, subpage);
              Message("已重新加载");
            });
          }));
    } else {
      Add(body, Label("此页面随对应 Linux 模块接入。", "caption"));
    }
    gtk_widget_show_all(content);
  }
  void OpenPath(const std::filesystem::path &path) {
    gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (!uri) {
      Message("无法打开目录");
      return;
    }
    GError *error = nullptr;
    if (!g_app_info_launch_default_for_uri(uri, nullptr, &error))
      Message("未找到可用的文件管理器");
    g_clear_error(&error);
    g_free(uri);
  }
  void Launch(const char *program) {
    GError *error = nullptr;
    auto *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error,
                                  program, nullptr);
    if (!proc)
      Message("功能尚未安装，请检查程序版本");
    else
      g_object_unref(proc);
    g_clear_error(&error);
  }
  void PackageAction(const std::string &id, const std::string &action) {
    if (busy)
      return;
    Try([&] {
      const auto view = packages.Inspect(id);
      const auto command =
          Json{{"op", action}, {"id", id}, {"grant", view.grant}}.dump();
      auto binary =
          std::filesystem::read_symlink("/proc/self/exe").parent_path() /
          "rimes-plugins";
      GError *error = nullptr;
      operation =
          g_subprocess_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                            G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                           &error, binary.c_str(), "--request", nullptr);
      if (!operation) {
        g_clear_error(&error);
        throw std::runtime_error("Companion unavailable");
      }
      busy = true;
      Message("正在处理…");
      struct Completion {
        Settings *owner;
        std::shared_ptr<std::atomic<bool>> alive;
      };
      auto *completion = new Completion{this, alive};
      g_subprocess_communicate_utf8_async(
          operation, command.c_str(), cancel,
          +[](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Completion> done(static_cast<Completion *>(data));
            gchar *output = nullptr;
            GError *error = nullptr;
            bool ok = g_subprocess_communicate_utf8_finish(
                G_SUBPROCESS(source), result, &output, nullptr, &error);
            if (done->alive->load()) {
              auto *self = done->owner;
              self->busy = false;
              g_clear_object(&self->operation);
              try {
                ok = ok && output && strlen(output) < 2 * 1024 * 1024 &&
                     Json::parse(output).value("ok", false);
              } catch (...) {
                ok = false;
              }
              self->message =
                  ok ? "插件状态已更新" : "操作未完成，请刷新状态后重试";
              self->Select(self->route, self->subpage);
            }
            g_clear_error(&error);
            g_free(output);
          },
          completion);
    });
  }
  void Configure(const std::string &id) {
    Try([&] {
      auto *dialog = gtk_dialog_new_with_buttons(
          "插件设置", GTK_WINDOW(window), GTK_DIALOG_MODAL, "恢复默认", 1,
          "取消", GTK_RESPONSE_CANCEL, "保存", GTK_RESPONSE_ACCEPT, nullptr);
      gtk_window_set_default_size(GTK_WINDOW(dialog), 610, 240);
      Form form(PluginFields(id), config.Plugin(id));
      gtk_widget_set_margin_start(form.Widget(), 18);
      gtk_widget_set_margin_end(form.Widget(), 18);
      gtk_widget_set_margin_top(form.Widget(), 18);
      gtk_widget_set_margin_bottom(form.Widget(), 18);
      gtk_container_add(
          GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
          form.Widget());
      if (id == "builtin.apple-translation") {
        auto* resources = Button("语言资源…", [dialog] { TranslationResources(GTK_WINDOW(dialog)); });
        gtk_widget_set_margin_start(resources, 18);
        gtk_widget_set_margin_end(resources, 18);
        gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), resources);
      }
      gtk_widget_show_all(dialog);
      const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
      try {
        if (response == GTK_RESPONSE_ACCEPT) {
          auto next = saved;
          next["plugins"][id] = form.Values();
          Save(next);
        } else if (response == 1 &&
                   Confirm(GTK_WINDOW(dialog), "恢复此插件的默认设置？",
                           "连接器与已保存的 API Key 保留。")) {
          auto next = saved;
          next["plugins"].erase(id);
          Save(next);
        }
      } catch (...) {
        gtk_widget_destroy(dialog);
        throw;
      }
      gtk_widget_destroy(dialog);
    });
  }
  void PickPackage(bool uninstall) {
    auto *dialog = gtk_dialog_new_with_buttons(
        uninstall ? "卸载官方插件" : "安装官方插件", GTK_WINDOW(window),
        GTK_DIALOG_MODAL, "取消", GTK_RESPONSE_CANCEL, "确认",
        GTK_RESPONSE_ACCEPT, nullptr);
    auto *choice = gtk_combo_box_text_new();
    int count = 0;
    for (const auto &p : packages.Entries())
      if (p.available && p.installed == uninstall) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(choice), p.id.c_str(),
                                  p.name.c_str());
        ++count;
      }
    gtk_combo_box_set_active(GTK_COMBO_BOX(choice), 0);
    gtk_container_add(
        GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), choice);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT,
                                      count > 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
      const char *value = gtk_combo_box_get_active_id(GTK_COMBO_BOX(choice));
      if (value) {
        const std::string id = value;
        if (uninstall)
          PackageAction(id, "uninstall");
        else {
          const auto catalog = OfficialCatalog();
          bool bundled = false;
          for (const auto &e : catalog.at("plugins"))
            if (e.at("id") == id)
              bundled =
                  e.at("platforms").at("linux").at("distribution") == "bundled";
          PackageAction(id, bundled ? "restore-bundled" : "install");
        }
      }
    }
    gtk_widget_destroy(dialog);
  }
  GtkWidget *Card(const PackageView &p) {
    const auto &identity = Visuals().at("plugins").at(p.id);
    auto *box = Class(Column(5), "card");
    gtk_widget_set_size_request(box, 211, Metric("cardHeight"));
    auto *inner = Column(5);
    gtk_widget_set_margin_start(inner, 10);
    gtk_widget_set_margin_end(inner, 10);
    gtk_widget_set_margin_top(inner, 9);
    gtk_widget_set_margin_bottom(inner, 9);
    Add(box, inner, true);
    auto *top = Row(4);
    auto *tile = Class(gtk_event_box_new(), "icon-tile");
    gtk_widget_set_size_request(tile, 30, 30);
    auto *icon = Icon(identity.at("icon"), 19);
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(tile), icon);
    Add(top, tile);
    Add(top, Label(""), true);
    Add(top, IconButton("info.circle", "查看" + p.name + "详情", [this, p] {
          Message(
              p.name + " · " + p.version + " · " +
              (p.available ? (p.installed ? "已安装" : "未安装") : p.error));
        }));
    if (p.id == "builtin.apple-translation" && !p.installed)
      Add(top, IconButton("gearshape", "本地翻译语言资源", [this] { TranslationResources(GTK_WINDOW(window)); }));
    if (p.installed && !PluginFields(p.id).empty())
      Add(top, IconButton("gearshape", "配置" + p.name,
                          [this, p] { Configure(p.id); }));
    Add(inner, top);
    Add(inner, Label(identity.at("name"), "name"));
    Add(inner, Label("出品方 · " + identity.at("producer").get<std::string>(),
                     "tiny"));
    auto *bottom = Row(6);
    Add(bottom,
        Label(!p.available   ? "尚未适配 Linux"
              : !p.installed ? "未安装"
              : p.enabled    ? "已启用"
                             : "已停用",
              "tiny"),
        true);
    if (p.installed) {
      auto *toggle = gtk_switch_new();
      gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
      gtk_switch_set_active(GTK_SWITCH(toggle), p.enabled);
      atk_object_set_name(gtk_widget_get_accessible(toggle),
                          ("启用" + p.name).c_str());
      struct Toggle {
        Settings *self;
        PackageView view;
      };
      auto *data = new Toggle{this, p};
      g_signal_connect_data(
          toggle, "state-set",
          G_CALLBACK(
              +[](GtkSwitch *, gboolean state, gpointer pointer) -> gboolean {
                auto data = *static_cast<Toggle *>(pointer);
                data.self->PackageAction(data.view.id,
                                         state ? "enable" : "disable");
                return TRUE;
              }),
          data,
          +[](gpointer p, GClosure *) { delete static_cast<Toggle *>(p); },
          GConnectFlags(0));
      Add(bottom, toggle);
    } else {
      auto *install =
          IconButton("arrow.down.circle", "安装" + p.name, [this, p] {
            PackageAction(p.id, p.bundled ? "restore-bundled" : "install");
          });
      gtk_widget_set_sensitive(install, p.available && !busy);
      Add(bottom, install);
    }
    Add(inner, bottom);
    return box;
  }
  void Plugins(GtkWidget *body) {
    if (subpage == "all" || subpage == "buffer-plugins") {
      auto *actions = Row(6);
      Add(actions, Label(""), true);
      Add(actions, Button("安装…", [this] { PickPackage(false); }));
      Add(actions, Button("卸载…", [this] { PickPackage(true); }));
      Add(actions,
          Button("管理…", [this] { OpenPath(UserRoot() / "plugins"); }));
      gtk_widget_set_size_request(actions, Metric("contentWidth"), 16);
      gtk_widget_set_margin_bottom(actions, 8);
      Add(body, actions);
    }
    const std::vector<std::pair<std::string, std::string>> kinds{
        {"mailbox", "Mailbox 插件"},
        {"capsule", "Capsule 插件"},
        {"extension", "内置扩展"},
        {"buffer", "缓冲插件"}};
    const std::map<std::string, std::string> tabs{
        {"mailbox", "mailbox-plugins"},
        {"capsule", "capsule-plugins"},
        {"extension", "built-in-extensions"},
        {"buffer", "buffer-plugins"}};
    auto entries = packages.Entries();
    std::sort(entries.begin(), entries.end(),
              [](const PackageView &a, const PackageView &b) {
                auto folded = [](const std::string &text) {
                  gchar *value = g_utf8_casefold(text.c_str(), -1);
                  std::string result(value);
                  g_free(value);
                  return result;
                };
                return folded(a.name) < folded(b.name);
              });
    for (const auto &[kind, title] : kinds) {
      if (subpage != "all" && subpage != tabs.at(kind))
        continue;
      auto *label = Label(title, "section");
      gtk_widget_set_margin_top(label, 8);
      Add(body, label);
      auto *grid = gtk_grid_new();
      gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
      gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
      gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
      gtk_widget_set_size_request(grid, 650, -1);
      int index = 0;
      for (const auto &p : entries)
        if (p.kind == kind) {
          gtk_grid_attach(GTK_GRID(grid), Card(p), index % 3, index / 3, 1, 1);
          ++index;
        }
      Add(body, grid);
    }
  }
  void ProviderEditor(GtkWidget *body) {
    auto *panel = Class(Column(10), "panel");
    gtk_widget_set_size_request(panel, 650, -1);
    Add(body, panel);
    auto *row = Row(8);
    auto *choice = gtk_combo_box_text_new();
    for (const auto &p : saved.at("providers"))
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(choice),
                                p.at("id").get<std::string>().c_str(),
                                p.at("name").get<std::string>().c_str());
    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(choice),
        saved.at("activeProvider").get<std::string>().c_str());
    gtk_widget_set_size_request(choice, 236, -1);
    Add(row, choice);
    Add(row, Button("新增", [this] {
          Try([&] {
            auto next = saved;
            auto p = FieldDefaults(ProviderFields());
            p["name"] = "新 Provider";
            p["id"] = RandomID();
            next["activeProvider"] = p["id"];
            next["providers"].push_back(p);
            Save(next);
            Select(route, subpage);
          });
        }));
    Add(row, Button("移除", [this] {
          Try([&] {
            if (!Confirm(GTK_WINDOW(window), "移除此 Provider？",
                         "此 Provider 的已保存密钥也会删除。"))
              return;
            auto next = saved;
            auto &list = next["providers"];
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](const Json &p) {
                                        return p.at("id") ==
                                               saved.at("activeProvider");
                                      }),
                       list.end());
            next["activeProvider"] = list.empty() ? Json("") : list[0]["id"];
            Save(next);
            Select(route, subpage);
          });
        }));
    Add(panel, row);
    g_signal_connect(choice, "changed",
                     G_CALLBACK(+[](GtkComboBox *combo, gpointer p) {
                       auto *self = static_cast<Settings *>(p);
                       const auto *id = gtk_combo_box_get_active_id(combo);
                       if (id)
                         self->Try([&] {
                           auto next = self->saved;
                           next["activeProvider"] = id;
                           self->Save(next);
                           self->Select(self->route, self->subpage);
                         });
                     }),
                     this);
    auto found = std::find_if(saved.at("providers").begin(),
                              saved.at("providers").end(), [&](const Json &p) {
                                return p.at("id") == saved.at("activeProvider");
                              });
    if (found == saved.at("providers").end()) {
      Add(panel,
          Label("添加 Provider 后配置 API 地址、模型和密钥。", "caption"));
      return;
    }
    const auto id = found->at("id").get<std::string>();
    auto form = std::make_shared<Form>(ProviderFields(), *found);
    Add(panel, form->Widget());
    auto *actions = Row(8);
    Add(actions, Button("保存", [this, id, form] {
          Try([&] {
            auto next = saved;
            for (auto &p : next["providers"])
              if (p.at("id") == id) {
                auto fields = form->Values();
                for (auto it = fields.begin(); it != fields.end(); ++it)
                  p[it.key()] = it.value();
              }
            std::map<std::string, std::string> secret;
            const auto key = form->Secret("apiKey");
            if (!key.empty())
              secret[id] = key;
            Save(next, secret);
            form->ClearSecrets();
          });
        }));
    Add(actions, Button("清除密钥…", [this, id, form] {
          if (Confirm(GTK_WINDOW(window), "清除此 Provider 的 API Key？",
                      "普通设置和其他 Provider 不变。"))
            Try([&] {
              Save(saved, {{id, ""}});
              form->ClearSecrets();
            });
        }));
    Add(panel, actions);
    Add(panel, Label(found->contains("credential")
                         ? "密钥已保存；留空保持不变。"
                         : "尚未保存密钥；支持无需密钥的本机服务。",
                     "caption"));
  }
  void Connectors(GtkWidget *body) {
    auto *row = Row(8);
    gtk_widget_set_size_request(row, 650, 68);
    for (const auto &entry : std::vector<std::pair<std::string, std::string>>{
             {"codex-cli", "ChatGPT"},
             {"claude-code-cli", "Claude"},
             {"openai-compatible", "AI Provider"}}) {
      const auto id = entry.first;
      auto *b = Button(
          entry.second,
          [this, id] {
            Try([&] {
              auto next = saved;
              next["activeConnector"] = id;
              Save(next);
              Select(route, subpage);
            });
          },
          "choice");
      if (saved.at("activeConnector") == id)
        Class(b, "selected");
      Add(row, b, true);
    }
    Add(body, row);
    if (saved.at("activeConnector") == "openai-compatible")
      ProviderEditor(body);
    else {
      auto *panel = Class(Column(10), "panel");
      gtk_widget_set_size_request(panel, 650, -1);
      Add(panel,
          Label(saved.at("activeConnector") == "codex-cli" ? "Codex CLI"
                                                           : "Claude Code CLI",
                "name"));
      Add(panel, Label("登录与会话入口随对应 CLI 插件接入。", "caption"));
      Add(body, panel);
    }
  }
  void Appearance(GtkWidget *body) {
    if (subpage == "size") {
      auto form = std::make_shared<Form>(
          std::vector<Field>{{"bufferWidth",
                              "工作台宽度",
                              "逻辑像素",
                              FieldKind::Number,
                              760,
                              {},
                              rimes::native::bufferMinimumWidth,
                              rimes::native::bufferMaximumWidth}},
          Json{{"bufferWidth", saved.at("bufferWidth")}});
      Add(body, form->Widget());
      Add(body, Button("应用修改", [this, form] {
            Try([&] {
              auto next = saved;
              next["bufferWidth"] = form->Values().at("bufferWidth");
              Save(next);
            });
          }));
      return;
    }
    Add(body, Label("选择主题", "section"));
    auto *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_widget_set_size_request(grid, 650, -1);
    int index = 0;
    for (const auto &[id, title] :
         std::vector<std::pair<std::string, std::string>>{
             {"night", "墨竹"},
             {"day", "翡翠"},
             {"quiet", "静谧"},
             {"rasta", "拉斯塔"},
             {"liquidGlass", "Liquid Glass"}}) {
      auto *b = ThemeCard(id, title, saved.at("theme") == id, [this, id] {
        Try([&] {
          auto next = saved;
          next["theme"] = id;
          Save(next);
          Select(route, subpage);
        });
      });
      gtk_widget_set_size_request(b, 321, Metric("cardHeight"));
      if (saved.at("theme") == id)
        Class(b, "selected");
      gtk_grid_attach(GTK_GRID(grid), b, index % 2, index / 2, 1, 1);
      ++index;
    }
    Add(body, grid);
  }
  void Capsule(GtkWidget* body) {
      const std::string id = "builtin.capsule.temporary";
      auto form = std::make_shared<Form>(PluginFields(id), config.Plugin(id));
      Add(body, form->Widget());
      Add(body, Button("应用修改", [this, form, id] {
              Try([&] {
                  auto next = saved;
                  next["plugins"][id] = form->Values();
                  Save(next);
              });
          }));
      Add(body,
          Label("文字通过输入法直接上屏；图片、文件和富文本可复制后在目标应用粘贴。", "caption"));
      Add(body, Label("本地资料库", "section"));
      Add(body, Label("剪贴板原始格式与 Capsule 内容保存在本机，卸载程序时保留。", "caption"));
      Add(body, Button("打开数据目录…", [this] { OpenPath(UserRoot() / "capsule"); }));
      Add(body,
          Label("X11 使用剪贴板所有权事件；Wayland 需要桌面开放 data-control 接口。", "caption"));
      Add(body, Button("管理 Capsule 插件…", [this] { Select("core.plugins"); }));
  }
  void Buffer(GtkWidget *body) {
    auto form = std::make_shared<Form>(
        std::vector<Field>{{"closeAfterLast",
                            "最后一块上屏后关闭工作台",
                            "",
                            FieldKind::Toggle,
                            true,
                            {}},
                           {"resetOnAppSwitch",
                            "切换应用时清空本地缓冲",
                            "保留为跨应用投递而粘贴的内容。",
                            FieldKind::Toggle,
                            false,
                            {}}},
        saved);
    Add(body, form->Widget());
    Add(body, Button("应用修改", [this, form] {
          Try([&] {
            auto next = saved;
            const auto values = form->Values();
            for (auto it = values.begin(); it != values.end(); ++it)
              next[it.key()] = it.value();
            Save(next);
          });
        }));
    auto* spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(spacer, -1, 20); Add(body, spacer);
    Add(body, Label("快捷键", "section"));
    Shortcuts(body);
  }
  static std::string ShortcutTitle(const std::string& chord) {
    if (chord.empty()) return "未设置";
    const auto parsed = ParseShortcut(chord);
    std::string result;
    if (parsed->modifiers & Shortcut::Control) result += "Ctrl ";
    if (parsed->modifiers & Shortcut::Alt) result += "Alt ";
    if (parsed->modifiers & Shortcut::Shift) result += "⇧ ";
    if (parsed->modifiers & Shortcut::Logo) result += "Super ";
    auto key = parsed->key;
    if (key.size() == 1) key[0] = g_ascii_toupper(key[0]);
    return result + key;
  }
  void ShortcutStatus(const std::string& method = "GetStatus") {
    if (rendering || !shortcut_status) return;
    const auto revision = page_revision;
    LauncherCall(method, "", cancel, [this, life = alive, revision](Json status) {
      if (!life->load() || revision != page_revision || !shortcut_status) return;
      const std::map<std::string, std::string> names = {
        {"recording", "正在录入"},
        {"registered", "已绑定"}, {"pending", "等待桌面授权"}, {"conflict", "键位已被占用"},
        {"not-granted", "未获授权"}, {"layout-unavailable", "当前键盘布局不可用"},
        {"unavailable", "尚不可用"}, {"input-method", "仅在输入框中可用"}, {"unbound", "未绑定"}};
      const auto actions = status.value("actions", Json::object());
      std::string summary;
      for (const auto& action : ShortcutActions()) {
        const auto id = action.at("id").get<std::string>();
        if (!actions.contains(id) || !shortcut_buttons.count(id)) continue;
        const auto state = actions[id].value("state", "unavailable");
        const auto label = names.count(state) ? names.at(state) : names.at("unavailable");
        const auto text = action.at("title").get<std::string>() + "：" + label;
        if (!summary.empty()) summary += "；";
        summary += text;
        const auto tip = text + "\n" + actions[id].value("trigger", "") +
          "\n点击后按组合键；Delete 恢复默认，Esc 取消。";
        gtk_widget_set_tooltip_text(shortcut_buttons.at(id), tip.c_str());
        if (recording_action != id && status.value("backend", "") == "portal" && state == "registered") {
          const auto actual = actions[id].value("trigger", "");
          if (!actual.empty()) gtk_button_set_label(GTK_BUTTON(shortcut_buttons.at(id)), actual.c_str());
        }
      }
      if (summary.empty()) summary = status.value("message", "无法获取桌面绑定状态。");
      gtk_label_set_text(GTK_LABEL(shortcut_status), summary.c_str());
      const auto detail = summary + "\n" + status.value("message", "");
      gtk_widget_set_tooltip_text(shortcut_status, detail.c_str());
    });
  }
  void StopShortcutRecording() {
    if (!recording_token.empty() && !rendering)
      LauncherCall("EndRecording", recording_token, nullptr, [](Json) {});
    recording_action.clear(); recording_token.clear(); recording_ready = false;
  }
  void BeginShortcutRecording(const std::string& id) {
    StopShortcutRecording();
    recording_action = id; recording_token = RandomID(); recording_ready = rendering;
    gtk_button_set_label(GTK_BUTTON(shortcut_buttons.at(id)), rendering ? "请按快捷键…" : "正在暂停全局绑定…");
    gtk_widget_grab_focus(shortcut_buttons.at(id));
    if (rendering) return;
    const auto token = recording_token;
    LauncherCall("BeginRecording", token, cancel, [this, life = alive, token](Json status) {
      if (!life->load()) { LauncherCall("EndRecording", token, nullptr, [](Json) {}); return; }
      if (recording_token != token) { LauncherCall("EndRecording", token, nullptr, [](Json) {}); return; }
      if (!status.value("recording", false)) {
        Message(status.value("message", "无法暂停全局快捷键，请启动 Fcitx5 后重试。"));
        Select(route, subpage); return;
      }
      recording_ready = true;
      if (shortcut_buttons.count(recording_action))
        gtk_button_set_label(GTK_BUTTON(shortcut_buttons.at(recording_action)), "请按快捷键…");
    });
  }
  bool RecordShortcut(GdkEventKey* event) {
    if (recording_action.empty()) return false;
    if (event->keyval == GDK_KEY_Escape) { Select(route, subpage); return true; }
    if (!recording_ready) return true;
    if (event->is_modifier) return true;
    const auto id = recording_action;
    Try([&] {
      std::string chord;
      const auto modifiers = event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK | GDK_MOD4_MASK | GDK_SHIFT_MASK);
      if (!modifiers && (event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_Delete)) {
        chord = DefaultShortcuts().at(id);
      } else {
        Shortcut value;
        if (event->state & GDK_CONTROL_MASK) value.modifiers |= Shortcut::Control;
        if (event->state & GDK_MOD1_MASK) value.modifiers |= Shortcut::Alt;
        if (event->state & (GDK_SUPER_MASK | GDK_MOD4_MASK)) value.modifiers |= Shortcut::Logo;
        if (event->state & GDK_SHIFT_MASK) value.modifiers |= Shortcut::Shift;
        guint key = event->keyval;
        // Store the base XKB symbol, even for Shift+1 or Shift+Tab.
        if (event->hardware_keycode)
          gdk_keymap_translate_keyboard_state(gdk_keymap_get_for_display(gtk_widget_get_display(window)),
            event->hardware_keycode, static_cast<GdkModifierType>(0), event->group, &key, nullptr, nullptr, nullptr);
        key = gdk_keyval_to_lower(key);
        const char* name = gdk_keyval_name(key);
        value.key = name ? name : "";
        chord = value.Canonical();
      }
      auto next = saved; next["shortcuts"][id] = chord;
      // Report a local validation failure without exposing persisted values.
      try { ValidateShortcuts(next.at("shortcuts")); }
      catch (const std::exception&) {
        Message("请选择包含 Ctrl、Alt 或 Super 的组合键；不可重复或占用全选、粘贴键。"); return;
      }
      Save(next); Select(route, subpage);
    });
    return true;
  }
  void Shortcuts(GtkWidget* body) {
    auto* rows = Column(Metric("shortcutRowSpacing")); Add(body, rows);
    for (const auto& action : ShortcutActions()) {
      const std::string id = action.at("id"), title = action.at("title");
      auto* card = Class(gtk_event_box_new(), "shortcut-row");
      auto* row = Row(Metric("shortcutControlSpacing"));
      gtk_widget_set_margin_start(row, Metric("shortcutInsetHorizontal")); gtk_widget_set_margin_end(row, Metric("shortcutInsetHorizontal"));
      gtk_widget_set_margin_top(row, Metric("shortcutInsetVertical")); gtk_widget_set_margin_bottom(row, Metric("shortcutInsetVertical"));
      gtk_widget_set_size_request(card, Metric("shortcutRowWidth"), -1);
      auto* title_label = Class(Label(title), "shortcut-title");
      gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
      Add(row, title_label); Add(row, Column(0), true);
      auto* recorder = Button(ShortcutTitle(saved.at("shortcuts").at(id)), [this, id] {
        const bool stop = recording_action == id;
        for (const auto& [other, button] : shortcut_buttons)
          gtk_button_set_label(GTK_BUTTON(button), ShortcutTitle(saved.at("shortcuts").at(other)).c_str());
        if (stop) { StopShortcutRecording(); return; }
        BeginShortcutRecording(id);
      }, "shortcut-recorder");
      gtk_widget_set_size_request(recorder, Metric("shortcutRecorderMinWidth"), -1);
      auto* recorder_label = gtk_bin_get_child(GTK_BIN(recorder));
      gtk_label_set_max_width_chars(GTK_LABEL(recorder_label), 22);
      gtk_label_set_ellipsize(GTK_LABEL(recorder_label), PANGO_ELLIPSIZE_END);
      atk_object_set_name(gtk_widget_get_accessible(recorder), (title + "快捷键").c_str());
      gtk_widget_set_tooltip_text(recorder, "点击后按组合键；Delete 恢复默认，Esc 取消。");
      g_signal_connect(recorder, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* e, gpointer p) -> gboolean {
        return static_cast<Settings*>(p)->RecordShortcut(e);
      }), this);
      g_signal_connect(recorder, "focus-out-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventFocus*, gpointer p) -> gboolean {
        auto* self = static_cast<Settings*>(p);
        const auto found = self->shortcut_buttons.find(self->recording_action);
        if (found != self->shortcut_buttons.end() && found->second == widget) {
          gtk_button_set_label(GTK_BUTTON(widget), ShortcutTitle(self->saved.at("shortcuts").at(found->first)).c_str());
          self->StopShortcutRecording();
        }
        return FALSE;
      }), this);
      shortcut_buttons[id] = recorder;
      Add(row, recorder); gtk_container_add(GTK_CONTAINER(card), row); Add(rows, card);
    }
    Add(body, Button("恢复全部默认快捷键", [this] {
      Try([&] { auto next = saved; next["shortcuts"] = DefaultShortcuts(); Save(next); Select(route, subpage); });
    }));
    auto* global_row = Row(16);
    Add(global_row, Label("在其他输入法中使用全局快捷键", "name"), true);
    auto* global = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(global), saved.at("globalShortcuts"));
    atk_object_set_name(gtk_widget_get_accessible(global), "在其他输入法中使用全局快捷键");
    g_signal_connect(global, "notify::active", G_CALLBACK(+[](GObject* button, GParamSpec*, gpointer data) {
      auto* self = static_cast<Settings*>(data);
      self->Try([&] {
        auto next = self->saved; next["globalShortcuts"] = static_cast<bool>(gtk_switch_get_active(GTK_SWITCH(button)));
        self->Save(next); self->ShortcutStatus();
      });
    }), this);
    Add(global_row, global); Add(body, global_row);
    shortcut_status = Label("正在检查桌面快捷键…", "caption");
    gtk_label_set_ellipsize(GTK_LABEL(shortcut_status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(shortcut_status, 650, -1);
    Add(body, shortcut_status);
    Add(body, Button("重新绑定全局快捷键", [this] { ShortcutStatus("Retry"); }));
    Add(body, Label("若桌面未提供授权，可在系统自定义快捷键中绑定 rimes-launch buffer 或 capsule。", "caption"));
    ShortcutStatus();
  }
};

GtkWidget *FindControl(GtkWidget *root, const std::string &name) {
  const auto *label = atk_object_get_name(gtk_widget_get_accessible(root));
  if (label && name == label && !GTK_IS_LABEL(root))
    return root;
  if (!GTK_IS_CONTAINER(root))
    return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(root));
  GtkWidget *found = nullptr;
  for (auto *item = children; item && !found; item = item->next)
    found = FindControl(GTK_WIDGET(item->data), name);
  g_list_free(children);
  return found;
}

void SettleLayout() {
  // GTK schedules style invalidation and allocation on the next frame clock.
  // An empty immediate event queue does not mean the requested page is laid
  // out.
  auto *loop = g_main_loop_new(nullptr, FALSE);
  g_timeout_add(
      50,
      +[](gpointer p) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop *>(p));
        return G_SOURCE_REMOVE;
      },
      loop);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
}

int SettingsSmoke() {
  char pattern[] = "/tmp/xsime-settings-smoke-XXXXXX";
  if (!mkdtemp(pattern))
    return 1;
  const auto root = std::filesystem::path(pattern);
  setenv("RIMES_USER_DIR", (root / "user").c_str(), 1);
  bool success = false;
  std::string phase = "window";
  try {
    Settings app;
    app.Build(true);
    gtk_widget_show_all(app.window);
    auto control = [&](const char *name) {
      auto *widget = FindControl(app.root, name);
      if (!widget)
        throw std::runtime_error("Missing control");
      return widget;
    };
    auto click = [&](const char *name) {
      gtk_button_clicked(GTK_BUTTON(control(name)));
      while (gtk_events_pending())
        gtk_main_iteration();
    };
    auto check = [](bool value) {
      if (!value)
        throw std::runtime_error("UI state mismatch");
    };
    phase = "buffer";
    app.Select("core.buffer");
    gtk_switch_set_active(GTK_SWITCH(control("最后一块上屏后关闭工作台")),
                          false);
    click("应用修改");
    check(app.config.Load().at("closeAfterLast") == false);
    check(app.config.Load().at("resetOnAppSwitch") == false);
    phase = "shortcuts";
    app.recording_action = "buffer";
    app.recording_ready = true;
    GdkEventKey event{}; event.keyval = GDK_KEY_j; event.state = GDK_CONTROL_MASK | GDK_MOD1_MASK;
    check(app.RecordShortcut(&event));
    check(app.config.Load().at("shortcuts").at("buffer") == "CTRL+ALT+j");
    app.recording_action = "capsule";
    app.recording_ready = true;
    const auto before_conflict = app.config.Load();
    check(app.RecordShortcut(&event)); check(app.config.Load() == before_conflict);
    event.keyval = GDK_KEY_Escape; app.RecordShortcut(&event);
    check(app.recording_action.empty());
    app.recording_action = "buffer"; app.recording_ready = true;
    event.keyval = GDK_KEY_Delete; event.state = 0; app.RecordShortcut(&event);
    check(app.config.Load().at("shortcuts").at("buffer") == DefaultShortcuts().at("buffer"));
    gtk_switch_set_active(GTK_SWITCH(control("在其他输入法中使用全局快捷键")), false);
    check(app.config.Load().at("globalShortcuts") == false);
    click("恢复全部默认快捷键");
    check(app.config.Load().at("shortcuts") == DefaultShortcuts());
    phase = "theme";
    app.Select("core.appearance", "theme");
    click("翡翠");
    check(app.config.Load().at("theme") == "day");
    phase = "width";
    app.Select("core.appearance", "size");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control("工作台宽度")), 920);
    click("应用修改");
    check(app.config.Load().at("bufferWidth") == 920);
    phase = "provider";
    app.Select("core.connectors", "ai-model");
    click("新增");
    gtk_entry_set_text(GTK_ENTRY(control("API 地址")),
                       "http://127.0.0.1:9988/v1");
    gtk_entry_set_text(GTK_ENTRY(control("模型")), "fixture-model");
    gtk_entry_set_text(GTK_ENTRY(control("API Key")),
                       "fixture-key-not-for-output");
    click("保存");
    check(
        std::string(gtk_entry_get_text(GTK_ENTRY(control("API Key")))).empty());
    check(app.config.Freeze("builtin.openai-compatible").credential ==
          "fixture-key-not-for-output");
    check(app.config.Load().dump().find("fixture-key-not-for-output") ==
          std::string::npos);
    const auto before = app.config.Load();
    gtk_entry_set_text(GTK_ENTRY(control("API 地址")),
                       "https://username:secret@example.org/v1");
    click("保存");
    check(app.config.Load() == before);
    check(app.message.find("secret") == std::string::npos);
    phase = "plugins";
    app.Select("core.plugins", "buffer-plugins");
    for (const auto &package : app.packages.Entries()) {
      if (package.kind == "buffer" && !package.available) {
        check(!gtk_widget_get_sensitive(
            control(("安装" + package.name).c_str())));
      }
    }
    phase = "layout";
    app.Select("core.appearance", "theme");
    gtk_window_resize(GTK_WINDOW(app.window), Settings::Metric("settingsWidth"),
                      Settings::Metric("settingsHeight"));
    SettleLayout();
    const int width = gtk_widget_get_allocated_width(app.root);
    const int height = gtk_widget_get_allocated_height(app.root);
    if (width != Settings::Metric("settingsWidth") ||
        height != Settings::Metric("settingsHeight")) {
      std::cerr << "Settings allocation: " << width << 'x' << height << '\n';
      check(false);
    }
    gtk_widget_destroy(app.window);
    ConfigurationStore reopened(root / "user" / "configuration");
    check(reopened.Load().at("theme") == "day");
    check(reopened.Load().at("bufferWidth") == 920);
    success = true;
  } catch (const std::exception &) {
    std::cerr << "FAILED: settings UI " << phase << " check\n";
  }
  std::filesystem::remove_all(root);
  if (success)
    std::cout << "Settings UI saves, validation, credential redaction, restart "
                 "and native dimensions passed\n";
  return success ? 0 : 1;
}
} // namespace
int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "Usage: rimes-settings [--render DIRECTORY | --smoke]\n";
    return 0;
  }
  try {
    const bool smoke = argc == 2 && std::string_view(argv[1]) == "--smoke";
    const bool render = argc == 3 && std::string_view(argv[1]) == "--render";
    if (argc != 1 && !render && !smoke)
      return 2;
    const std::filesystem::path output = render ? argv[2] : "";
    if (render && !std::getenv("RIMES_USER_DIR")) {
      std::cerr << "渲染请指定独立的 RIMES_USER_DIR 测试目录。\n";
      return 2;
    }
    if (!gtk_init_check(&argc, &argv)) {
      std::cerr << "XSIME 设置需要图形桌面会话。\n";
      return 1;
    }
    if (smoke)
      return SettingsSmoke();
    if (!render) {
      struct Session { std::unique_ptr<Settings> settings; bool failed = false; } session;
      // Isolate test/user roots; normal launches in the same desktop session
      // share one settings window, including activations from the IME.
      const auto identity = "org.scholay.rimes.settings.u" + Sha256(UserRoot().string()).substr(0, 16);
      auto *application = gtk_application_new(identity.c_str(), static_cast<GApplicationFlags>(0));
      g_signal_connect(application, "activate", G_CALLBACK(+[](GtkApplication *application, gpointer data) {
        auto *session = static_cast<Session *>(data);
        try {
          if (!session->settings || !session->settings->window) {
            session->settings = std::make_unique<Settings>();
            session->settings->Build(false, application);
          }
          gtk_widget_show_all(session->settings->window);
          gtk_window_present(GTK_WINDOW(session->settings->window));
        } catch (const std::exception &) {
          session->failed = true;
          g_printerr("XSIME 设置无法启动；现有配置已保留。\n");
          g_application_quit(G_APPLICATION(application));
        }
      }), &session);
      const int result = g_application_run(G_APPLICATION(application), argc, argv);
      session.settings.reset();
      g_object_unref(application);
      return session.failed ? 1 : result;
    }
    Settings app;
    app.Build(render);
    if (render) {
      std::filesystem::create_directories(output);
      gtk_widget_show_all(app.window);
      Json manifest = Json::array();
      for (const auto &route : Visuals().at("routes"))
        for (const auto &page : route.at("subpages")) {
          app.Select(route.at("id"), page[0]);
          gtk_window_resize(GTK_WINDOW(app.window), 980, 680);
          SettleLayout();
          const std::string name = route.at("id").get<std::string>() + "--" +
                                   page[0].get<std::string>() + ".png";
          if (!Render(app.root, (output / name).string()))
            throw std::runtime_error("Render failed");
          manifest.push_back({{"routeID", route.at("id")},
                              {"subpageID", page[0]},
                              {"file", name}});
        }
      std::ofstream file(output / "manifest.json");
      file << manifest.dump(2) << '\n';
      return file ? 0 : 1;
    }
    return 0;
  } catch (const std::exception &) {
    std::cerr
        << "XSIME 设置无法启动，请检查私有配置的格式和权限；现有文件已保留。\n";
    return 1;
  }
}
