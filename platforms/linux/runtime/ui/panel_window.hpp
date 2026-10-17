#pragma once
#include "window_geometry.hpp"
#include <functional>
#include <gtk/gtk.h>
#include <memory>

namespace rimes::desktop {
// A passive desktop surface. It never presents/activates a client, changes an
// input source, or acquires delivery authority from its screen coordinates.
class PanelWindow {
public:
    using Rect = runtime::WindowRect;
    using Placement = std::function<Rect(const Rect& work, const Rect& caret)>;
    PanelWindow(GtkWindow* window, std::string identity, int bottom_offset, bool persist = true);
    ~PanelWindow();
    PanelWindow(const PanelWindow&) = delete;
    PanelWindow& operator=(const PanelWindow&) = delete;
    Rect Place(int width, int height, std::optional<Rect> physical_caret = {}, bool opening = false, Placement placement = {});
    bool Layer() const { return layer_; }
    bool CanPosition() const { return layer_ || x11_; }
    std::string Backend() const;
    std::vector<std::string> MonitorNames() const;
    void SelectMonitor(std::size_t index);
    Rect Frame() const;
    void Move(Rect frame);
    void Remember();
    void Reset();
private:
    struct Monitor { GdkMonitor* native; runtime::MonitorArea area; Rect bounds; };
    std::vector<Monitor> Monitors() const;
    void QueueReposition();
    void WatchMonitors();
    void Apply(Rect frame, const Monitor& monitor);
    GtkWindow* window_;
    GdkDisplay* fallback_display_ = nullptr;
    GdkDisplay* display_ = nullptr;
    GdkScreen* screen_;
    std::vector<GdkMonitor*> watched_monitors_;
    bool layer_ = false, x11_ = false, destroyed_ = false, positioned_ = false;
    bool topology_changed_ = false;
    int bottom_, width_ = 760, height_ = 73;
    guint pending_ = 0;
    Rect frame_;
    Rect work_;
    std::string monitor_id_;
    std::optional<Rect> caret_;
    Placement placement_;
    std::optional<runtime::WindowPosition> saved_;
    std::unique_ptr<runtime::WindowPositionStore> store_;
};
} // namespace rimes::desktop
