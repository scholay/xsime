#pragma once
#include "private_files.hpp"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace rimes::runtime {
inline void CheckUpdateCancelled(const std::atomic<bool>& cancelled) {
    if (cancelled.load()) throw std::runtime_error("Update cancelled");
}
inline FileLock WaitForUpdateStorage(const PrivateDirectory& directory, const std::atomic<bool>& cancelled) {
    for (;;) {
        CheckUpdateCancelled(cancelled);
        try {
            auto lock = directory.Lock(false);
            CheckUpdateCancelled(cancelled);
            return lock;
        } catch (const StorageBusy&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}
} // namespace rimes::runtime
