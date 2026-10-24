#include "model.hpp"
#include "semantic_blocks.hpp"

#include <algorithm>
#include <string_view>

namespace rimes::windows::workbench {
namespace {
std::string Join(const std::deque<Block>& blocks) {
  std::string text;
  for (const auto& block : blocks) text += block.text;
  return text;
}
bool EndsSentence(std::string_view text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                           text.back() == '\r'))
    text.remove_suffix(1);
  return text.ends_with("。") || text.ends_with("！") ||
         text.ends_with("？") || text.ends_with(".") || text.ends_with("!") ||
         text.ends_with("?") || text.ends_with("\n");
}
}  // namespace
std::vector<std::string> Sentences(const std::string& text) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    std::size_t end = 0;
    if (text[i] == '\n' || text[i] == '!' || text[i] == '?' || text[i] == '.')
      end = i + 1;
    for (const auto* mark : {"。", "！", "？"})
      if (text.compare(i, 3, mark) == 0) end = i + 3;
    if (end) {
      while (end < text.size() &&
             (text[end] == '\r' || text[end] == '\n' || text[end] == ' '))
        ++end;
      parts.push_back(text.substr(start, end - start));
      start = end;
      i = end - 1;
    }
  }
  if (start < text.size()) parts.push_back(text.substr(start));
  return parts;
}
void Model::Focus(Target target) {
  if (live == target) return;
  direct_blocks_.clear();
  live = target;
  if (capture && bound != live) {
    capture = false;
    preedit.clear();
    if (!busy) status = "输入已暂停，等待目标输入框。";
    // Focus owns delivery, not the source/plugin generation. Keep streaming
    // and staged content in this workbench while its input rail is folded.
    CancelSendAll();
  }
}
void Model::Revoke(Target target) {
  if (live == target) Focus({});
  if (bound == target) {
    capture = false;
    preedit.clear();
  }
}
void Model::Open() {
  visible = true;
  bound = live;
  capture = static_cast<bool>(bound);
  status = capture ? "Buffer" : "Copy only - no input target";
}
void Model::Close() {
  direct_blocks_.clear();
  visible = false;
  capture = false;
  preedit.clear();
  Cancel();
}
void Model::Protect() {
  Close();
  live = {};
  bound = {};
  preview.clear();
  status = "Protected";
}
void Model::Discard() {
  Protect();
  source.clear(); result.clear(); source_links_.clear();
  result_units_.clear(); insertion = 0; selected = false; generated_mode = false;
  translated_source_.clear(); pending_.reset();
  send_all_ = false; send_until_ = 0;
  uncertain = false; translate = false;
  ++revision;
  status = "Buffer reset";
  // Keep request/block identities monotonic: a late pre-reset ack must never
  // consume new content or continue an old multi-block send.
}
std::string Model::SourceText() const { return Join(source); }
std::string Model::ResultText() const { return Join(result); }
bool Model::CompleteResult() const {
  return !result.empty() && !busy && preedit.empty() &&
      (!translate || SourceText() == translated_source_);
}
bool Model::NeedsTranslation() const {
  const auto text = SourceText();
  return text.size() > translated_source_.size() &&
      text.substr(translated_source_.size()).find_first_not_of(" \t\r\n") != std::string::npos;
}
void Model::BeginTranslation() {
  if (!translation_results_ && !result.empty()) ResetTranslation();
  translate = true;
}
void Model::ResetTranslation() {
  if (pending_) return;
  Cancel();
  if (translation_results_) {
    std::erase_if(result, [&](const Block& block) {
      const auto unit = result_units_.find(block.id);
      return unit == result_units_.end() || source_links_.contains(unit->second);
    });
    std::erase_if(result_units_, [&](const auto& entry) { return source_links_.contains(entry.second); });
  } else {
    result.clear(); result_units_.clear();
  }
  source_links_.clear(); translated_source_.clear();
}
bool Model::Append(std::string text, InputKind kind) {
  if (text.empty()) return true;
  if ((selected ? 0 : SourceText().size()) + text.size() > kLimit ||
      (pending_ && pending_->result))
    return false;
  if (selected) {
    if (pending_) return false;
    source.clear(); insertion = 0; selected = false; direct_blocks_.clear();
  }
  insertion = (std::min)(insertion, source.size());
  if (insertion != source.size()) direct_blocks_.clear();
  auto insert = [&](std::string value) {
    const auto id = next_id_++;
    source.insert(source.begin() + static_cast<std::ptrdiff_t>(insertion),
                  {id, std::move(value), {}, kind == InputKind::kExternal});
    ++insertion;
    return id;
  };
  if (kind != InputKind::kDirect) {
    direct_blocks_.clear();
    if (kind == InputKind::kCommit) {
      insert(std::move(text));
    } else {
      for (auto& part : Sentences(text))
        insert(std::move(part));
    }
  } else {
    const bool extend = insertion == source.size() && !source.empty() && !direct_blocks_.empty() &&
        direct_blocks_.back() == source.back().id &&
        (!pending_ || pending_->block.id != source.back().id) &&
        !EndsSentence(source.back().text);
    if (extend) {
      // Keep the editable tail; extending it invalidates only its translation.
      auto parts = Sentences(source.back().text + text);
      source.back().text = std::move(parts.front());
      for (std::size_t i = 1; i < parts.size(); ++i) {
        direct_blocks_.push_back(insert(std::move(parts[i])));
      }
    } else {
      for (auto& part : Sentences(text)) {
        direct_blocks_.push_back(insert(std::move(part)));
      }
    }
  }
  ++revision;
  SourceEdited();
  return true;
}
bool Model::Backspace() {
  if (source.empty() || pending_) return false;
  insertion = (std::min)(insertion, source.size());
  if (selected) {
    source.clear(); insertion = 0; selected = false; direct_blocks_.clear();
  } else if (!insertion) {
    return false;
  } else if (insertion == source.size() && !direct_blocks_.empty() && direct_blocks_.back() == source.back().id) {
    auto& text = source.back().text;
    auto last = text.size() - 1;
    while (last > 0 && (static_cast<unsigned char>(text[last]) & 0xc0) == 0x80)
      --last;
    text.erase(last);
    if (text.empty()) {
      source.pop_back();
      --insertion;
      direct_blocks_.pop_back();
    }
  } else {
    direct_blocks_.clear();
    source.erase(source.begin() + static_cast<std::ptrdiff_t>(--insertion));
  }
  ++revision;
  SourceEdited();
  return true;
}
bool Model::SetInsertion(std::size_t index) {
  if (pending_) return false;
  direct_blocks_.clear();
  insertion = (std::min)(index, source.size());
  selected = false;
  return true;
}
bool Model::MoveInsertion(int direction) {
  if (selected) return SetInsertion(direction < 0 ? 0 : source.size());
  return SetInsertion(direction < 0 ? (insertion ? insertion - 1 : 0) : insertion + 1);
}
bool Model::SelectAll() {
  if (pending_) return false;
  selected = !source.empty(); direct_blocks_.clear();
  return selected;
}
bool Model::DeleteForward() {
  if (selected) return Backspace();
  if (pending_ || insertion >= source.size()) return false;
  source.erase(source.begin() + static_cast<std::ptrdiff_t>(insertion));
  direct_blocks_.clear(); ++revision; SourceEdited();
  return true;
}
void Model::ReturnToEdit() {
  if (pending_) return;
  if (translation_results_) { ResetTranslation(); status = "Buffer"; return; }
  Cancel(); result.clear(); source_links_.clear(); result_units_.clear();
  translated_source_.clear(); status = "Buffer";
}
void Model::SourceEdited() {
  if (!translate) { ResetTranslation(); status = "Buffer"; return; }
  // Keep retired output and unchanged complete source units. Editing a tail
  // must not discard accepted output or include retired source in a new job.
  const auto text = SourceText();
  auto exact_unit = [&](const std::string& prefix, const std::string& unit) {
    if (!text.starts_with(prefix)) return false;
    const auto parts = Sentences(text.substr(prefix.size()));
    return !parts.empty() && parts.front() == unit;
  };
  if (busy && !exact_unit(translated_source_, translating_prefix_.substr(translated_source_.size()))) Cancel();
  std::string prefix;
  bool changed = false;
  for (auto it = source_links_.begin(); it != source_links_.end();) {
    if (!changed && exact_unit(prefix, it->second)) {
      prefix += it->second; ++it;
    } else {
      changed = true;
      const auto unit = result_units_.contains(it->first) ? result_units_.at(it->first) : it->first;
      std::erase_if(result, [&](const Block& b) {
        const auto found = result_units_.find(b.id);
        return found != result_units_.end() && found->second == unit;
      });
      std::erase_if(result_units_, [&](const auto& entry) { return entry.second == unit; });
      it = source_links_.erase(it);
    }
  }
  translated_source_ = std::move(prefix);
}
Generation Model::Generate(std::uint64_t settings_revision, bool translation,
                            bool complete_sentence_only) {
  Cancel();
  Generation job{
      generation_, revision, settings_revision, SourceText(), translation,
      0,           {}};
  if (translation) {
    job.source_offset = translated_source_.size();
    job.prefix = translated_source_;
    if (!job.source.starts_with(job.prefix)) return job;
    job.source.erase(0, job.source_offset);
    const auto parts = Sentences(job.source);
    if (!parts.empty()) job.source = parts.front();
    if (complete_sentence_only && !EndsSentence(job.source)) job.source.clear();
    if (job.source.find_first_not_of(" \t\r\n") == std::string::npos) job.source.clear();
  }
  busy = !job.source.empty() && !pending_ && (translation || result.empty());
  if (busy) {
    translating_prefix_ = translation ? job.prefix + job.source : std::string{};
    preview.clear();
    status = "Waiting for response...";
  }
  return job;
}
bool Model::Accepts(const Generation& job,
                    std::uint64_t settings_revision) const {
  if (!busy || job.id != generation_ ||
      job.settings_revision != settings_revision)
    return false;
  if (!job.translation)
    return job.revision == revision && job.source == SourceText();
  const auto text = SourceText();
  if (!text.starts_with(job.prefix)) return false;
  const auto parts = Sentences(text.substr(job.source_offset));
  return !parts.empty() && parts.front() == job.source;
}
bool Model::Stream(const Generation& job, std::uint64_t settings_revision,
                   std::string text) {
  return StreamSnapshot(job, settings_revision, preview + text);
}
bool Model::StreamSnapshot(const Generation& job, std::uint64_t settings_revision, std::string text) {
  if (!Accepts(job, settings_revision) || text.find('\0') != std::string::npos || ResultText().size() + text.size() > kLimit) return false;
  std::vector<SemanticFragment> parts;
  try { parts = SemanticBlocks(text); } catch (...) { return false; }
  preview = std::move(text);
  streaming.clear();
  for (const auto& part : parts) {
    auto [found, inserted] = stream_ids_.try_emplace(part.index, next_id_);
    if (inserted) ++next_id_;
    streaming.push_back({found->second, part.text, job.target_language});
  }
  status = "Receiving...";
  return true;
}
bool Model::Finish(const Generation& job, std::uint64_t settings_revision,
                   bool success) {
  if (!Accepts(job, settings_revision)) return false;
  busy = false;
  if (!success || preview.empty()) {
    preview.clear(); streaming.clear(); stream_ids_.clear(); reasoning_summary.clear();
    status = "Request failed. Source retained.";
    return false;
  }
  if (streaming.empty()) { preview.clear(); return false; }
  const auto unit = streaming.front().id;
  translation_results_ = job.translation;
  for (auto& block : streaming) {
    result_units_[block.id] = unit;
    result.push_back(std::move(block));
  }
  streaming.clear(); reasoning_summary.clear();
  // Translation source retires when its FIRST child is accepted. Generation
  // keeps its source until the last child, for the return-to-edit lifecycle.
  source_links_[job.translation ? unit : result.back().id] = job.source;
  translated_source_ += job.source;
  preview.clear();
  status = "Ready";
  return true;
}
void Model::Cancel() {
  ++generation_;
  busy = false;
  preview.clear(); streaming.clear(); stream_ids_.clear(); reasoning_summary.clear();
}
void Model::InvalidatePluginResults() {
  Cancel(); translate = false; send_all_ = false;
  // An already-issued insertion must still be acknowledged exactly once.
  // Prevent continuation, then discard remaining results after that ack.
  if (pending_) return;
  result.clear(); source_links_.clear(); result_units_.clear(); translated_source_.clear();
  status = "Plugin authorization changed. Source retained.";
}
Model::Results Model::ParkResults() {
  Results state{std::move(result), std::move(source_links_), std::move(result_units_),
      std::move(translated_source_), SourceText(), translation_results_};
  result.clear(); source_links_.clear(); result_units_.clear(); translated_source_.clear();
  translating_prefix_.clear(); translation_results_ = false;
  return state;
}
void Model::RestoreResults(Results state) {
  result = std::move(state.blocks); source_links_ = std::move(state.links);
  result_units_ = std::move(state.units); translated_source_ = std::move(state.translated_source);
  translation_results_ = state.translation;
  if (state.source_snapshot != SourceText()) {
    // Revalidate mutable units against the shared source. Already-retired
    // translation children survive, since their original text no longer exists.
    const bool mode = translate;
    translate = translation_results_;
    SourceEdited();
    translate = mode;
  }
}
std::optional<Delivery> Model::Send(bool all) {
  if (!visible || !capture || !bound || bound != live || pending_ ||
      uncertain || (busy && result.empty()))
    return {};
  const bool output = !result.empty();
  if (!output && (translate || generated_mode)) return {};
  const auto& blocks = output ? result : source;
  if (blocks.empty()) return {};
  if (output && !translated_source_.empty() &&
      !SourceText().starts_with(translated_source_))
    return {};
  // Logical insertion can make source ids non-monotonic in display order.
  // Freeze the creation watermark, not the id of the rightmost block.
  if (!send_all_) send_until_ = next_id_ - 1;
  send_all_ = all;
  pending_ = Delivery{next_request_++, bound, blocks.front(), output};
  if (!output) { insertion = source.size(); selected = false; }
  direct_blocks_.clear();
  status = "Sending...";
  return pending_;
}
void Model::RemoveSourcePrefix(std::size_t bytes) {
  while (bytes && !source.empty()) {
    auto& b = source.front();
    auto take = (std::min)(bytes, b.text.size());
    b.text.erase(0, take);
    bytes -= take;
    if (b.text.empty()) {
      source.pop_front();
      if (insertion) --insertion;
    }
  }
  ++revision;
}
std::optional<Delivery> Model::Acknowledge(std::uint64_t request, Target target,
                                           bool accepted) {
  if (!pending_ || pending_->request != request || pending_->target != target)
    return {};
  const auto delivered = *pending_;
  pending_.reset();
  auto& blocks = delivered.result ? result : source;
  if (!accepted) {
    send_all_ = false;
    status = "Delivery failed. Content retained.";
    return {};
  }
  if (blocks.empty() || blocks.front().id != delivered.block.id ||
      blocks.front().text != delivered.block.text) {
    uncertain = true;
    send_all_ = false;
    status = "Delivery state changed. Check the target before retrying.";
    return {};
  }
  blocks.pop_front();
  if (!delivered.result && insertion) --insertion;
  if (delivered.result) result_units_.erase(delivered.block.id);
  if (delivered.result) {
    const auto link = source_links_.find(delivered.block.id);
    if (link != source_links_.end()) {
      if (!SourceText().starts_with(link->second)) {
        uncertain = true;
        send_all_ = false;
        return {};
      }
      const auto bytes = link->second.size();
      RemoveSourcePrefix(bytes);
      translated_source_.erase(0, bytes);
      source_links_.erase(link);
      Cancel();
    }
  }
  if (!delivered.result) ++revision;
  if (send_all_ && !blocks.empty() && blocks.front().id <= send_until_ &&
      live == bound && capture)
    return Send(true);
  send_all_ = false;
  status = "Delivered";
  return {};
}
void Model::LostAcknowledgement() {
  if (pending_) {
    uncertain = true;
    send_all_ = false;
    capture = false;
    status =
        "Delivery unconfirmed. Check the target; automatic retry disabled.";
  }
}
}  // namespace rimes::windows::workbench
