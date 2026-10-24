#include <Windows.h>

#include <iostream>

#include "TextService.h"
#include "candidate_layout.hpp"
#include "fake_tsf.hpp"

namespace rimes::windows::tsf {
struct CaretRegressionProbe {
  static void BufferAnchor(TextService& service, bool enabled, RECT caret) {
    service.SetCapture(enabled); service.buffer_caret_ = caret;
  }
  static bool Bind(TextService& service, ITfContext* context) {
    service.client_id_ = 1;
    return service.BindContext(context);
  }
  static HRESULT Apply(TextService& service, ITfContext* context,
                       const BrokerInputState& state) {
    return service.ApplyDocumentState(context, state);
  }
  static bool Retains(TextService& service, ITfContext* context,
                      std::uint64_t revision) {
    return service.active_context_ == context && service.edit_valid_ &&
           service.edit_valid_->load() && service.last_state_.revision == revision;
  }
  static RECT Query(TextService& service, ITfContext* context) {
    return service.QueryCaretRect(context, 0);
  }
  static CandidateSnapshot Snapshot(TextService& service) {
    return service.candidate_window_.snapshot();
  }
  static CandidateSnapshot Present(TextService& service, ITfContext* context) {
    BrokerInputState state;
    state.candidates_visible = true;
    state.composing = true;
    state.composition = L"ni";
    state.caret_utf16 = 2;
    state.buffer_capture = service.capture_active_;
    state.page_size = 5;
    state.candidates.push_back({0, L"test", L"", L"1"});
    service.ApplyDocumentState(context, state, 0);
    return service.candidate_window_.snapshot();
  }
};
}  // namespace rimes::windows::tsf

int main() {
  using namespace rimes::windows;
  const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    std::cerr << "FAIL: COM initialization failed\n";
    return 1;
  }
  e2e::FakeDocument document;
  auto* context = new e2e::FakeContext(&document);
  auto* service = new tsf::TextService(nullptr);
  int failures = 0;
  auto expect = [&](bool ok, const char* message) {
    if (!ok) {
      ++failures;
      std::cerr << "FAIL: " << message << '\n';
    }
  };
  expect(tsf::CaretRegressionProbe::Bind(*service, context), "caret fixture binds");
  document.refuse_caret = true;
  expect(!tsf::CaretRegressionProbe::Present(*service, context).visible,
         "first candidates without verified geometry remain hidden");
  document.refuse_caret = false;
  const RECT known = tsf::CaretRegressionProbe::Query(*service, context);
  expect(EqualRect(&known, &document.caret_rect),
         "TSF caret geometry is retained");
  const auto initial = tsf::CaretRegressionProbe::Present(*service, context);
  expect(initial.visible && EqualRect(&initial.caret_rect, &known),
         "verified TSF geometry anchors the candidate window");
  RECT buffer_caret{360, 240, 361, 258};
  tsf::CaretRegressionProbe::BufferAnchor(*service, true, buffer_caret);
  const auto anchored = tsf::CaretRegressionProbe::Present(*service, context);
  expect(anchored.visible && EqualRect(&anchored.caret_rect, &buffer_caret),
         "captured candidates use the logical Buffer caret instead of the host caret");
  tsf::CaretRegressionProbe::BufferAnchor(*service, true, {});
  expect(!tsf::CaretRegressionProbe::Present(*service, context).visible,
         "missing Buffer geometry cannot reuse a host or previous session caret");
  tsf::CaretRegressionProbe::BufferAnchor(*service, false, {});
  const auto restored = tsf::CaretRegressionProbe::Present(*service, context);
  expect(restored.visible && EqualRect(&restored.caret_rect, &known),
         "leaving Buffer restores verified host caret geometry");
  HWND frame = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 700, 420,
                               600, 400, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
  expect(frame != nullptr, "native host frame exists");
  expect(CreateCaret(frame, nullptr, 2, 20) != FALSE, "dummy native caret exists");
  expect(SetCaretPos(0, 0) != FALSE, "dummy native caret uses the frame origin");
  document.view_window = frame;
  document.refuse_caret = true;
  const RECT unknown = tsf::CaretRegressionProbe::Query(*service, context);
  expect(unknown.bottom <= unknown.top,
         "failed TSF geometry does not use the native frame origin");
  tsf::ScreenRect resolved{};
  expect(tsf::ResolveCandidateCaret(
             {unknown.left, unknown.top, unknown.right, unknown.bottom},
             {known.left, known.top, known.right, known.bottom}, &resolved)
         && resolved.left == known.left && resolved.top == known.top,
         "candidate placement stays at the last verified TSF caret");
  const auto cached = tsf::CaretRegressionProbe::Present(*service, context);
  expect(cached.visible && EqualRect(&cached.caret_rect, &known),
         "candidate update does not jump to the native host frame");
  document.write_caret_before_failure = true;
  document.caret_rect = {700, 420, 702, 440};
  const RECT failed_output = tsf::CaretRegressionProbe::Query(*service, context);
  expect(failed_output.bottom <= failed_output.top,
         "failed GetTextExt output cannot supply candidate geometry");
  const auto failed_cached = tsf::CaretRegressionProbe::Present(*service, context);
  expect(failed_cached.visible && EqualRect(&failed_cached.caret_rect, &known),
         "failed GetTextExt output cannot replace verified placement");
  document.write_caret_before_failure = false;
  document.refuse_caret = false;
  document.caret_rect = {300, 260, 302, 284};
  const auto moved = tsf::CaretRegressionProbe::Present(*service, context);
  expect(moved.visible && EqualRect(&moved.caret_rect, &document.caret_rect),
         "resumed TSF geometry moves candidates to the new caret");
  document.caret_rect = {0, 0, 0, 20};
  const RECT origin = tsf::CaretRegressionProbe::Query(*service, context);
  expect(EqualRect(&origin, &document.caret_rect),
         "valid TSF geometry at the screen origin still works");
  DestroyCaret();
  DestroyWindow(frame);

  // A real composition range and moving selection must produce different
  // rectangles. Querying only the selection would fail these assertions.
  e2e::FakeDocument anchor_document;
  anchor_document.text = L"prefix";
  anchor_document.range_advance = 12;
  auto* anchor_context = new e2e::FakeContext(&anchor_document);
  auto* anchor_service = new tsf::TextService(nullptr);
  expect(tsf::CaretRegressionProbe::Bind(*anchor_service, anchor_context),
         "composition anchor fixture binds");
  tsf::BrokerInputState typing;
  typing.composing = typing.candidates_visible = typing.has_snapshot = true;
  typing.candidates.push_back({0, L"test", L"", L"1"});
  typing.page_size = 5;
  const LONG start_x = anchor_document.caret_rect.left + 6 * anchor_document.range_advance;
  for (const auto* text : {L"n", L"nihao", L"ni"}) {
    typing.composition = text;
    typing.caret_utf16 = static_cast<std::uint32_t>(typing.composition.size());
    expect(SUCCEEDED(tsf::CaretRegressionProbe::Apply(*anchor_service, anchor_context, typing)),
           "preedit update succeeds");
    const auto actual = tsf::CaretRegressionProbe::Snapshot(*anchor_service);
    const auto selection = tsf::CaretRegressionProbe::Query(*anchor_service, anchor_context);
    expect(actual.visible && actual.caret_rect.left == start_x && selection.left > start_x,
           "typing and backspace retain composition origin while selection moves");
    expect(anchor_document.composition == text,
           "querying candidate origin does not collapse or mutate live composition");
  }
  typing.page_start = 5;
  typing.caret_utf16 = 1;
  tsf::CaretRegressionProbe::Apply(*anchor_service, anchor_context, typing);
  expect(tsf::CaretRegressionProbe::Snapshot(*anchor_service).caret_rect.left == start_x,
         "candidate paging and movement inside preedit retain its origin");
  anchor_document.caret_rect.left += 80; anchor_document.caret_rect.right += 80;
  anchor_document.caret_rect.top += 40; anchor_document.caret_rect.bottom += 40;
  anchor_service->OnLayoutChange(anchor_context, TF_LC_CHANGE, nullptr);
  const auto relocated = tsf::CaretRegressionProbe::Snapshot(*anchor_service);
  expect(relocated.caret_rect.left == start_x + 80 &&
             relocated.caret_rect.top == anchor_document.caret_rect.top,
         "host scroll/window layout relocates the composition origin");
  anchor_document.refuse_caret = true;
  typing.composition = L"nihao"; typing.caret_utf16 = 5;
  tsf::CaretRegressionProbe::Apply(*anchor_service, anchor_context, typing);
  auto retained = tsf::CaretRegressionProbe::Snapshot(*anchor_service);
  expect(EqualRect(&retained.caret_rect, &relocated.caret_rect),
         "temporary missing range geometry retains this composition's verified origin");
  anchor_document.refuse_caret = false;
  typing.commit_text = L"你"; typing.composition = L"hao"; typing.caret_utf16 = 3;
  tsf::CaretRegressionProbe::Apply(*anchor_service, anchor_context, typing);
  expect(tsf::CaretRegressionProbe::Snapshot(*anchor_service).caret_rect.left == start_x + 92,
         "partial commit starts the remaining composition after committed text");
  tsf::BrokerInputState cancelled;
  tsf::CaretRegressionProbe::Apply(*anchor_service, anchor_context, cancelled);
  anchor_document.refuse_caret = true;
  typing.commit_text.clear();
  tsf::CaretRegressionProbe::Apply(*anchor_service, anchor_context, typing);
  expect(!tsf::CaretRegressionProbe::Snapshot(*anchor_service).visible,
         "new composition without geometry cannot reuse the cancelled origin");
  tsf::CaretRegressionProbe::Apply(*anchor_service, anchor_context, cancelled);
  anchor_document.refuse_caret = false;
  anchor_document.selection_acp = static_cast<LONG>(anchor_document.text.size());
  anchor_context->defer_edits = true;
  tsf::CaretRegressionProbe::Apply(*anchor_service, anchor_context, typing);
  const auto deferred = tsf::CaretRegressionProbe::Snapshot(*anchor_service);
  expect(deferred.visible && deferred.caret_rect.left == start_x + 92,
         "deferred first preedit uses its pre-write insertion anchor");
  anchor_context->DrainEdits();
  anchor_context->defer_edits = false;
  anchor_service->OnLayoutChange(anchor_context, TF_LC_CHANGE, nullptr);
  retained = tsf::CaretRegressionProbe::Snapshot(*anchor_service);
  expect(EqualRect(&retained.caret_rect, &deferred.caret_rect),
         "accepting deferred preedit does not move candidates to its tail");
  e2e::FakeDocument other_document;
  other_document.refuse_caret = true;
  auto* other_context = new e2e::FakeContext(&other_document);
  tsf::CaretRegressionProbe::Bind(*anchor_service, other_context);
  tsf::CaretRegressionProbe::Apply(*anchor_service, other_context, typing);
  expect(!tsf::CaretRegressionProbe::Snapshot(*anchor_service).visible,
         "switching input contexts cannot reuse the previous composition origin");
  anchor_service->Release();
  other_context->Release();
  anchor_context->Release();

  // An idle Shift tap returns an authoritative modifier snapshot but has no
  // document mutation. Hosts can refuse write locks at this point; this must
  // not revoke the context (which would reset the just-toggled ASCII session).
  e2e::FakeDocument idle_document;
  auto* idle_context = new e2e::FakeContext(&idle_document);
  auto* idle_service = new tsf::TextService(nullptr);
  expect(tsf::CaretRegressionProbe::Bind(*idle_service, idle_context),
         "idle modifier fixture binds without a live Broker");
  idle_context->refuse_write_edits = true;
  tsf::BrokerInputState idle;
  idle.has_snapshot = true;
  idle.revision = 7;
  expect(SUCCEEDED(tsf::CaretRegressionProbe::Apply(
             *idle_service, idle_context, idle)),
         "idle modifier snapshot needs no host write lock");
  expect(idle_context->write_requests == 0 && idle_document.text.empty(),
         "idle modifier snapshot cannot request or queue a document edit");
  expect(tsf::CaretRegressionProbe::Retains(*idle_service, idle_context, 7),
         "idle modifier snapshot retains its context and authoritative revision");

  idle_context->refuse_write_edits = false;
  tsf::CaretRegressionProbe::Bind(*idle_service, idle_context);
  tsf::BrokerInputState preedit;
  preedit.has_snapshot = true;
  preedit.composing = true;
  preedit.composition = L"ni";
  preedit.caret_utf16 = 2;
  expect(SUCCEEDED(tsf::CaretRegressionProbe::Apply(
             *idle_service, idle_context, preedit)) && idle_document.composing,
         "nonempty preedit still obtains a write session");
  const auto before_cancel = idle_context->write_requests;
  expect(SUCCEEDED(tsf::CaretRegressionProbe::Apply(
             *idle_service, idle_context, idle)) && !idle_document.composing &&
             idle_document.text.empty() &&
             idle_context->write_requests > before_cancel,
         "empty state still clears an existing host composition under a write lock");
  tsf::BrokerInputState commit;
  commit.has_snapshot = true;
  commit.commit_text = L"123";
  const auto before_commit = idle_context->write_requests;
  expect(SUCCEEDED(tsf::CaretRegressionProbe::Apply(
             *idle_service, idle_context, commit)) && idle_document.text == L"123" &&
             idle_context->write_requests > before_commit,
         "an idle commit still writes text exactly once");
  tsf::BrokerInputState pending;
  pending.has_snapshot = pending.pending_chord = true;
  expect(SUCCEEDED(tsf::CaretRegressionProbe::Apply(
             *idle_service, idle_context, pending)) && idle_document.composing &&
             idle_document.composition == L"\u200B" && idle_document.text == L"123",
         "a pending chord starts host marked text before engine preedit exists");
  const auto guard_writes = idle_context->write_requests;
  tsf::CaretRegressionProbe::Apply(*idle_service, idle_context, pending);
  expect(idle_context->write_requests == guard_writes,
         "a held chord retains its guard without repeated text writes");
  preedit.pending_chord = true;
  tsf::CaretRegressionProbe::Apply(*idle_service, idle_context, preedit);
  expect(idle_document.composition == L"ni",
         "pending next chord never replaces existing visible preedit with a guard");
  tsf::CaretRegressionProbe::Apply(*idle_service, idle_context, idle);
  expect(!idle_document.composing && idle_document.text == L"123",
         "cancelling chord state leaves no placeholder or raw code in the document");
  tsf::CaretRegressionProbe::Apply(*idle_service, idle_context, pending);
  commit.commit_text = L"有";
  tsf::CaretRegressionProbe::Apply(*idle_service, idle_context, commit);
  expect(!idle_document.composing && idle_document.text == L"123有",
         "a direct chord commit replaces the guard without leaking its placeholder");
  idle_service->Release();
  idle_context->Release();
  service->Release();
  context->Release();
  if (SUCCEEDED(com)) CoUninitialize();
  if (!failures) std::cout << "Caret placement and idle modifier snapshot regression passed\n";
  return failures ? 1 : 0;
}
