#pragma once
#include "buffer_model.hpp"
#include "plugin_registry.hpp"
#include <optional>

namespace rimes::buffer {
enum class DerivedPhase { Idle, Running, Ready, Failed, Protected };
struct FocusIdentity {
  std::string context;
  std::uint64_t epoch = 0;
  bool operator==(const FocusIdentity &other) const {
    return context == other.context && epoch == other.epoch;
  }
};
// A trusted host freezes this identity. Renderers cannot mint worker grants.
struct DerivedRequest {
  std::string lifetime, id, configuration;
  std::uint64_t source_revision = 0;
  plugins::Lease lease;
  FocusIdentity focus;
  std::vector<Block> source;
  std::string text;
};
struct DerivedResult {
  std::string id, text;
  unsigned alternative = 0;
  bool ready = false;
};
// Event-loop owned. Workers only receive immutable requests and post updates.
class DerivedWorkspace {
public:
  static constexpr std::size_t kSourceLimit = 256 * 1024;
  // Source is present as blocks and staged_text in the v1 snapshot. Leave
  // room for both, results and metadata within its 1 MiB transport frame.
  static constexpr std::size_t kResultLimit = 256 * 1024;
  DerivedWorkspace();
  void Synchronize(const std::vector<Block> &source, std::string plugin,
                   std::string configuration, FocusIdentity focus, bool visible,
                   bool protected_state);
  std::optional<DerivedRequest> Begin(const plugins::Lease &lease);
  bool Accepts(const DerivedRequest &request, bool grant_valid) const;
  // Also validates a finished result at the moment it is copied/delivered.
  bool Owns(const DerivedRequest &request, bool grant_valid) const;
  // Accumulated text replaces the preview, never append a delta twice.
  bool Update(const DerivedRequest &request, bool grant_valid,
              unsigned alternative, std::string text);
  bool Finish(const DerivedRequest &request, bool grant_valid, bool success);
  void Cancel();
  void ReturnToEdit();
  bool SelectResult(std::string_view id);
  bool MoveResult(int delta);
  const DerivedResult *SelectedResult() const;
  std::string CopyText() const;
  // First successful delivery prunes alternatives; every consumption changes
  // the result identity so an older copy command cannot copy a changed value.
  bool ConsumePrefix(const DerivedRequest& request, bool grant_valid,
                     std::string_view result_id, std::string_view text);
  DerivedPhase Phase() const { return phase_; }
  const std::vector<DerivedResult> &Results() const { return results_; }
  const std::string &Plugin() const { return plugin_; }
  std::uint64_t SourceRevision() const { return source_revision_; }
  bool CanGenerate() const;

private:
  void Invalidate();
  std::string lifetime_, plugin_, configuration_, active_request_, selected_;
  std::vector<Block> source_;
  std::vector<DerivedResult> results_;
  FocusIdentity focus_;
  plugins::Lease active_lease_;
  std::uint64_t source_revision_ = 0;
  bool visible_ = false, protected_ = false;
  DerivedPhase phase_ = DerivedPhase::Idle;
};
} // namespace rimes::buffer
