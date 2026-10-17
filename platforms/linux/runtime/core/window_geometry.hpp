#pragma once
#include "private_files.hpp"
#include <optional>
#include <vector>

namespace rimes::runtime {
struct WindowRect { int x = 0, y = 0, width = 0, height = 0; };
struct MonitorArea { std::string id; WindowRect work; };
struct WindowPosition {
    std::string monitor;
    double center_x = 0.5, bottom = 0;
};
WindowRect ConstrainWindow(WindowRect frame, const WindowRect& work, int margin = 8);
WindowPosition RememberWindow(const WindowRect& frame, const MonitorArea& monitor);
WindowRect RestoreWindow(const WindowPosition& position, const WindowRect& work, int width, int height);
WindowRect BottomWindow(const WindowRect& work, int width, int height, int bottom);
std::size_t BestMonitor(const std::vector<MonitorArea>& monitors, const WindowRect& frame);

// Geometry has its own private file, independent of request configuration.
// Moving a window must never change an AI/translation configuration revision.
class WindowPositionStore {
public:
    WindowPositionStore(const std::filesystem::path& root, std::string name);
    std::optional<WindowPosition> Load() const;
    void Save(const WindowPosition& position) const;
private:
    PrivateDirectory files_;
    std::string name_;
};
} // namespace rimes::runtime
