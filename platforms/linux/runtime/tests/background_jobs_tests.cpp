#include "background_jobs.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace rimes::runtime;
void Check(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
int main() {
    BackgroundJobs jobs;
    std::mutex mutex;
    std::condition_variable changed;
    int started = 0, finished = 0, obsolete = 0, cancelled_completions = 0;
    bool release = false, latest = false;
    auto cancel = [] { return std::make_shared<std::atomic<bool>>(false); };
    try {
        // A cancellation acknowledgement and process-group cleanup are
        // distinct. Hold all four workers in cleanup while retrying quickly.
        std::vector<std::shared_ptr<std::atomic<bool>>> old;
        for (int i = 0; i < 4; ++i) {
            old.push_back(cancel());
            Check(jobs.Run(old.back(), [&] {
                std::unique_lock lock(mutex); ++started; changed.notify_all();
                changed.wait(lock, [&] { return release; });
                ++finished; changed.notify_all();
            }), "initial job rejected");
        }
        {
            std::unique_lock lock(mutex);
            Check(changed.wait_for(lock, std::chrono::seconds(3), [&] { return started == 4; }), "workers did not start");
        }
        Check(!jobs.Run(cancel(), [] {}), "live request bound was not enforced");
        jobs.CancelAll();
        for (const auto& flag : old) Check(flag->load(), "active process cancellation missing");
        for (int i = 0; i < 100; ++i) {
            auto flag = cancel();
            Check(jobs.Run(flag, [&] { std::lock_guard lock(mutex); ++obsolete; },
                [&] { std::lock_guard lock(mutex); ++cancelled_completions; }), "cleanup blocked replacement request");
            flag->store(true);
        }
        Check(jobs.Run(cancel(), [&] { std::lock_guard lock(mutex); latest = true; changed.notify_all(); }), "latest request rejected");
        {
            std::unique_lock lock(mutex);
            Check(!latest && !obsolete && started == 4, "cancel-and-retry created unbounded workers");
            release = true; changed.notify_all();
            Check(changed.wait_for(lock, std::chrono::seconds(3), [&] { return latest && finished == 4; }), "queued latest request never ran");
            Check(obsolete == 0, "cancelled queued work executed");
            Check(cancelled_completions == 100, "cancelled queued requests lost or duplicated completion");
        }
        jobs.Shutdown();
        Check(!jobs.Run(cancel(), [] {}), "shutdown admitted new work");
        std::cout << "Bounded jobs: live capacity, 100 rapid cancellations, pending replacement and shutdown passed\n";
    } catch (const std::exception& error) {
        { std::lock_guard lock(mutex); release = true; changed.notify_all(); }
        jobs.Shutdown();
        std::cerr << error.what() << '\n'; return 1;
    }
}
