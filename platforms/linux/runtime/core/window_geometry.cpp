#include "window_geometry.hpp"
#include "plugin_store.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rimes::runtime {
WindowRect ConstrainWindow(WindowRect frame, const WindowRect& work, int margin) {
    const int horizontal = std::min(std::max(0, margin), std::max(0, (work.width - 1) / 2));
    const int vertical = std::min(std::max(0, margin), std::max(0, (work.height - 1) / 2));
    frame.width = std::clamp(frame.width, 1, std::max(1, work.width - 2 * horizontal));
    frame.height = std::clamp(frame.height, 1, std::max(1, work.height - 2 * vertical));
    frame.x = std::clamp(frame.x, work.x + horizontal, work.x + std::max(horizontal, work.width - horizontal - frame.width));
    frame.y = std::clamp(frame.y, work.y + vertical, work.y + std::max(vertical, work.height - vertical - frame.height));
    return frame;
}
WindowPosition RememberWindow(const WindowRect& frame, const MonitorArea& monitor) {
    const auto placed = ConstrainWindow(frame, monitor.work);
    return {monitor.id,
        std::clamp((placed.x - monitor.work.x + placed.width / 2.0) / std::max(1, monitor.work.width), 0.0, 1.0),
        std::clamp((monitor.work.y + monitor.work.height - placed.y - placed.height) / static_cast<double>(std::max(1, monitor.work.height)), 0.0, 1.0)};
}
WindowRect RestoreWindow(const WindowPosition& position, const WindowRect& work, int width, int height) {
    return ConstrainWindow({work.x + static_cast<int>(std::lround(work.width * position.center_x - width / 2.0)),
        work.y + static_cast<int>(std::lround(work.height * (1.0 - position.bottom))) - height, width, height}, work);
}
WindowRect BottomWindow(const WindowRect& work, int width, int height, int bottom) {
    return ConstrainWindow({work.x + (work.width - width) / 2, work.y + work.height - height - std::max(8, bottom), width, height}, work);
}
std::size_t BestMonitor(const std::vector<MonitorArea>& monitors, const WindowRect& frame) {
    std::size_t selected = 0;
    std::int64_t best = -1;
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        const auto& work = monitors[i].work;
        const auto width = std::max(0, std::min(work.x + work.width, frame.x + frame.width) - std::max(work.x, frame.x));
        const auto height = std::max(0, std::min(work.y + work.height, frame.y + frame.height) - std::max(work.y, frame.y));
        const auto area = static_cast<std::int64_t>(width) * height;
        if (area > best) { best = area; selected = i; }
    }
    return selected;
}
WindowPositionStore::WindowPositionStore(const std::filesystem::path& root, std::string name)
    : files_(root), name_(std::move(name)) {
    if (name_.empty() || name_.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != name_.npos)
        throw std::runtime_error("Invalid window identity");
    name_ += ".json";
}
std::optional<WindowPosition> WindowPositionStore::Load() const {
    const auto bytes = files_.Read(name_, 2048);
    if (!bytes) return {};
    const auto value = plugins::Json::parse(*bytes);
    if (!value.is_object() || value.size() != 4 || value.at("schemaVersion") != 1 ||
        !value.at("monitor").is_string() || !value.at("centerX").is_number() || !value.at("bottom").is_number())
        throw std::runtime_error("Invalid window position");
    WindowPosition position{value.at("monitor"), value.at("centerX"), value.at("bottom")};
    if (position.monitor.empty() || position.monitor.size() > 256 || position.monitor.find('\0') != position.monitor.npos ||
        !std::isfinite(position.center_x) || !std::isfinite(position.bottom) ||
        position.center_x < 0 || position.center_x > 1 || position.bottom < 0 || position.bottom > 1)
        throw std::runtime_error("Invalid window coordinates");
    return position;
}
void WindowPositionStore::Save(const WindowPosition& position) const {
    if (position.monitor.empty() || position.monitor.size() > 256 || position.monitor.find('\0') != position.monitor.npos ||
        !std::isfinite(position.center_x) || !std::isfinite(position.bottom) ||
        position.center_x < 0 || position.center_x > 1 || position.bottom < 0 || position.bottom > 1)
        throw std::runtime_error("Invalid window coordinates");
    auto lock = files_.Lock();
    files_.Write(name_, plugins::Json{{"schemaVersion",1},{"monitor",position.monitor},
                                    {"centerX",position.center_x},{"bottom",position.bottom}}.dump());
}
} // namespace rimes::runtime
