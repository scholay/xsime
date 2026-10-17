#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
namespace rimes::capsule {
struct CaptureControl {
    std::atomic<bool> cancel{false}, stop{false};
    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex);
        cancel.store(true);
    }
    bool Commit(const std::function<void()>& action) {
        std::lock_guard<std::mutex> lock(mutex);
        if (cancel.load())
            return false;
        action();
        return true;
    }

  private:
    std::mutex mutex;
};
// Blocking worker API. The child receives only a private destination and owns
// its portal / encoder. Every exit joins the child and its process group.
nlohmann::json RunCapture(const std::filesystem::path& output, bool video, bool portal,
                          const CaptureControl& control,
                          const std::function<void(const nlohmann::json&)>& event);
} // namespace rimes::capsule
