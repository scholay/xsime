#include "background_jobs.hpp"
#include <algorithm>

namespace rimes::runtime {
BackgroundJobs::~BackgroundJobs() { Shutdown(); }
bool BackgroundJobs::Run(std::shared_ptr<std::atomic<bool>> cancelled, std::function<void()> work,
                         std::function<void()> discarded) {
    std::list<Job> retired;
    std::unique_lock lock(mutex_);
    if (stopping_ || !cancelled || cancelled->load() || !work) return false;
    for(auto it=pending_.begin();it!=pending_.end();) {
        auto current=it++;
        if(current->cancelled->load()) retired.splice(retired.end(),pending_,current);
    }
    const auto active = std::count_if(active_.begin(), active_.end(), [](const auto& flag) { return flag && !flag->load(); });
    if (pending_.size() + active >= kLimit) { lock.unlock(); Discard(std::move(retired)); return false; }
    workers_.reserve(kLimit);
    while (workers_.size() < kLimit) workers_.emplace_back([this, index = workers_.size()] { Loop(index); });
    pending_.push_back({std::move(cancelled), std::move(work), std::move(discarded)});
    changed_.notify_one();
    lock.unlock(); Discard(std::move(retired));
    return true;
}
void BackgroundJobs::Discard(std::list<Job> jobs) {
    for(auto& job:jobs) if(job.discarded) { try { job.discarded(); } catch(...) {} }
}
void BackgroundJobs::CancelAll() {
    std::list<Job> retired;
    {
        std::lock_guard lock(mutex_);
        for (auto& job : pending_) job.cancelled->store(true);
        for (auto& flag : active_) if (flag) flag->store(true);
        retired.swap(pending_);
    }
    Discard(std::move(retired));
}
void BackgroundJobs::Shutdown() {
    std::list<Job> retired;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& job : pending_) job.cancelled->store(true);
        for (auto& flag : active_) if (flag) flag->store(true);
        retired.swap(pending_);
    }
    changed_.notify_all();
    Discard(std::move(retired));
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    workers_.clear();
}
void BackgroundJobs::Loop(std::size_t index) {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_) return;
            job = std::move(pending_.front()); pending_.pop_front();
            active_[index] = job.cancelled;
        }
        // A caller's completion must not terminate an input-method process or
        // consume a worker permanently if it unexpectedly throws.
        try {
            if(job.cancelled->load()) { if(job.discarded) job.discarded(); }
            else job.work();
        } catch (...) {}
        { std::lock_guard lock(mutex_); active_[index].reset(); }
    }
}
}
