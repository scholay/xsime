#pragma once
#include "buffer_model.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>

namespace rimes::buffer {
// Same monotonic-clock contract as macOS BufferAutoSendClock. Ages belong to
// exact blocks; hidden time and time before a block existed never count.
class AutoSendClock {
    struct Entry { std::string text; double age = 0; std::optional<double> last; };
    std::string workspace_;
    std::map<std::string, Entry> entries_;
    std::vector<std::string> order_;
public:
    void Reset() { workspace_.clear(); entries_.clear(); order_.clear(); }
    void Pause() { for (auto& [id, entry] : entries_) entry.last.reset(); }
    void Synchronize(const std::string& workspace, const std::vector<Block>& blocks) {
        if (workspace != workspace_) { Reset(); workspace_ = workspace; }
        std::set<std::string> live;
        order_.clear();
        for (const auto& block : blocks) {
            live.insert(block.id); order_.push_back(block.id);
            auto it = entries_.find(block.id);
            if (it == entries_.end() || it->second.text != block.text)
                entries_[block.id] = {block.text, 0, {}};
        }
        for (auto it = entries_.begin(); it != entries_.end();)
            if (!live.count(it->first)) it = entries_.erase(it); else ++it;
    }
    std::optional<std::string> Tick(double uptime, bool can_age, double lifetime) {
        if (!can_age || !std::isfinite(uptime) || !std::isfinite(lifetime) || lifetime <= 0) {
            Pause(); return {};
        }
        for (auto& [id, entry] : entries_) {
            const auto elapsed = entry.last ? uptime - *entry.last : 0;
            entry.age = std::min(lifetime, entry.age + std::clamp(elapsed, 0.0, lifetime));
            entry.last = uptime;
        }
        if (!order_.empty() && entries_.at(order_.front()).age >= lifetime) return order_.front();
        return {};
    }
    double Age(const std::string& id) const {
        const auto it = entries_.find(id); return it == entries_.end() ? 0 : it->second.age;
    }
};
} // namespace rimes::buffer
