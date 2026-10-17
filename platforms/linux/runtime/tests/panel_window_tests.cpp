#include "panel_window.hpp"
#include <gdk/gdkx.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace rimes::desktop;
void Check(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
void Settle() {
    for (int i = 0; i < 10; ++i) {
        while (gtk_events_pending()) gtk_main_iteration();
        gdk_display_sync(gdk_display_get_default());
        g_usleep(10000);
    }
}
int main(int argc, char** argv) {
    char pattern[] = "/tmp/xsime-panel-XXXXXX";
    const auto* directory = mkdtemp(pattern); if (!directory) return 1;
    setenv("RIMES_USER_DIR", directory, 1);
    try {
        Check(gtk_init_check(&argc, &argv), "display unavailable");
        auto* window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
        gtk_window_set_decorated(window, FALSE);
        {
            PanelWindow panel(window, "fixture", 120);
            const bool x11 = GDK_IS_X11_DISPLAY(gtk_widget_get_display(GTK_WIDGET(window)));
            Check(!gtk_window_get_accept_focus(window) && !gtk_window_get_focus_on_map(window), "panel may steal focus");
            int seen_x = -1;
            const auto placement = [&](const auto&, const auto& caret) {
                seen_x = caret.x;
                return PanelWindow::Rect{caret.x - 380, caret.y - 83, 760, 73};
            };
            panel.Place(760, 73, PanelWindow::Rect{800, 600, 2, 20}, true, placement);
            gtk_widget_show(GTK_WIDGET(window)); Settle();
            auto frame = panel.Frame();
            Check(frame.width == 760 && frame.height == 73, "fixed panel size stretched");
            Check(gtk_widget_get_allocated_width(GTK_WIDGET(window)) == 760 &&
                  gtk_widget_get_allocated_height(GTK_WIDGET(window)) == 73, "compositor stretched actual panel allocation");
            if (x11) Check(seen_x == 800 / gtk_widget_get_scale_factor(GTK_WIDGET(window)), "physical caret was not converted to application pixels");
            else Check(seen_x == -1, "Wayland cursor was treated as global");
            panel.Move({100, 220, 760, 73}); Settle(); panel.Remember();
            frame = panel.Frame();
            panel.Place(760, 105); Settle();
            const auto taller = panel.Frame();
            Check(taller.width == frame.width && taller.y + taller.height == frame.y + frame.height, "panel resize moved its bottom edge");
            panel.SelectMonitor(0); Settle();
            Check(panel.Frame().width == 760 && !panel.MonitorNames().empty(), "monitor placement stretched panel");
            if (panel.MonitorNames().size() > 1) {
                panel.SelectMonitor(panel.MonitorNames().size() - 1); Settle();
                const auto monitors = gdk_display_get_n_monitors(gtk_widget_get_display(GTK_WIDGET(window)));
                auto* monitor = gdk_display_get_monitor(gtk_widget_get_display(GTK_WIDGET(window)), monitors - 1);
                GdkRectangle work{}; gdk_monitor_get_workarea(monitor, &work);
                Check(panel.Frame().x >= work.x && panel.Frame().x + panel.Frame().width <= work.x + work.width,
                      "move-to-monitor was not clamped to requested display");
                Check(gtk_widget_get_allocated_width(GTK_WIDGET(window)) == 760, "destination monitor stretched panel");
                if (!x11 && g_getenv("RIMES_PANEL_TEST_SWAY")) {
                    gint status = -1;
                    Check(g_spawn_command_line_sync("swaymsg output HEADLESS-2 disable", nullptr, nullptr, &status, nullptr) && !status, "fixture monitor disable");
                    Settle();
                    Check(panel.MonitorNames().size() == 1, "removed output remained in monitor inventory");
                    auto* remaining = gdk_display_get_monitor(gtk_widget_get_display(GTK_WIDGET(window)), 0);
                    gdk_monitor_get_workarea(remaining, &work);
                    Check(gtk_widget_get_visible(GTK_WIDGET(window)) && gtk_widget_get_mapped(GTK_WIDGET(window)),
                          "output removal closed the passive panel");
                    Check(panel.Frame().x >= work.x && panel.Frame().x + panel.Frame().width <= work.x + work.width,
                          "panel was stranded on removed monitor");
                    Check(g_spawn_command_line_sync("swaymsg output HEADLESS-2 enable", nullptr, nullptr, &status, nullptr) && !status, "fixture monitor reconnect");
                    Settle();
                }
            }
            gtk_widget_destroy(GTK_WIDGET(window));
        }
        auto* displays = gdk_display_manager_list_displays(gdk_display_manager_get());
        Check(g_slist_length(displays) <= 2, "panel recreation leaked display connections");
        g_slist_free(displays);
        auto* restored = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
        gtk_window_set_decorated(restored, FALSE);
        {
            PanelWindow panel(restored, "fixture", 120);
            panel.Place(760, 73, {}, true); gtk_widget_show(GTK_WIDGET(restored)); Settle();
            Check(panel.Frame().width == 760, "restored panel size");
            std::cout << "Panel window: " << panel.Backend() << ", fixed width, non-focus hints, scale, resize and restoration passed\n";
            gtk_widget_destroy(GTK_WIDGET(restored));
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::filesystem::remove_all(directory); return 1; }
    std::filesystem::remove_all(directory);
}
