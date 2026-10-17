#include "window_geometry.hpp"
#include "configuration.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
using namespace rimes::runtime;
void Check(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
int main() {
    char pattern[] = "/tmp/xsime-window-XXXXXX";
    const auto* directory = mkdtemp(pattern); if (!directory) return 1;
    const std::filesystem::path root(directory);
    try {
        const MonitorArea left{"left", {-1920, 24, 1920, 1056}}, right{"right", {0, 0, 1440, 900}};
        const WindowRect frame{-1400, 700, 760, 73};
        Check(BestMonitor({left, right}, frame) == 0 && BestMonitor({left, right}, {1200, 100, 760, 73}) == 1, "monitor intersection");
        const auto saved = RememberWindow(frame, left);
        const auto restored = RestoreWindow(saved, left.work, 760, 105);
        Check(restored.x == -1400 && restored.y == 668, "dual rail must preserve bottom edge");
        const auto scaled = RestoreWindow(saved, {-960, 12, 960, 528}, 520, 105);
        Check(scaled.x >= -952 && scaled.x + scaled.width <= -8 && scaled.y >= 20 && scaled.y + scaled.height <= 532, "scaled display clamp");
        const auto disconnected = RestoreWindow(saved, right.work, 760, 73);
        Check(disconnected.x >= 8 && disconnected.x + disconnected.width <= 1432, "removed display recovery");
        const auto tiny = ConstrainWindow({-1000, 2000, 760, 105}, {0, 0, 100, 40});
        Check(tiny.x == 8 && tiny.y == 8 && tiny.width == 84 && tiny.height == 24, "small workarea clamp");
        WindowPositionStore store(root / "windows", "buffer");
        ConfigurationStore config(root / "configuration");
        const auto before = config.Load();
        Check(!store.Load(), "new profile position");
        store.Save(saved);
        Check(store.Load()->monitor == "left" && config.Load() == before, "position changed request configuration");
        struct stat mode{}; Check(stat((root / "windows/buffer.json").c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600, "private geometry mode");
        bool rejected = false;
        try { store.Save({"left", std::nan(""), 0}); } catch (const std::exception&) { rejected = true; }
        Check(rejected, "non-finite position accepted");
        PrivateDirectory files(root / "windows"); files.Write("buffer.json", R"({"schemaVersion":1,"monitor":"left","centerX":2,"bottom":0})");
        rejected = false; try { (void)store.Load(); } catch (const std::exception&) { rejected = true; }
        Check(rejected && files.Read("buffer.json", 2048)->find("centerX") != std::string::npos, "malformed position overwritten");
        std::filesystem::remove(root / "windows/buffer.json");
        std::filesystem::create_symlink(root / "outside", root / "windows/buffer.json");
        rejected = false; try { store.Save(saved); } catch (const std::exception&) { rejected = true; }
        Check(rejected && !std::filesystem::exists(root / "outside"), "position symlink followed");
        std::cout << "Window geometry: negative origins, scaling, unplug, dual-rail resize, small workarea and private independent storage passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::filesystem::remove_all(root); return 1; }
    std::filesystem::remove_all(root);
}
