#include "derived_workspace.hpp"
#include "buffer_ids.hpp"
#include <algorithm>

namespace rimes::buffer {
namespace {
bool SameSource(const std::vector<Block> &a, const std::vector<Block> &b) {
  return a.size() == b.size() &&
         std::equal(
             a.begin(), a.end(), b.begin(), [](const Block &x, const Block &y) {
               return x.id == y.id && x.text == y.text && x.origin == y.origin;
             });
}
bool Utf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < 0x80) {
      if (!first)
        return false;
      continue;
    }
    unsigned count = 0, value = 0, minimum = 0;
    if ((first & 0xE0) == 0xC0) {
      count = 1;
      value = first & 0x1F;
      minimum = 0x80;
    } else if ((first & 0xF0) == 0xE0) {
      count = 2;
      value = first & 0x0F;
      minimum = 0x800;
    } else if ((first & 0xF8) == 0xF0) {
      count = 3;
      value = first & 7;
      minimum = 0x10000;
    } else
      return false;
    if (count > text.size() - i)
      return false;
    while (count--) {
      const auto byte = static_cast<unsigned char>(text[i++]);
      if ((byte & 0xC0) != 0x80)
        return false;
      value = (value << 6) | (byte & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF))
      return false;
  }
  return true;
}
} // namespace
DerivedWorkspace::DerivedWorkspace() : lifetime_(NewBlockId()) {}
void DerivedWorkspace::Invalidate() {
  active_request_.clear();
  active_lease_ = {};
  results_.clear();
  selected_.clear();
  phase_ = protected_ ? DerivedPhase::Protected : DerivedPhase::Idle;
}
void DerivedWorkspace::Synchronize(const std::vector<Block> &source,
                                   std::string plugin,
                                   std::string configuration,
                                   FocusIdentity focus, bool visible,
                                   bool protected_state) {
  const bool changed = !SameSource(source_, source);
  const bool invalid = changed || plugin_ != plugin ||
                       configuration_ != configuration || !(focus_ == focus) ||
                       visible_ != visible || protected_ != protected_state;
  if (changed) {
    source_ = source;
    ++source_revision_;
  }
  plugin_ = std::move(plugin);
  configuration_ = std::move(configuration);
  focus_ = std::move(focus);
  visible_ = visible;
  protected_ = protected_state;
  if (invalid)
    Invalidate();
}
bool DerivedWorkspace::CanGenerate() const {
  if (!visible_ || protected_ || plugin_.empty() || configuration_.empty() ||
      source_.empty() || phase_ == DerivedPhase::Running)
    return false;
  std::size_t size = 0;
  for (const auto &b : source_) {
    if (b.id.empty() || !Utf8(b.text) || b.text.size() > kSourceLimit - size)
      return false;
    size += b.text.size();
  }
  return size != 0;
}
std::optional<DerivedRequest>
DerivedWorkspace::Begin(const plugins::Lease &lease) {
  if (!CanGenerate() || lease.plugin_id != plugin_ || lease.registry.empty() ||
      lease.capability.empty())
    return std::nullopt;
  Invalidate();
  active_request_ = NewBlockId();
  active_lease_ = lease;
  phase_ = DerivedPhase::Running;
  DerivedRequest request{
      lifetime_, active_request_, configuration_, source_revision_,
      lease,     focus_,          source_,        {}};
  for (const auto &b : source_)
    request.text += b.text;
  return request;
}
bool DerivedWorkspace::Accepts(const DerivedRequest &r,
                               bool grant_valid) const {
  return phase_ == DerivedPhase::Running && Owns(r, grant_valid);
}
bool DerivedWorkspace::Owns(const DerivedRequest &r, bool grant_valid) const {
  return grant_valid && visible_ && !protected_ &&
         (phase_ == DerivedPhase::Running || phase_ == DerivedPhase::Ready) && !active_request_.empty() &&
         r.id == active_request_ && r.lifetime == lifetime_ &&
         r.source_revision == source_revision_ &&
         r.configuration == configuration_ && r.lease.plugin_id == plugin_ &&
         r.lease.registry == active_lease_.registry &&
         r.lease.generation == active_lease_.generation &&
         r.lease.capability == active_lease_.capability &&
         r.focus == focus_ && SameSource(r.source, source_);
}
bool DerivedWorkspace::Update(const DerivedRequest &r, bool grant_valid,
                              unsigned alternative, std::string text) {
  if (!Accepts(r, grant_valid) || alternative >= 5 || !Utf8(text) ||
      text.size() > kResultLimit)
    return false;
  auto size = text.size();
  for (const auto &item : results_)
    if (item.alternative != alternative) {
      if (item.text.size() > kResultLimit - size)
        return false;
      size += item.text.size();
    }
  const auto found =
      std::find_if(results_.begin(), results_.end(), [&](const auto &item) {
        return item.alternative == alternative;
      });
  if (found != results_.end())
    found->text = std::move(text);
  else {
    results_.push_back({NewBlockId(), std::move(text), alternative, false});
    std::sort(results_.begin(), results_.end(),
              [](const auto &a, const auto &b) {
                return a.alternative < b.alternative;
              });
    if (selected_.empty())
      selected_ = results_.front().id;
  }
  return true;
}
bool DerivedWorkspace::Finish(const DerivedRequest &r, bool grant_valid,
                              bool success) {
  if (!Accepts(r, grant_valid))
    return false;
  results_.erase(
      std::remove_if(results_.begin(), results_.end(),
                     [](const auto &item) { return item.text.empty(); }),
      results_.end());
  if (!success || results_.empty()) {
    active_request_.clear();
    results_.clear();
    selected_.clear();
    phase_ = DerivedPhase::Failed;
    return true;
  }
  for (auto &item : results_)
    item.ready = true;
  if (!SelectedResult())
    selected_ = results_.front().id;
  phase_ = DerivedPhase::Ready;
  return true;
}
void DerivedWorkspace::Cancel() { Invalidate(); }
void DerivedWorkspace::ReturnToEdit() { Invalidate(); }
bool DerivedWorkspace::SelectResult(std::string_view id) {
  const auto found =
      std::find_if(results_.begin(), results_.end(),
                   [&](const auto &item) { return item.id == id; });
  if (found == results_.end() || protected_)
    return false;
  selected_ = found->id;
  return true;
}
bool DerivedWorkspace::MoveResult(int delta) {
  if (results_.empty() || protected_ || !delta)
    return false;
  const auto found =
      std::find_if(results_.begin(), results_.end(),
                   [&](const auto &r) { return r.id == selected_; });
  const int index =
      found == results_.end() ? 0 : static_cast<int>(found - results_.begin());
  selected_ = results_[std::clamp(index + (delta < 0 ? -1 : 1), 0,
                                  static_cast<int>(results_.size()) - 1)]
                  .id;
  return true;
}
const DerivedResult *DerivedWorkspace::SelectedResult() const {
  const auto found =
      std::find_if(results_.begin(), results_.end(),
                   [&](const auto &item) { return item.id == selected_; });
  return found == results_.end() ? nullptr : &*found;
}
std::string DerivedWorkspace::CopyText() const {
  const auto *result = SelectedResult();
  return visible_ && !protected_ && phase_ == DerivedPhase::Ready && result &&
                 result->ready
             ? result->text
             : "";
}
bool DerivedWorkspace::ConsumePrefix(const DerivedRequest& request, bool grant_valid,
    std::string_view id, std::string_view text) {
  const auto* selected = SelectedResult();
  if (!Owns(request, grant_valid) || phase_ != DerivedPhase::Ready || !selected || selected->id != id ||
      text.empty() || selected->text.compare(0, text.size(), text) != 0) return false;
  auto remaining = *selected;
  remaining.text.erase(0, text.size());
  remaining.id = NewBlockId();
  results_ = {remaining};
  selected_ = remaining.id;
  return true;
}
} // namespace rimes::buffer
