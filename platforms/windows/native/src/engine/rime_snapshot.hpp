#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../core/broker_protocol.hpp"

namespace rimes::windows::engine {

// An owned, validated view of one librime update. librime expresses the
// composition positions as UTF-8 byte offsets; this boundary converts them to
// UTF-16 code-unit offsets before anything reaches TSF.
struct EngineCandidate {
  std::uint32_t id = 0;
  std::string text;
  std::string comment;
  std::string label;
};

struct EngineSnapshot {
  bool handled = false;
  bool modifier_snapshot = false;
  bool composing = false;
  std::uint32_t caret_utf16 = 0;
  std::uint32_t selection_start_utf16 = 0;
  std::uint32_t selection_length_utf16 = 0;
  std::uint16_t highlighted_candidate = core::kNoCandidateSelected;
  std::uint32_t page_number = 0;
  std::uint16_t page_size = 0;
  bool last_page = false;
  std::uint32_t candidate_page_keys = 0;
  std::string composition;
  std::string commit_text;
  std::vector<EngineCandidate> candidates;
};

// Non-owning input used by the ABI adapter and by conversion unit tests. Every
// string is validated and copied before BuildEngineSnapshot succeeds.
struct RawCandidateView {
  std::string_view text;
  std::string_view comment;
  std::string_view label;
};

struct RawSnapshotView {
  bool handled = false;
  bool modifier_snapshot = false;
  bool has_context = false;
  std::string_view composition;
  std::string_view commit_text;
  std::size_t caret_utf8 = 0;
  std::size_t selection_start_utf8 = 0;
  std::size_t selection_end_utf8 = 0;
  std::uint32_t page_number = 0;
  std::uint32_t page_size = 0;
  bool last_page = false;
  std::uint32_t candidate_page_keys = 0;
  int highlighted_candidate = -1;
  std::span<const RawCandidateView> candidates;
};

// Builds an all-or-nothing snapshot. Invalid UTF-8, non-scalar byte offsets,
// oversized text, and inconsistent paging data leave output empty.
bool BuildEngineSnapshot(const RawSnapshotView& raw,
                         EngineSnapshot* output,
                         std::string* error = nullptr) noexcept;

// Converts the engine result into the bounded broker DTO. The current protocol
// carries a selection length but no independent selection start. To preserve a
// truthful caret, selection length is transmitted only when it begins at the
// caret; EngineSnapshot retains the complete range for a future DTO revision.
bool MapSnapshotToInputState(std::uint64_t broker_session_id,
                             std::uint64_t sequence_id,
                             std::uint64_t revision,
                             const EngineSnapshot& snapshot,
                             core::InputState* output,
                             std::string* error = nullptr) noexcept;

namespace detail {

bool IsValidUtf8(std::string_view text) noexcept;
bool Utf16OffsetAtUtf8Boundary(std::string_view text,
                               std::size_t byte_offset,
                               std::uint32_t* utf16_offset) noexcept;

}  // namespace detail

}  // namespace rimes::windows::engine
