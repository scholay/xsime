#include <iostream>
#include "TextService.h"
#include "fake_tsf.hpp"

namespace rimes::windows::tsf {
struct CandidateUIRegressionProbe {
  static InputModeIndicator* Mode(TextService& service) {
    service.input_mode_ = new InputModeIndicator([&service] { service.ToggleInputMode(); });
    return service.input_mode_;
  }
  static HRESULT Apply(TextService& service, ITfContext* context,
                       const BrokerInputState& state) {
    service.BindContext(context);
    return service.ApplyDocumentState(context, state);
  }
  static CandidateWindow& Window(TextService& service) { return service.candidate_window_; }
  static void Page(TextService& service, int delta) { service.PageCandidate(delta); }
};
}

namespace {
class StubBroker final : public rimes::windows::tsf::BrokerClient {
 public:
  bool ascii = false;
  std::optional<bool> AsciiMode() const noexcept override { return ascii; }
  using State = rimes::windows::tsf::BrokerInputState;
  void BeginConnect() noexcept override {}
  void Disconnect() noexcept override {}
  bool SetContext(std::uint64_t) noexcept override { return true; }
  void SetNotificationWindow(HWND) noexcept override {}
  std::optional<rimes::windows::core::Json> TakeNotification() override { return {}; }
  bool Control(rimes::windows::core::Json) noexcept override { return true; }
  bool Capturing() const noexcept override { return false; }
  std::uint64_t ConnectionGeneration() const noexcept override { return 0; }
  bool IsConnected() const noexcept override { return true; }
  rimes::windows::tsf::BrokerKeyResult HandleKey(
      const rimes::windows::tsf::BrokerKeyEvent& event, State* state) noexcept override {
    using namespace rimes::windows::tsf;
    if (event.phase == BrokerKeyPhase::kInputModeToggle ||
        (event.phase == BrokerKeyPhase::kKeyUp && event.virtual_key == VK_SHIFT)) {
      ascii = !ascii;
      if (state) { *state = {}; state->has_snapshot = true; }
      return BrokerKeyResult::kPassThrough;
    }
    if (event.phase != BrokerKeyPhase::kKeyDown || event.virtual_key != '2')
      return BrokerKeyResult::kPassThrough;
    *state = {};
    state->has_snapshot = true;
    state->revision = 2;
    state->commit_text = L"拟好";
    return BrokerKeyResult::kConsumed;
  }
};

class PagingBroker final : public rimes::windows::tsf::BrokerClient {
 public:
  using State = rimes::windows::tsf::BrokerInputState;
  std::size_t page = 0;
  std::uint64_t revision = 1;
  unsigned engine_steps = 0, commits = 0;
  std::size_t committed_index = 99, guard_index = 99;
  std::uint64_t guard_revision = 0;
  bool custom_labels = false;
  std::uint32_t page_keys = 0;
  State Snapshot() const {
    State state;
    state.has_snapshot = state.composing = state.candidates_visible = true;
    state.composition = L"nihao"; state.revision = revision;
    state.page_size = 3; state.highlighted_candidate = 0; state.last_page = page == 1;
    state.candidate_page_keys = page_keys;
    for (std::size_t i = 0; i < 3; ++i)
      state.candidates.push_back({static_cast<std::uint32_t>(page * 3 + i),
          L"候选内容足够长用于分页" + std::to_wstring(page * 3 + i), L"",
          custom_labels ? std::wstring(1, static_cast<wchar_t>(L'a' + i)) : std::to_wstring(i + 1)});
    return state;
  }
  void BeginConnect() noexcept override {}
  void Disconnect() noexcept override {}
  bool SetContext(std::uint64_t) noexcept override { return true; }
  void SetNotificationWindow(HWND) noexcept override {}
  std::optional<rimes::windows::core::Json> TakeNotification() override { return {}; }
  bool Control(rimes::windows::core::Json value) noexcept override {
    if (value.value("op", "") == "candidate_guard") {
      guard_index = value.value("index", std::size_t{99});
      guard_revision = value.value("revision", 0ULL);
      return guard_revision == revision;
    }
    return true;
  }
  bool Capturing() const noexcept override { return false; }
  std::uint64_t ConnectionGeneration() const noexcept override { return 0; }
  bool IsConnected() const noexcept override { return true; }
  rimes::windows::tsf::BrokerKeyResult HandleKey(
      const rimes::windows::tsf::BrokerKeyEvent& event, State* state) noexcept override {
    using namespace rimes::windows::tsf;
    if (event.phase != BrokerKeyPhase::kKeyDown || !state) return BrokerKeyResult::kPassThrough;
    if (event.virtual_key == VK_NEXT || event.virtual_key == VK_PRIOR) {
      ++engine_steps; ++revision;
      page = event.virtual_key == VK_NEXT ? 1 : 0;
      *state = Snapshot(); return BrokerKeyResult::kConsumed;
    }
    if (!custom_labels && event.virtual_key >= '1' && event.virtual_key <= '3' &&
        guard_revision == revision && guard_index == event.virtual_key - '1') {
      ++commits; committed_index = page * 3 + guard_index;
      *state = {}; state->has_snapshot = true; state->revision = ++revision;
      state->commit_text = L"selected"; return BrokerKeyResult::kConsumed;
    }
    return BrokerKeyResult::kPassThrough;
  }
};

int TestNativeCompactPaging() {
  using namespace rimes::windows;
  int failures = 0;
  const auto expect = [&](bool ok, const char* name) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
  };
  e2e::FakeDocument data;
  auto* context = new e2e::FakeContext(&data);
  auto* document = new e2e::FakeDocumentMgr(context);
  auto* manager = new e2e::FakeThreadMgr;
  manager->SetFocus(document);
  auto broker = std::make_unique<PagingBroker>(); auto* engine = broker.get();
  auto* service = new tsf::TextService(std::move(broker));
  expect(SUCCEEDED(service->ActivateEx(manager, 1, 0)), "activate compact candidate fixture");
  auto& window = tsf::CandidateUIRegressionProbe::Window(*service);
  window.SetMetrics(240, 34, 10);
  const auto apply = [&] { tsf::CandidateUIRegressionProbe::Apply(*service, context, engine->Snapshot()); };
  apply();
  expect(window.IsCompact() && window.CompactDigitIndex(0) == std::size_t{0} &&
      window.CompactPageIndex(1) == std::size_t{1}, "narrow native page exposes one numbered candidate");
  const auto tap = [&](WPARAM key) {
    BOOL tested = FALSE, down = FALSE, up = FALSE;
    service->OnTestKeyDown(context, key, 1, &tested);
    service->OnKeyDown(context, key, 1, &down);
    service->OnKeyUp(context, key, static_cast<LPARAM>(1ULL << 31), &up);
    return tested && down && up;
  };
  BOOL predicted = FALSE;
  service->OnTestKeyDown(context, VK_NEXT, 1, &predicted);
  expect(predicted && engine->engine_steps == 0 && window.CompactDigitIndex(0) == std::size_t{0},
      "OnTestKeyDown predicts paging without changing engine or presentation");
  expect(tap(VK_NEXT) && engine->engine_steps == 0 && window.CompactDigitIndex(0) == std::size_t{1},
      "PageDown visits the second visible subpage before touching librime");
  expect(tap('9') && engine->commits == 0 && window.CompactDigitIndex(0) == std::size_t{1},
      "unavailable visible number is owned but cannot commit a hidden candidate");
  expect(tap(VK_NEXT) && engine->engine_steps == 0 && window.CompactDigitIndex(0) == std::size_t{2},
      "PageDown visits the complete engine-page suffix");
  expect(tap(VK_NEXT) && engine->engine_steps == 1 && engine->page == 1 &&
      window.CompactDigitIndex(0) == std::size_t{0}, "only after the last subpage does PageDown advance librime");
  expect(tap(VK_PRIOR) && engine->engine_steps == 2 && engine->page == 0 &&
      window.CompactDigitIndex(0) == std::size_t{2}, "PageUp enters the previous engine page's last visible subpage");
  expect(tap(VK_PRIOR) && engine->engine_steps == 2 && window.CompactDigitIndex(0) == std::size_t{1},
      "reverse paging stays local until reaching the first visible subpage");
  expect(tap('1') && engine->committed_index == 1 && engine->commits == 1 && data.text == L"selected",
      "visible digit one commits engine candidate two with its exact revision/index guard");
  apply();
  BOOL passed = TRUE;
  service->OnTestKeyDown(context, VK_OEM_MINUS, 1, &passed);
  expect(!passed, "compact punctuation remains owned by the user's schema");
  service->OnTestKeyDown(context, VK_RETURN, 1, &passed);
  expect(!passed, "raw Return semantics are unchanged");
  using F = core::InputStateFlags;
  const auto flag = [](F value) { return static_cast<std::uint32_t>(value); };
  engine->page_keys = flag(F::kMinusPageUp) | flag(F::kMinusPagingOnly) | flag(F::kEqualPageDown);
  apply();
  service->OnTestKeyDown(context, VK_OEM_MINUS, 1, &passed);
  expect(!passed, "when paging does not steal minus before the first visible page turn");
  const auto symbol_steps = engine->engine_steps;
  expect(tap(VK_OEM_PLUS) && engine->engine_steps == symbol_steps &&
      window.CompactDigitIndex(0) == std::size_t{1}, "schema-bound equal visits the next subpage first");
  expect(tap(VK_OEM_MINUS) && engine->engine_steps == symbol_steps &&
      window.CompactDigitIndex(0) == std::size_t{0}, "schema-bound minus visits the previous subpage first");
  expect(tap(VK_OEM_MINUS) && engine->engine_steps == symbol_steps,
      "returning to the first subpage preserves the paging gesture without wrapping");
  tap(VK_OEM_PLUS); tap(VK_OEM_PLUS);
  expect(tap(VK_OEM_PLUS) && engine->engine_steps == symbol_steps + 1 && engine->page == 1,
      "schema-bound equal reaches the engine only after all visible subpages");
  expect(tap(VK_OEM_MINUS) && engine->page == 0 && window.CompactDigitIndex(0) == std::size_t{2},
      "schema-bound minus enters the previous engine page at its last subpage");
  engine->page_keys = flag(F::kMinusPageDown); apply();
  const auto reversed_steps = engine->engine_steps;
  expect(tap(VK_OEM_MINUS) && engine->engine_steps == reversed_steps &&
      window.CompactDigitIndex(0) == std::size_t{1}, "an explicit reverse symbol binding keeps its configured direction");
  engine->page_keys = 0; apply();
  service->OnTestKeyDown(context, VK_OEM_MINUS, 1, &passed);
  expect(!passed, "schema changes revoke stale native symbol ownership immediately");
  expect(tap(VK_DOWN) && !window.snapshot().matrix_rows.empty(), "Down opens the candidate matrix for symbol ownership checks");
  const auto matrix_steps = engine->engine_steps;
  for (const auto symbol : {VK_OEM_MINUS, VK_OEM_PLUS}) {
    BOOL tested = TRUE, down = TRUE, up = TRUE;
    service->OnTestKeyDown(context, symbol, 1, &tested);
    service->OnKeyDown(context, symbol, 1, &down);
    service->OnKeyUp(context, symbol, static_cast<LPARAM>(1ULL << 31), &up);
    expect(!tested && !down && !up && engine->engine_steps == matrix_steps && engine->page == 0,
        "expanded matrix leaves unbound or rejected custom symbol actions with the engine");
  }
  engine->page_keys = flag(F::kMinusPageDown) | flag(F::kEqualPageUp);
  apply(); tap(VK_DOWN);
  expect(tap(VK_OEM_MINUS) && engine->page == 1 && !window.snapshot().matrix_rows.empty(),
      "expanded matrix follows a reverse minus PageDown binding");
  expect(tap(VK_OEM_PLUS) && engine->page == 0 && !window.snapshot().matrix_rows.empty(),
      "expanded matrix follows a reverse equal PageUp binding");
  engine->page_keys = 0; apply(); tap(VK_DOWN);
  service->OnTestKeyDown(context, VK_OEM_PLUS, 1, &passed);
  expect(!passed, "changing schema bindings also revokes symbol ownership in expanded mode");
  engine->custom_labels = true; apply();
  service->OnTestKeyDown(context, '1', 1, &passed);
  expect(!passed && !window.CompactDigitIndex(0), "custom selection labels do not acquire numeric shortcuts");
  engine->custom_labels = false; engine->page = 1; apply();
  tap(VK_NEXT); tap(VK_NEXT);
  const auto steps = engine->engine_steps;
  expect(!window.CanPage(1) && tap(VK_NEXT) && engine->engine_steps == steps,
      "known final subpage disables next and never probes or wraps the engine");
  engine->page = 0; apply();
  context->defer_edits = true;
  tsf::CandidateUIRegressionProbe::Page(*service, 1);
  expect(!context->delayed_edits.empty(), "pager callback can wait for a host edit lock");
  service->OnSetFocus(FALSE);
  context->DrainEdits();
  expect(!window.IsVisible() && engine->engine_steps == steps,
      "a delayed pager callback cannot resurrect candidates after focus loss");
  service->Deactivate(); service->Release(); manager->Release(); document->Release(); context->Release();
  return failures;
}
}

int main() {
  using namespace rimes::windows;
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
  int failures = 0;
  auto expect = [&](bool ok, const char* name) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
  };
  e2e::FakeDocument data;
  data.refuse_caret = true; // Windows Runtime edits need not expose an HWND/caret.
  auto* context = new e2e::FakeContext(&data);
  auto* document = new e2e::FakeDocumentMgr(context);
  auto* manager = new e2e::FakeThreadMgr;
  manager->ui_enabled = true;
  manager->SetFocus(document);
  auto* service = new tsf::TextService(std::make_unique<StubBroker>());
  expect(SUCCEEDED(service->ActivateEx(manager, 1, TF_TMAE_UIELEMENTENABLEDONLY)), "activate");
  auto* mode = tsf::CandidateUIRegressionProbe::Mode(*service);
  mode->AddRef(); // A shell may keep its item after the text service ends.
  TF_LANGBARITEMINFO info{};
  expect(mode->GetInfo(&info) == S_OK && info.guidItem == GUID_LBI_INPUTMODE &&
      (info.dwStyle & TF_LBI_STYLE_SHOWNINTRAY), "native input indicator identity");
  auto mode_text = [&] {
    BSTR text = nullptr;
    mode->GetText(&text);
    std::wstring result(text ? text : L""); SysFreeString(text); return result;
  };
  DWORD mode_status = 0;
  mode->GetStatus(&mode_status);
  expect(mode_status & TF_LBI_STATUS_DISABLED, "unknown mode starts disabled");
  tsf::CandidateUIRegressionProbe::Apply(*service, context, {});
  expect(mode_text() == L"中", "mode is obtained from the authoritative broker session");
  BOOL eaten = FALSE;
  service->OnKeyUp(context, VK_SHIFT, 0, &eaten);
  expect(mode_text() == L"英" && !eaten, "Shift updates indicator without eating host modifier");
  mode->OnClick(TF_LBI_CLK_LEFT, {}, nullptr);
  expect(mode_text() == L"中" && data.text.empty(), "indicator click changes mode through a validated edit without inserting text");
  tsf::BrokerInputState state;
  state.has_snapshot = state.composing = state.candidates_visible = true;
  state.composition = L"nihao";
  state.revision = 1;
  state.highlighted_candidate = 0;
  state.candidates = {{0, L"你好", L"", L"1"}, {1, L"拟好", L"", L"2"}};
  expect(SUCCEEDED(tsf::CandidateUIRegressionProbe::Apply(*service, context, state)), "preedit");
  expect(data.composition == L"nihao" && manager->ui_begin == 1 && manager->ui_update == 1,
         "host gets candidates without Win32 caret");
  expect(manager->ui_first_flags == (TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_STRING |
      TF_CLUIE_SELECTION | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE), "initial flags");
  ITfCandidateListUIElementBehavior* element = nullptr;
  if (manager->ui_element) manager->ui_element->QueryInterface(IID_ITfCandidateListUIElementBehavior,
                                              reinterpret_cast<void**>(&element));
  expect(element != nullptr, "candidate behavior interface");
  if (element) {
    UINT count = 0, selected = 0;
    BOOL visible = TRUE;
    element->GetCount(&count);
    element->IsShown(&visible);
    expect(count == 2 && !visible, "host suppresses custom window");
    BSTR text = nullptr;
    expect(element->GetString(0, &text) == S_OK && std::wstring(text) == L"你好", "candidate string");
    SysFreeString(text);
    text = nullptr;
    expect(element->GetString(9, &text) == E_INVALIDARG && !text, "invalid candidate rejected");
    UINT pages[]{0, 1}, returned[2]{};
    expect(element->SetPageIndex(pages, 2) == S_OK, "host pagination");
    expect(element->SetSelection(1) == S_OK, "host changes highlight");
    element->GetSelection(&selected);
    element->GetPageIndex(returned, 2, &count);
    expect(selected == 1 && count == 2 && returned[1] == 1, "selection preserves host pagination");
    expect(element->Finalize() == S_OK && data.text == L"拟好" && !data.composing,
           "host finalization commits once through edit session");
    expect(manager->ui_end == 1 && !manager->ui_element, "end after commit");
    element->GetCount(&count);
    expect(count == 0 && element->Show(TRUE) == TF_E_DISCONNECTED &&
           element->Abort() == TF_E_DISCONNECTED, "retained element is inert and cleared");
    element->Release();
  }
  tsf::CandidateUIRegressionProbe::Apply(*service, context, state);
  service->OnSetFocus(FALSE);
  expect(!manager->ui_element && !data.composing, "focus loss ends host UI");
  manager->ui_fail_begin = true;
  tsf::CandidateUIRegressionProbe::Apply(*service, context, state);
  expect(!manager->ui_element, "failed Begin is not drawn");
  manager->ui_fail_begin = false;
  service->OnSetFocus(FALSE);
  manager->on_ui_begin = [&] { service->OnSetFocus(FALSE); };
  tsf::CandidateUIRegressionProbe::Apply(*service, context, state);
  expect(!manager->ui_element, "reentrant focus loss during Begin does not resurrect UI");
  manager->on_ui_begin = {};
  tsf::CandidateUIRegressionProbe::Apply(*service, context, state);
  manager->on_ui_update = [&] { service->OnSetFocus(FALSE); };
  tsf::CandidateUIRegressionProbe::Apply(*service, context, state);
  expect(!manager->ui_element && !data.composing,
         "reentrant focus loss during Update retires the element");
  manager->on_ui_update = {};
  service->Deactivate();
  expect(mode_text() == L"—" && mode->OnClick(TF_LBI_CLK_LEFT, {}, nullptr) == TF_E_DISCONNECTED,
      "retained indicator clears state and cannot call a retired service");
  mode->Release();
  service->Release();
  manager->Release();
  document->Release();
  context->Release();
  failures += TestNativeCompactPaging();
  CoUninitialize();
  if (!failures) std::cout << "UI-less candidate lifecycle, selection, pagination, stale/reentrant callbacks passed\n";
  return failures ? 1 : 0;
}
