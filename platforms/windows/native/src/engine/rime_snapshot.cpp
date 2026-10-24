#include "rime_snapshot.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace rimes::windows::engine {
namespace {

void SetError(std::string* error, std::string_view message) noexcept {
  if (error == nullptr) {
    return;
  }
  try {
    error->assign(message);
  } catch (...) {
    // Error reporting must not turn malformed engine output into an exception.
  }
}

void Clear(EngineSnapshot* output) noexcept {
  if (output != nullptr) {
    *output = EngineSnapshot{};
  }
}

void Clear(core::InputState* output) noexcept {
  if (output != nullptr) {
    *output = core::InputState{};
  }
}

struct Utf8Scalar {
  std::uint32_t value = 0;
  std::size_t width = 0;
};

std::optional<Utf8Scalar> DecodeScalar(std::string_view text,
                                       std::size_t index) noexcept {
  if (index >= text.size()) {
    return std::nullopt;
  }

  const auto first = static_cast<unsigned char>(text[index]);
  Utf8Scalar scalar;
  if (first <= 0x7fU) {
    scalar = {first, 1};
  } else if (first >= 0xc2U && first <= 0xdfU) {
    scalar = {static_cast<std::uint32_t>(first & 0x1fU), 2};
  } else if (first >= 0xe0U && first <= 0xefU) {
    scalar = {static_cast<std::uint32_t>(first & 0x0fU), 3};
  } else if (first >= 0xf0U && first <= 0xf4U) {
    scalar = {static_cast<std::uint32_t>(first & 0x07U), 4};
  } else {
    return std::nullopt;
  }

  if (scalar.width > text.size() - index) {
    return std::nullopt;
  }
  for (std::size_t offset = 1; offset < scalar.width; ++offset) {
    const auto byte = static_cast<unsigned char>(text[index + offset]);
    if ((byte & 0xc0U) != 0x80U) {
      return std::nullopt;
    }
    scalar.value = (scalar.value << 6U) | (byte & 0x3fU);
  }

  if ((scalar.width == 2 && scalar.value < 0x80U) ||
      (scalar.width == 3 && scalar.value < 0x800U) ||
      (scalar.width == 4 && scalar.value < 0x10000U) ||
      (scalar.value >= 0xd800U && scalar.value <= 0xdfffU) ||
      scalar.value > 0x10ffffU) {
    return std::nullopt;
  }
  return scalar;
}

bool ValidateText(std::string_view text,
                  std::size_t maximum_bytes,
                  const char* field,
                  std::string* error) noexcept {
  if (text.size() > maximum_bytes) {
    SetError(error, field);
    return false;
  }
  if (text.find('\0') != std::string_view::npos) {
    SetError(error, "engine text contains an embedded NUL");
    return false;
  }
  if (!detail::IsValidUtf8(text)) {
    SetError(error, "engine text is not valid UTF-8");
    return false;
  }
  return true;
}

}  // namespace

namespace detail {

bool IsValidUtf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto scalar = DecodeScalar(text, index);
    if (!scalar) {
      return false;
    }
    index += scalar->width;
  }
  return true;
}

bool Utf16OffsetAtUtf8Boundary(std::string_view text,
                               std::size_t byte_offset,
                               std::uint32_t* utf16_offset) noexcept {
  if (utf16_offset == nullptr || byte_offset > text.size()) {
    return false;
  }

  std::uint64_t units = 0;
  std::size_t index = 0;
  while (index < byte_offset) {
    const auto scalar = DecodeScalar(text, index);
    if (!scalar || scalar->width > byte_offset - index) {
      return false;
    }
    units += scalar->value > 0xffffU ? 2U : 1U;
    if (units > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    index += scalar->width;
  }
  if (index != byte_offset) {
    return false;
  }
  // Also reject an otherwise valid prefix followed by malformed UTF-8.
  if (!IsValidUtf8(text.substr(byte_offset))) {
    return false;
  }
  *utf16_offset = static_cast<std::uint32_t>(units);
  return true;
}

}  // namespace detail

bool BuildEngineSnapshot(const RawSnapshotView& raw,
                         EngineSnapshot* output,
                         std::string* error) noexcept {
  Clear(output);
  if (output == nullptr) {
    SetError(error, "engine snapshot output is null");
    return false;
  }

  try {
    EngineSnapshot built;
    built.handled = raw.handled;
    built.modifier_snapshot = raw.modifier_snapshot;

    // Ordinary unhandled events remain mutation-free. The negotiated modifier
    // path carries a separately marked snapshot without claiming the host key.
    if (!raw.handled && !raw.modifier_snapshot) {
      *output = std::move(built);
      return true;
    }

    if (!ValidateText(raw.composition, core::kMaxCompositionBytes,
                      "composition exceeds its engine limit", error) ||
        !ValidateText(raw.commit_text, core::kMaxCommitTextBytes,
                      "commit text exceeds its engine limit", error)) {
      return false;
    }
    if (raw.candidates.size() > core::kMaxCandidateCount) {
      SetError(error, "candidate count exceeds its engine limit");
      return false;
    }
    if (!raw.has_context &&
        (!raw.composition.empty() || !raw.candidates.empty() ||
         raw.caret_utf8 != 0 || raw.selection_start_utf8 != 0 ||
         raw.selection_end_utf8 != 0 || raw.page_number != 0 ||
         raw.page_size != 0 || raw.highlighted_candidate != -1)) {
      SetError(error, "missing librime context contains context data");
      return false;
    }

    std::uint32_t caret = 0;
    std::uint32_t selection_start = 0;
    std::uint32_t selection_end = 0;
    if (!detail::Utf16OffsetAtUtf8Boundary(raw.composition, raw.caret_utf8,
                                           &caret) ||
        !detail::Utf16OffsetAtUtf8Boundary(raw.composition,
                                           raw.selection_start_utf8,
                                           &selection_start) ||
        !detail::Utf16OffsetAtUtf8Boundary(raw.composition,
                                           raw.selection_end_utf8,
                                           &selection_end) ||
        selection_end < selection_start) {
      SetError(error, "composition offset is not a valid UTF-8 boundary");
      return false;
    }

    if (raw.candidates.empty()) {
      if (raw.page_number != 0 || raw.page_size != 0 ||
          raw.highlighted_candidate != -1) {
        SetError(error, "empty candidate list contains paging data");
        return false;
      }
    } else {
      if (raw.page_size == 0 || raw.page_size > core::kMaxCandidateCount ||
          raw.candidates.size() > raw.page_size) {
        SetError(error, "candidate page metadata is inconsistent");
        return false;
      }
      if (raw.highlighted_candidate < -1 ||
          raw.highlighted_candidate >=
              static_cast<int>(raw.candidates.size())) {
        SetError(error, "highlighted candidate is outside the current page");
        return false;
      }
    }

    const std::uint64_t first_candidate_id =
        static_cast<std::uint64_t>(raw.page_number) * raw.page_size;
    if (first_candidate_id + raw.candidates.size() >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
            1U) {
      SetError(error, "candidate id exceeds its engine limit");
      return false;
    }

    built.composition.assign(raw.composition);
    built.commit_text.assign(raw.commit_text);
    built.caret_utf16 = caret;
    built.selection_start_utf16 = selection_start;
    built.selection_length_utf16 = selection_end - selection_start;
    built.composing = raw.has_context &&
                      (!built.composition.empty() || !raw.candidates.empty());
    built.page_number = raw.page_number;
    built.page_size = static_cast<std::uint16_t>(raw.page_size);
    built.last_page = raw.last_page;
    built.candidate_page_keys = raw.candidate_page_keys;
    if (raw.highlighted_candidate >= 0) {
      built.highlighted_candidate =
          static_cast<std::uint16_t>(raw.highlighted_candidate);
    }

    built.candidates.reserve(raw.candidates.size());
    for (std::size_t index = 0; index < raw.candidates.size(); ++index) {
      const RawCandidateView& raw_candidate = raw.candidates[index];
      if (raw_candidate.text.empty()) {
        SetError(error, "candidate text must not be empty");
        return false;
      }
      const std::string_view comment = detail::IsValidUtf8(raw_candidate.comment)
                                           ? raw_candidate.comment
                                           : std::string_view{};
      if (!ValidateText(raw_candidate.text, core::kMaxCandidateTextBytes,
                        "candidate text exceeds its engine limit", error) ||
          !ValidateText(comment,
                        core::kMaxCandidateCommentBytes,
                        "candidate comment exceeds its engine limit", error) ||
          !ValidateText(raw_candidate.label, core::kMaxCandidateLabelBytes,
                        "candidate label exceeds its engine limit", error)) {
        return false;
      }
      built.candidates.push_back(
          {static_cast<std::uint32_t>(first_candidate_id + index),
           std::string(raw_candidate.text), std::string(comment),
           std::string(raw_candidate.label)});
    }

    *output = std::move(built);
    return true;
  } catch (...) {
    Clear(output);
    SetError(error, "exception while converting librime output");
    return false;
  }
}

bool MapSnapshotToInputState(std::uint64_t broker_session_id,
                             std::uint64_t sequence_id,
                             std::uint64_t revision,
                             const EngineSnapshot& snapshot,
                             core::InputState* output,
                             std::string* error) noexcept {
  Clear(output);
  if (output == nullptr) {
    SetError(error, "input state output is null");
    return false;
  }
  if (broker_session_id == 0 || sequence_id == 0) {
    SetError(error, "input state identifiers must not be zero");
    return false;
  }

  try {
    core::InputState mapped;
    mapped.session_id = broker_session_id;
    mapped.sequence_id = sequence_id;
    mapped.revision = revision;
    if (!snapshot.handled && !snapshot.modifier_snapshot) {
      *output = std::move(mapped);
      return true;
    }

    mapped.state_flags = static_cast<std::uint32_t>(
        snapshot.modifier_snapshot ? core::InputStateFlags::kModifierSnapshot
                                   : core::InputStateFlags::kHandled);
    if (snapshot.composing) {
      mapped.state_flags |=
          static_cast<std::uint32_t>(core::InputStateFlags::kComposing);
    }
    if (!snapshot.candidates.empty()) {
      mapped.state_flags |= static_cast<std::uint32_t>(
          core::InputStateFlags::kCandidatesVisible);
    }
    mapped.caret_utf16 = snapshot.caret_utf16;
    if (snapshot.selection_start_utf16 == snapshot.caret_utf16) {
      mapped.selection_length_utf16 = snapshot.selection_length_utf16;
    }
    mapped.composition = snapshot.composition;
    mapped.commit_text = snapshot.commit_text;
    mapped.highlighted_candidate = snapshot.highlighted_candidate;

    // EngineSnapshot contains only the current librime page. Candidate IDs
    // preserve the global page offset, while the DTO paging range is local to
    // its transmitted vector as required by broker_protocol validation.
    if (!snapshot.candidates.empty()) {
      mapped.page_start = 0;
      mapped.page_size = static_cast<std::uint16_t>(snapshot.candidates.size());
      if (snapshot.last_page)
        mapped.state_flags |= static_cast<std::uint32_t>(core::InputStateFlags::kLastCandidatePage);
      mapped.state_flags |= snapshot.candidate_page_keys & core::kCandidatePageKeyFlags;
    }
    mapped.candidates.reserve(snapshot.candidates.size());
    for (const EngineCandidate& candidate : snapshot.candidates) {
      mapped.candidates.push_back(
          {candidate.id, candidate.text, candidate.comment, candidate.label});
    }

    // Keep this mapper independently safe even when called without the wire
    // encoder immediately afterward.
    std::vector<std::byte> validation_payload;
    if (!core::EncodeInputState(mapped, &validation_payload, error)) {
      return false;
    }
    *output = std::move(mapped);
    return true;
  } catch (...) {
    Clear(output);
    SetError(error, "exception while mapping the engine snapshot");
    return false;
  }
}

}  // namespace rimes::windows::engine
