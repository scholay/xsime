#pragma once
#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rimes::windows::workbench {
struct Target {
  std::uint32_t process = 0;
  std::uint64_t session = 0;
  std::uint64_t context = 0;
  bool operator==(const Target&) const = default;
  explicit operator bool() const { return process && session && context; }
};
struct Block {
  std::uint64_t id = 0;
  std::string text;
  std::string language;
  bool external = false;
};
enum class InputKind { kCommit, kDirect, kExternal };
struct Delivery {
  std::uint64_t request = 0;
  Target target;
  Block block;
  bool result = false;
};
struct Generation {
  std::uint64_t id = 0, revision = 0, settings_revision = 0;
  std::string source;
  bool translation = false;
  std::size_t source_offset = 0;
  std::string prefix;
  std::string plugin_id, plugin_grant, instruction;
  std::string connector_id, connector_grant;
  std::string target_language;
  std::string model, effort;
  // Per-request observers, guarded by Runtime identity and grant checks.
  std::function<bool(const std::string&)> snapshot;
  std::function<bool(bool, const std::string&)> progress;
};
// Pure state machine. Broker serializes calls. Nothing here persists source
// text.
class Model {
 public:
  static constexpr std::size_t kLimit = 256 * 1024;
  bool visible = false, capture = false, busy = false, translate = false;
  bool uncertain = false;
  Target live, bound;
  std::deque<Block> source, result, streaming;
  std::string preview, preedit, status, reasoning_summary;
  std::uint64_t revision = 1;
  std::size_t insertion = 0;
  bool selected = false, generated_mode = false;
  void Focus(Target target);
  void Revoke(Target target);
  void Open();
  void Close();
  void Protect();
  void Discard();
  bool Append(std::string text, InputKind kind = InputKind::kCommit);
  bool Backspace();
  bool DeleteForward();
  bool MoveInsertion(int direction);
  bool SetInsertion(std::size_t index);
  bool SelectAll();
  void ReturnToEdit();
  std::string SourceText() const;
  std::string ResultText() const;
  bool CompleteResult() const;
  bool NeedsTranslation() const;
  // Language/owner changes retire only mutable output. Children whose source
  // has already been delivered remain immutable until sent or discarded.
  void ResetTranslation();
  void BeginTranslation();
  Generation Generate(std::uint64_t settings_revision, bool translation,
                      bool complete_sentence_only = false);
  bool Accepts(const Generation& job, std::uint64_t settings_revision) const;
  bool Stream(const Generation& job, std::uint64_t settings_revision,
              std::string text);
  bool StreamSnapshot(const Generation& job, std::uint64_t settings_revision, std::string text);
  bool Finish(const Generation& job, std::uint64_t settings_revision,
              bool success);
  void Cancel();
  void InvalidatePluginResults();
  // In-memory output owned by one plugin; source and input authority stay shared.
  struct Results {
    std::deque<Block> blocks;
    std::map<std::uint64_t, std::string> links;
    std::map<std::uint64_t, std::uint64_t> units;
    std::string translated_source, source_snapshot;
    bool translation = false;
  };
  Results ParkResults(); // caller has cancelled generation and excluded Pending()
  void RestoreResults(Results state);
  std::optional<Delivery> Send(bool all);
  // Shared monotonic wire sequence for explicit native peripheral delivery.
  std::uint64_t ReserveDeliveryRequest() { return next_request_++; }
  std::optional<Delivery> Acknowledge(std::uint64_t request, Target target,
                                      bool accepted);
  void LostAcknowledgement();
  bool Pending() const { return pending_.has_value(); }
  bool OwnsDelivery(std::uint64_t request, Target target) const {
    return pending_ && pending_->request == request && pending_->target == target;
  }
  void CancelSendAll() { send_all_ = false; }
  std::uint64_t GenerationID() const { return generation_; }

 private:
  std::optional<Delivery> pending_;
  std::uint64_t next_id_ = 1, next_request_ = 1, generation_ = 0;
  bool send_all_ = false;
  std::map<std::uint64_t, std::string> source_links_;
  std::map<std::uint64_t, std::uint64_t> result_units_;
  std::uint64_t send_until_ = 0;
  std::map<std::size_t, std::uint64_t> stream_ids_;
  std::string translated_source_;
  std::string translating_prefix_;
  bool translation_results_ = false;
  // Only an uninterrupted direct-input run is character-editable. Accepted
  // Rime commits are hard block boundaries, just like the macOS workbench.
  std::vector<std::uint64_t> direct_blocks_;
  void RemoveSourcePrefix(std::size_t bytes);
  void SourceEdited();
};
std::vector<std::string> Sentences(const std::string& text);
}  // namespace rimes::windows::workbench
