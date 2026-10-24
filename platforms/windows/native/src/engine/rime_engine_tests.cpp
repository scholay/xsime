#include "rime_engine.hpp"
#include "rime_snapshot.hpp"
#include "candidate_paging.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace rimes::windows::engine {
namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void TestUtf8AndSnapshotConversion() {
  const std::string composition =
      "a\xE4\xB8\xAD\xF0\x9F\x98\x80";  // a, CJK, supplementary scalar
  const std::vector<RawCandidateView> candidates = {
      {"\xE5\x80\x99\xE9\x80\x89\xE4\xB8\x80", "note", "1"},
      {"second", "\xF0\x9F\x8C\x9F", "2"},
  };

  RawSnapshotView raw;
  raw.handled = true;
  raw.has_context = true;
  raw.composition = composition;
  raw.commit_text = "";
  raw.caret_utf8 = composition.size();
  raw.selection_start_utf8 = 1;
  raw.selection_end_utf8 = 4;
  raw.page_number = 2;
  raw.page_size = 5;
  raw.last_page = true;
  raw.candidate_page_keys = static_cast<std::uint32_t>(core::InputStateFlags::kEqualPageDown);
  raw.highlighted_candidate = 1;
  raw.candidates = candidates;

  EngineSnapshot snapshot;
  std::string error;
  Check(BuildEngineSnapshot(raw, &snapshot, &error),
        "valid UTF-8 snapshot should convert");
  Check(error.empty(), "valid conversion should not report an error");
  Check(snapshot.handled && snapshot.composing,
        "handled composition flags should survive conversion");
  Check(snapshot.caret_utf16 == 4,
        "UTF-8 caret should convert to four UTF-16 code units");
  Check(snapshot.selection_start_utf16 == 1 &&
            snapshot.selection_length_utf16 == 1,
        "CJK selection should convert from byte to UTF-16 offsets");
  Check(snapshot.candidates.size() == 2 &&
            snapshot.candidates[0].id == 10 &&
            snapshot.candidates[1].id == 11,
        "candidate IDs should preserve the librime page offset");

  core::InputState state;
  Check(MapSnapshotToInputState(7, 9, 3, snapshot, &state, &error),
        "valid snapshot should map to the broker DTO");
  Check(state.caret_utf16 == 4 && state.selection_length_utf16 == 0,
        "mapper should preserve caret when selection has a different start");
  Check(state.page_start == 0 && state.page_size == 2,
        "wire page should describe the transmitted current-page candidates");
  Check(state.candidates.size() == 2 && state.candidates[0].id == 10,
        "wire candidate should retain its global engine ID");
  Check(snapshot.last_page && (state.state_flags &
      static_cast<std::uint32_t>(core::InputStateFlags::kLastCandidatePage)),
      "librime last-page metadata survives conversion without inferring from a short page");
  Check((state.state_flags & core::kCandidatePageKeyFlags) == raw.candidate_page_keys,
      "effective schema paging bindings survive the engine and wire snapshot boundary");
  raw.last_page = false;
  Check(BuildEngineSnapshot(raw, &snapshot, &error) &&
      MapSnapshotToInputState(7, 10, 4, snapshot, &state, &error) &&
      !(state.state_flags & static_cast<std::uint32_t>(core::InputStateFlags::kLastCandidatePage)),
      "a short page with an engine neighbour remains pageable");
}

void TestCandidatePageKeyBindings() {
  using F = core::InputStateFlags;
  const auto flag = [](F value) { return static_cast<std::uint32_t>(value); };
  std::map<std::string, std::string> config{
      {"engine/processors/@2", "key_binder"},
      {"key_binder/bindings/@0/accept", "minus"}, {"key_binder/bindings/@0/when", "paging"},
      {"key_binder/bindings/@0/send", "Page_Up"},
      {"key_binder/bindings/@1/accept", "equal"}, {"key_binder/bindings/@1/when", "has_menu"},
      {"key_binder/bindings/@1/send", "Page_Down"}};
  const auto read = [&] { return ReadCandidatePageKeys([&](const std::string& path) {
    const auto found = config.find(path); return found == config.end() ? std::string() : found->second;
  }); };
  const auto normal = flag(F::kMinusPageUp) | flag(F::kMinusPagingOnly) | flag(F::kEqualPageDown);
  Check(read() == normal, "actual menu/paging symbol bindings retain their direction and condition");
  config["key_binder/bindings/@2/accept"] = "minus";
  config["key_binder/bindings/@2/when"] = "paging";
  config["key_binder/bindings/@2/send"] = "Page_Up";
  Check(read() == normal, "identical duplicate bindings remain unambiguous");
  config["key_binder/bindings/@2/send"] = "Escape";
  Check(read() == flag(F::kEqualPageDown), "a later custom binding blocks interception of the same symbol");
  config["key_binder/bindings/@0/send"] = "Escape";
  config["key_binder/bindings/@2/send"] = "Page_Up";
  Check(read() == flag(F::kEqualPageDown), "an earlier unknown binding also fails closed");
  config["key_binder/bindings/@0/accept"] = "Control+minus";
  Check(read() == normal, "modified bindings do not change plain-symbol ownership");
  config["key_binder/bindings/@2/when"] = "always";
  Check(read() == flag(F::kEqualPageDown), "unsupported conditions remain owned by the engine");
  config["key_binder/bindings/@2/accept"] = "a";
  config["key_binder/bindings/@1/send"] = "Page_Up";
  Check(read() == flag(F::kEqualPageUp), "explicit reverse paging is supported without inferring letter bindings");
  config["key_binder/bindings/@3/accept"] = "equal";
  config["key_binder/bindings/@3/when"] = "has_menu";
  config["key_binder/bindings/@3/send"] = "Escape";
  Check(read() == 0, "custom equal action revokes paging metadata for compact and expanded consumers");
  config["key_binder/bindings/@3/send"] = "Page_Down";
  Check(read() == 0, "conflicting equal directions never grant either paging direction");
  config["key_binder/bindings/@3/send"] = "Page_Up";
  Check(read() == flag(F::kEqualPageUp), "identical reverse bindings retain one unambiguous direction");
  config.erase("engine/processors/@2");
  Check(read() == 0, "unused key_binder configuration never creates a shortcut");
}

void TestMalformedUtf8FailsClosed() {
  const std::string malformed("\xED\xA0\x80", 3);  // UTF-8 surrogate
  RawSnapshotView raw;
  raw.handled = true;
  raw.has_context = true;
  raw.composition = malformed;
  raw.caret_utf8 = malformed.size();
  raw.selection_start_utf8 = malformed.size();
  raw.selection_end_utf8 = malformed.size();

  EngineSnapshot snapshot;
  std::string error;
  Check(!BuildEngineSnapshot(raw, &snapshot, &error),
        "surrogate UTF-8 must be rejected");
  Check(!snapshot.handled && snapshot.composition.empty(),
        "failed conversion must clear its output");
}

void TestMidScalarOffsetFailsClosed() {
  const std::string composition = "\xE4\xB8\xAD";
  RawSnapshotView raw;
  raw.handled = true;
  raw.has_context = true;
  raw.composition = composition;
  raw.caret_utf8 = 1;
  raw.selection_start_utf8 = 0;
  raw.selection_end_utf8 = composition.size();

  EngineSnapshot snapshot;
  Check(!BuildEngineSnapshot(raw, &snapshot),
        "offset inside a UTF-8 scalar must be rejected");
}

void TestMalformedOptionalCommentPreservesCandidate() {
  const std::string malformed("\xE4\xB8", 2);  // Truncated CJK annotation.
  std::vector<RawCandidateView> candidates = {{"valid", malformed, "1"}};
  RawSnapshotView raw;
  raw.handled = true;
  raw.has_context = true;
  raw.candidates = candidates;
  raw.page_size = 1;
  raw.highlighted_candidate = 0;
  EngineSnapshot snapshot;
  std::string error;
  Check(BuildEngineSnapshot(raw, &snapshot, &error) && error.empty() &&
            snapshot.candidates.size() == 1 &&
            snapshot.candidates[0].text == "valid" &&
            snapshot.candidates[0].comment.empty(),
        "malformed optional annotations cannot discard valid input candidates");
  candidates[0].text = malformed;
  Check(!BuildEngineSnapshot(raw, &snapshot),
        "malformed candidate text remains rejected");
  candidates[0].text = "valid";
  raw.commit_text = malformed;
  Check(!BuildEngineSnapshot(raw, &snapshot),
        "malformed committed text remains rejected");
}

void TestCandidateLimitsFailClosed() {
  std::vector<RawCandidateView> candidates(core::kMaxCandidateCount + 1,
                                           {"x", "", "1"});
  RawSnapshotView raw;
  raw.handled = true;
  raw.has_context = true;
  raw.composition = "x";
  raw.caret_utf8 = 1;
  raw.selection_start_utf8 = 1;
  raw.selection_end_utf8 = 1;
  raw.page_size = static_cast<std::uint32_t>(candidates.size());
  raw.highlighted_candidate = 0;
  raw.candidates = candidates;

  EngineSnapshot snapshot;
  Check(!BuildEngineSnapshot(raw, &snapshot),
        "oversized candidate pages must be rejected");
}

void TestUnhandledInputIsMutationFree() {
  RawSnapshotView raw;
  raw.handled = false;
  raw.has_context = true;
  raw.composition = std::string_view("\xFF", 1);

  EngineSnapshot snapshot;
  Check(BuildEngineSnapshot(raw, &snapshot),
        "unhandled input should fail open without parsing mutations");
  Check(!snapshot.handled && snapshot.composition.empty() &&
            snapshot.candidates.empty(),
        "unhandled snapshot must be empty");
}

void TestModifierSnapshotPreservesHostOwnership() {
  RawSnapshotView raw;
  raw.modifier_snapshot = true;
  raw.commit_text = "ni";
  EngineSnapshot snapshot;
  Check(BuildEngineSnapshot(raw, &snapshot), "modifier commit is a valid snapshot");
  Check(!snapshot.handled && snapshot.modifier_snapshot && snapshot.commit_text == "ni",
        "modifier commit never claims the host key");
  core::InputState wire;
  Check(MapSnapshotToInputState(7, 8, 9, snapshot, &wire),
        "modifier commit maps to the negotiated wire state");
  Check((wire.state_flags & static_cast<unsigned>(core::InputStateFlags::kHandled)) == 0 &&
            (wire.state_flags & static_cast<unsigned>(core::InputStateFlags::kModifierSnapshot)) != 0,
        "wire state separates a modifier snapshot from eaten keys");
  raw.commit_text = std::string_view("\xFF", 1);
  Check(!BuildEngineSnapshot(raw, &snapshot),
        "pass-through modifier commits still reject malformed text");
}

#ifdef _WIN32
void TestDllPathGate() {
  RimeEngine engine;
  RimeEngineOptions options;
  options.dll_path = L"rime.dll";
  options.shared_data_dir = L"C:\\RIMES-test-shared";
  options.user_data_dir = L"C:\\RIMES-test-user";

  std::string error;
  Check(!engine.Start(options, &error),
        "relative DLL path must be rejected before loading");
  Check(!error.empty(), "rejected DLL path should report a reason");
  Check(!engine.IsHealthy(), "failed load must leave the engine unhealthy");
  EngineSnapshot snapshot; snapshot.commit_text = "stale";
  Check(!engine.CommitComposition(9, &snapshot, &error) && snapshot.commit_text.empty(),
        "composition settlement fails closed without an owning engine session");
}
#endif

}  // namespace
}  // namespace rimes::windows::engine

int main() {
  using namespace rimes::windows::engine;
  TestUtf8AndSnapshotConversion();
  TestCandidatePageKeyBindings();
  TestMalformedUtf8FailsClosed();
  TestMalformedOptionalCommentPreservesCandidate();
  TestMidScalarOffsetFailsClosed();
  TestCandidateLimitsFailClosed();
  TestUnhandledInputIsMutationFree();
  TestModifierSnapshotPreservesHostOwnership();
#ifdef _WIN32
  TestDllPathGate();
#endif

  if (failures != 0) {
    std::cerr << failures << " engine test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "Rime engine tests passed\n";
  return EXIT_SUCCESS;
}
