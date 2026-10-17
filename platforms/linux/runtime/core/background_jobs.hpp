#pragma once
#include <atomic>
#include <array>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rimes::runtime {
// A fixed worker pool with a bounded live queue. Cancelled jobs may still be
// cleaning up a process group; they cannot reject the next live request or
// cause unbounded replacement threads. An accepted but discarded job invokes
// its optional cancellation completion exactly once, outside the pool lock.
class BackgroundJobs {
public:
    ~BackgroundJobs();
    bool Run(std::shared_ptr<std::atomic<bool>> cancelled, std::function<void()> work,
             std::function<void()> discarded = {});
    void CancelAll();
    void Shutdown();
private:
    struct Job { std::shared_ptr<std::atomic<bool>> cancelled; std::function<void()> work, discarded; };
    static void Discard(std::list<Job> jobs);
    void Loop(std::size_t index);
    static constexpr std::size_t kLimit = 4;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::list<Job> pending_;
    std::array<std::shared_ptr<std::atomic<bool>>, kLimit> active_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};
}
