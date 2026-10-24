#include <cstdlib>
#include <iostream>
#include <atomic>
#include <filesystem>
#include <algorithm>

#include "runtime.hpp"
#include "official_features.hpp"
#include "../core/buffer_keys.hpp"
using namespace rimes::windows;
void Check(bool ok, const char* reason) {
  if (!ok) {
    std::cerr << reason << '\n';
    std::exit(1);
  }
}
void TestTargets() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 1;
  auto target = runtime.Register(peer, 1, 1);
  runtime.Focus(target);
  runtime.Toggle(peer);
  Check(runtime.Capturing(target), "explicit binding captures input");
  engine::EngineSnapshot text;
  text.handled = true;
  text.commit_text = "First. ";
  runtime.Capture(target, &text);
  text.commit_text = "Second.";
  runtime.Capture(target, &text);
  Check(text.commit_text.empty(), "captured input does not leak into host");
  runtime.Send(false);
  auto event = runtime.Control({{"op", "wait"}, {"session", 1}}, peer);
  if (event.value("kind", "") == "capture")
    event = runtime.Control({{"op", "wait"}, {"session", 1}}, peer);
  Check(event.value("kind", "") == "deliver",
        "separate notification carries delivery");
  Check(runtime.Snapshot()["source"] == "First. Second.",
        "sending does not consume");
  auto foreign = runtime.Control({{"op", "ack"},
                                  {"session", 1},
                                  {"request", event["request"]},
                                  {"accepted", true}},
                                 peer + 1);
  Check(foreign["kind"] == "disconnected",
        "foreign process cannot acknowledge");
  runtime.Control({{"op", "ack"},
                   {"session", 1},
                   {"request", event["request"]},
                   {"accepted", true}},
                  peer);
  Check(runtime.Snapshot()["source"] == "Second.",
        "actual host acceptance consumes only first block");
  runtime.Control({{"op", "ack"},
                   {"session", 1},
                   {"request", event["request"]},
                   {"accepted", true}},
                  peer);
  Check(runtime.Snapshot()["source"] == "Second.",
        "duplicate ack does not consume again");
  core::KeyEvent key;
  key.virtual_key = VK_RETURN;
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(runtime.BeforeKey(target, key, false), "Return down owned");
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown) |
                    static_cast<unsigned>(core::KeyEventFlags::kRepeat);
  Check(runtime.BeforeKey(target, key, false), "Return repeat owned");
  key.event_flags = 0;
  Check(runtime.BeforeKey(target, key, false), "Return up owned");
  auto other = runtime.Register(peer, 2, 2);
  runtime.Focus(other);
  Check(!runtime.Capturing(target) && !runtime.Capturing(other),
        "focus change pauses without automatic retarget");
  Check(runtime.Snapshot()["target_pid"] == 0,
        "paused snapshot never advertises the saved target as bound");
  runtime.Bind(peer + 2);
  Check(!runtime.Capturing(other), "background target cannot bind from a tray");
  runtime.Focus(other);
  runtime.Bind(peer);
  Check(runtime.Capturing(other), "source click explicitly binds the new field");
  runtime.Bind(peer);
  Check(runtime.Capturing(other) && runtime.Snapshot()["visible"] == true,
        "repeated source clicks keep capture open instead of toggling closed");
  runtime.Remove(other);
  runtime.Bind(peer);
  Check(!runtime.Snapshot()["capture"].get<bool>() &&
            runtime.Snapshot()["target_pid"] == 0,
        "removed context cannot be revived by a click");
  auto own = runtime.Register(GetCurrentProcessId(), 3, 3);
  runtime.Focus(own);
  runtime.Bind(GetCurrentProcessId());
  Check(!runtime.Capturing(own), "settings window never becomes a Buffer target");
  runtime.Close();
  Check(runtime.Snapshot()["source"] == "Second.", "close retains content");
  runtime.Protect();
  Check(!runtime.Snapshot()["visible"].get<bool>(),
        "locked session hides Buffer");
  runtime.Stop();
  std::cout << "Runtime target/control/Return tests passed\n";
}

template <typename Predicate> void Wait(Predicate predicate, const char* reason) {
  const auto start = GetTickCount64();
  while (!predicate() && GetTickCount64() - start < 3000) Sleep(2);
  Check(predicate(), reason);
}
void TestCommandModifiersDoNotBecomeBufferCommands() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 20, 20);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("First. Second.");
  for (const auto modifier : {core::KeyModifiers::kControl,
                             core::KeyModifiers::kAlt,
                             core::KeyModifiers::kWindows,
                             core::KeyModifiers::kAltGr}) {
    for (const auto extra : {0U, static_cast<unsigned>(core::KeyModifiers::kShift)}) {
      for (const auto virtual_key : {VK_RETURN, VK_BACK, VK_ESCAPE}) {
        if (virtual_key == VK_RETURN && modifier == core::KeyModifiers::kAlt && extra)
          continue;  // This exact chord is deliberately a Buffer send action.
        core::KeyEvent key;
        key.virtual_key = static_cast<std::uint32_t>(virtual_key);
        key.modifiers = static_cast<unsigned>(modifier) | extra;
        key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
        Check(!runtime.BeforeKey(target, key, false), "modified command down must pass through");
        key.event_flags |= static_cast<unsigned>(core::KeyEventFlags::kRepeat);
        Check(!runtime.BeforeKey(target, key, false), "modified command repeat must pass through");
        key.event_flags = 0;
        Check(!runtime.BeforeKey(target, key, false), "modified command release must pass through");
        const auto state = runtime.Snapshot();
        Check(state["source"] == "First. Second." && state["result"] == "" &&
              state["capture"] == true && state["visible"] == true &&
              state["status"] == "Buffer", "host shortcut cannot send, edit, or close Buffer");
      }
    }
  }
  // A modified press must not acquire the release after its modifier is gone.
  core::KeyEvent released;
  released.virtual_key = VK_RETURN;
  runtime.BeforeKey(target, released, false);
  Check(runtime.Snapshot()["status"] == "Buffer", "unowned plain release must not deliver");
  runtime.Stop();
}
void TestUnhandledModifierCommitIsCapturedWithoutChangingKeyOwnership() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 26, 26);
  runtime.Focus(target); runtime.Bind(peer);
  engine::EngineSnapshot preedit;
  preedit.handled = true;
  preedit.composing = true;
  preedit.composition = "ni";
  runtime.Capture(target, &preedit);
  Check(runtime.Snapshot()["source"] == "" && runtime.Snapshot()["preedit"] == "ni",
        "uncommitted engine composition is only the Buffer preedit");
  // ascii_composer can commit raw code on a qualified Shift tap while librime
  // still reports that physical modifier as unhandled. The broker's modifier
  // snapshot path supplies this commit without acquiring host key ownership.
  engine::EngineSnapshot modifier;
  modifier.handled = false;
  modifier.commit_text = "ni";
  runtime.Capture(target, &modifier);
  Check(runtime.Snapshot()["source"] == "ni" && runtime.Snapshot()["preedit"] == "" &&
        modifier.commit_text.empty() && !modifier.handled && !modifier.composing,
        "modifier commit enters Buffer once without leaking text or consuming the host key");
  runtime.Capture(target, &modifier);
  Check(runtime.Snapshot()["source"] == "ni" && !modifier.handled,
        "the cleared snapshot cannot append or deliver the same modifier commit twice");
  runtime.Stop();
}
void CheckPausedShiftDoesNotProcessOrChangeDraft(workbench::Runtime& runtime,
                                               workbench::Target target) {
  const auto before = runtime.Snapshot();
  for (const auto virtual_key : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT}) {
    for (const bool composing : {false, true}) {
      core::KeyEvent key;
      key.virtual_key = static_cast<std::uint32_t>(virtual_key);
      key.modifiers = static_cast<unsigned>(core::KeyModifiers::kShift);
      for (const auto flags : {
               static_cast<unsigned>(core::KeyEventFlags::kKeyDown),
               static_cast<unsigned>(core::KeyEventFlags::kKeyDown) |
                   static_cast<unsigned>(core::KeyEventFlags::kRepeat),
               0U}) {
        key.event_flags = flags;
        key.modifiers = flags ? static_cast<unsigned>(core::KeyModifiers::kShift) : 0U;
        // This result stops engine processing. The broker separately preserves
        // physical Shift pass-through; forwarding it to librime could commit
        // raw preedit into a full draft or an immutable pending result.
        Check(runtime.BeforeKey(target, key, composing),
              "paused Buffer must block Shift engine processing");
        const auto after = runtime.Snapshot();
        for (const auto* field : {"source", "result", "source_blocks", "result_blocks",
                                  "preedit", "preview", "busy", "translate", "uncertain",
                                  "capture", "visible", "target_pid"})
          Check(after[field] == before[field],
                "blocked Shift must retain source, results, preedit, and target");
      }
    }
  }
}
void TestFullBufferBlocksShiftWithoutChangingDraft() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 27, 27);
  runtime.Focus(target); runtime.Bind(peer);
  engine::EngineSnapshot preedit;
  preedit.handled = true; preedit.composing = true; preedit.composition = "ni";
  runtime.Capture(target, &preedit);
  runtime.Paste(std::string(workbench::Model::kLimit, 'x'));
  Check(runtime.Snapshot()["source"].get<std::string>().size() == workbench::Model::kLimit,
        "the full-draft fixture reaches the production capacity");
  CheckPausedShiftDoesNotProcessOrChangeDraft(runtime, target);
  runtime.Stop();
}
void TestPendingResultBlocksShiftAndRetainsOriginalAcknowledgement() {
  std::atomic<unsigned> calls{0};
  workbench::Runtime runtime([&](const workbench::Settings&, const workbench::Generation&,
      const std::function<bool(const std::string&)>& chunk,
      const std::function<bool()>&, std::string*) {
    ++calls;
    return chunk("Translated output.");
  });
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 28, 28);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("原文。");
  runtime.Generate(true);
  Wait([&] { return runtime.Snapshot()["result"] == "Translated output."; },
       "the pending-result fixture receives a completed translation");
  engine::EngineSnapshot preedit;
  preedit.handled = true; preedit.composing = true; preedit.composition = "ni";
  runtime.Capture(target, &preedit);
  runtime.Send(false);
  auto delivery = runtime.Control({{"op", "wait"}, {"session", 28}}, peer);
  if (delivery.value("kind", "") == "capture")
    delivery = runtime.Control({{"op", "wait"}, {"session", 28}}, peer);
  Check(delivery["kind"] == "deliver" && delivery["text"] == "Translated output.",
        "the pending-result fixture issues exactly the reviewed result");
  CheckPausedShiftDoesNotProcessOrChangeDraft(runtime, target);
  runtime.Control({{"op", "ack"}, {"session", 28}, {"request", delivery["request"]},
                   {"accepted", true}}, peer);
  const auto accepted = runtime.Snapshot();
  Check(accepted["source"] == "" && accepted["result"] == "" &&
        accepted["status"] == "Delivered" && calls == 1,
        "blocked Shift retains the original result request and linked source for its ack");
  runtime.Control({{"op", "ack"}, {"session", 28}, {"request", delivery["request"]},
                   {"accepted", true}}, peer);
  Check(runtime.Snapshot() == accepted,
        "the original result ack still consumes exactly once after blocked Shift");
  runtime.Stop();
}
void TestOwnedSendKeepsItsLifecycleAcrossModifierChanges() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 21, 21);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("First. Second.");
  core::KeyEvent key;
  key.virtual_key = VK_RETURN;
  key.modifiers = core::ShortcutModifiers(MOD_ALT | MOD_SHIFT);
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(runtime.BeforeKey(target, key, false), "send chord press is owned");
  key.modifiers = static_cast<unsigned>(core::KeyModifiers::kControl);
  key.event_flags |= static_cast<unsigned>(core::KeyEventFlags::kRepeat);
  Check(runtime.BeforeKey(target, key, false), "owned Return repeat survives modifier change");
  Check(runtime.Snapshot()["status"] == "Buffer", "repeat cannot send a block early");
  key.event_flags = 0;
  Check(runtime.BeforeKey(target, key, false), "owned Return release survives modifier change");
  auto delivery = runtime.Control({{"op", "wait"}, {"session", 21}}, peer);
  if (delivery.value("kind", "") == "capture")
    delivery = runtime.Control({{"op", "wait"}, {"session", 21}}, peer);
  Check(delivery["kind"] == "deliver" && delivery["text"] == "First. ",
        "owned Return sends exactly the first block");
  runtime.Control({{"op", "ack"}, {"session", 21}, {"request", delivery["request"]},
                   {"accepted", true}}, peer);
  Check(runtime.Snapshot()["source"] == "Second.", "ack consumes exactly one owned block");
  key.modifiers = 0;
  runtime.BeforeKey(target, key, false);
  Check(runtime.Snapshot()["status"] == "Delivered" &&
        runtime.Snapshot()["source"] == "Second.", "late release cannot deliver the remainder");
  runtime.Stop();
}
void TestAltShiftReturnTapHoldAndComposition() {
  std::uint64_t now = 1000;
  workbench::Runtime runtime({}, {}, [&] { return now; });
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 30, 30);
  runtime.Focus(target); runtime.Bind(peer);
  auto commit = [&](const char* value) {
    engine::EngineSnapshot snapshot;
    snapshot.commit_text = value;
    runtime.Capture(target, &snapshot);
  };
  commit("一"); commit("二"); commit("三");
  core::KeyEvent key;
  key.virtual_key = VK_RETURN;
  key.modifiers = core::ShortcutModifiers(MOD_ALT | MOD_SHIFT);
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(core::IsBufferSendChord(VK_RETURN, key.modifiers), "TSF and Broker recognize the same Alt+Shift+Enter chord");
  Check(runtime.BeforeKey(target, key, false), "Alt+Shift+Enter down owned");
  Check(runtime.Snapshot()["send_hold_progress"] == 0.0, "hold feedback starts with the physical key");
  key.modifiers = 0; key.event_flags = 0;
  Check(runtime.BeforeKey(target, key, false), "modifier released first does not lose the owned Return up");
  Check(runtime.Snapshot()["send_hold_progress"] == -1.0, "tap release removes hold feedback");
  auto take = [&] {
    auto event = runtime.Control({{"op", "wait"}, {"session", 30}}, peer);
    if (event.value("kind", "") == "capture")
      event = runtime.Control({{"op", "wait"}, {"session", 30}}, peer);
    Check(event.value("kind", "") == "deliver", "send gesture emits a delivery");
    return event;
  };
  auto ack = [&](const core::Json& delivery) {
    runtime.Control({{"op", "ack"}, {"session", 30}, {"request", delivery["request"]},
                     {"accepted", true}}, peer);
  };
  auto first = take();
  Check(first["text"] == "一", "Alt+Shift+Enter tap sends one commit block");
  ack(first);
  Check(runtime.Snapshot()["source"] == "二三", "tap keeps unsent suffix");
  key.modifiers = core::ShortcutModifiers(MOD_ALT | MOD_SHIFT);
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  runtime.BeforeKey(target, key, false);
  now += runtime.Configuration().send_hold_ms / 2;
  runtime.Tick();
  Check(runtime.Snapshot()["send_hold_progress"] == 0.5 && !runtime.Snapshot()["pending"].get<bool>(),
        "half hold displays half progress without early delivery");
  now += runtime.Configuration().send_hold_ms / 2 + 1;
  runtime.Tick();
  Check(runtime.Snapshot()["send_hold_progress"] == 1.0, "completed hold shows full progress");
  auto second = take();
  Check(second["text"] == "二", "held Alt+Shift+Enter starts ordered drain");
  ack(second); auto third = take();
  Check(third["text"] == "三", "hold advances only after the previous ack");
  ack(third);
  key.event_flags = 0; runtime.BeforeKey(target, key, false);
  Check(runtime.Snapshot()["source"] == "", "hold key-up cannot send twice");
  Check(runtime.Snapshot()["send_hold_progress"] == -1.0, "hold release clears the completed strip");
  commit("保留");
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(!runtime.BeforeKey(target, key, true), "composing chord is left to Rime settlement");
  Check(runtime.Snapshot()["send_hold_progress"] == -1.0, "composition settlement never displays send progress");
  key.event_flags |= static_cast<unsigned>(core::KeyEventFlags::kRepeat);
  runtime.BeforeKey(target, key, false);
  key.event_flags = 0; runtime.BeforeKey(target, key, false);
  Check(runtime.Snapshot()["source"] == "保留" && runtime.Snapshot()["status"] != "Sending...",
        "settling composition never arms a send on the same physical press");
  runtime.Close();
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(!runtime.BeforeKey(target, key, false), "closed Buffer leaves Alt+Shift+Enter with the host");
  runtime.Stop();
}
void TestTranslationModeArmsAutomaticSentencePipeline() {
  std::atomic<unsigned> calls{0};
  std::uint64_t now = 1000;
  workbench::Runtime runtime([&](const workbench::Settings& config, const workbench::Generation& job,
      const std::function<bool(const std::string&)>& chunk,
      const std::function<bool()>&, std::string*) {
    ++calls;
    Check(job.translation && !job.source.empty(), "automatic job includes unfinished source");
    return chunk(config.target_language == "Chinese" ? "译文。" : "A sentence.");
  }, {}, [&] { return now; });
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 31, 31);
  runtime.Focus(target); runtime.Bind(peer); runtime.SelectTranslationMode();
  Check(runtime.Snapshot()["translate"] == true && calls == 0,
        "selecting Translation arms the empty workspace without a Generate click");
  runtime.Paste("完整句"); runtime.Tick();
  now += 299; runtime.Tick(); Check(calls == 0, "translation honors the 300ms debounce");
  now += 1; runtime.Tick();
  Wait([&] { return runtime.Snapshot()["result"] == "A sentence."; },
       "unfinished input automatically produces a translation after 300ms");
  runtime.Paste("。");
  Check(runtime.Snapshot()["result"] == "", "finishing the tail invalidates its former translation");
  now += 300; runtime.Tick();
  Wait([&] { return runtime.Snapshot()["result"] == "A sentence."; }, "changed sentence is translated again");
  const auto old_copy = runtime.PrepareCopy();
  Check(runtime.SetTranslationLanguages("English", "Chinese"), "toolbar swaps and persists the explicit pair");
  Check(!runtime.ValidateCopy(old_copy) && runtime.Snapshot()["capture"] == true &&
        runtime.Snapshot()["result"] == "", "language switch preserves focus and rejects the old result copy");
  now += 300; runtime.Tick();
  Wait([&] { return runtime.Snapshot()["result"] == "译文。"; }, "language switch retranslates current source");
  runtime.SetTranslationSpeech(true);
  const auto block = runtime.Snapshot()["result_blocks"][0].value("id", 0ULL);
  runtime.RequestTranslationSpeech(block);
  const auto speech = runtime.TakeTranslationSpeech();
  Check(speech["text"] == "译文。" && speech["language"] == "Chinese", "speech uses the clicked result's own language");
  runtime.RequestTranslationSpeech(block); runtime.SetTranslationSpeech(false);
  Check(runtime.TakeTranslationSpeech().is_null(), "turning speech off discards queued plaintext");
  runtime.Paste("继续"); runtime.PauseCapture(); now += 900; runtime.Tick();
  Check(calls == 3, "paused capture cannot start automatic network work");
  runtime.DiscardBuffer(); runtime.Bind(peer); runtime.SelectTranslationMode();
  runtime.Paste("持续输入"); runtime.Tick();
  for (int i = 0; i < 4; ++i) { now += 200; runtime.Paste("字"); runtime.Tick(); }
  Check(calls == 3, "continuous typing keeps the trailing debounce pending");
  now += 100; runtime.Paste("字"); runtime.Tick();
  Wait([&] { return calls == 4 && !runtime.Snapshot()["busy"].get<bool>(); }, "900ms maximum wait prevents starvation");
  Check(runtime.SetTranslationLanguages("Chinese", "English"), "restore default translation pair");
  runtime.ReturnToInput(); runtime.Tick();
  Check(calls == 4 && runtime.Snapshot()["translate"] == false, "input mode disarms automatic translation");
  runtime.Stop();
}
void TestTranslationLanguageChangeRejectsLateOutput() {
  std::atomic<bool> entered{false}, release{false}, stale_accepted{true};
  std::atomic<unsigned> calls{0};
  std::uint64_t now = 1000;
  workbench::Runtime runtime([&](const workbench::Settings& config, const workbench::Generation& job,
      const std::function<bool(const std::string&)>& chunk,
      const std::function<bool()>& cancelled, std::string*) {
    if (++calls == 1) {
      entered = true;
      while (!release) Sleep(1);
      Check(cancelled(), "changing language cancels the frozen provider request");
      stale_accepted = chunk("Obsolete English.");
      return false;
    }
    Check(config.source_language == "English" && config.target_language == "Chinese" &&
          job.target_language == "Chinese", "next request freezes the new pair");
    return chunk("新译文。");
  }, {}, [&] { return now; });
  runtime.Bind(0); runtime.SelectTranslationMode(); runtime.Paste("Original text");
  now += 300; runtime.Tick();
  Wait([&] { return entered.load(); }, "explicit paste translates without a bound host");
  Check(runtime.SetTranslationLanguages("English", "Chinese"), "change language while old request is running");
  release = true; now += 300; runtime.Tick();
  Wait([&] { return runtime.Snapshot()["result"] == "新译文。"; }, "new-language result replaces the cancelled request");
  Check(!stale_accepted && !runtime.Snapshot()["pending"].get<bool>(), "late output never publishes or delivers");
  Check(runtime.SetTranslationLanguages("Chinese", "English"), "restore default pair");
}

void TestTranslationSpeechAcknowledgementAndProtection() {
  workbench::Runtime runtime([](const workbench::Settings&, const workbench::Generation&,
      const std::function<bool(const std::string&)>& chunk,
      const std::function<bool()>&, std::string*) { return chunk("Spoken result."); });
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 331, 331);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("原文。");
  runtime.SelectTranslationMode(); runtime.SetTranslationSpeech(true); runtime.SetAutoSend(0, true);
  runtime.Generate(true);
  Wait([&] { return runtime.Snapshot()["result"] == "Spoken result."; }, "speech delivery fixture");
  runtime.Send(false);
  auto delivery = runtime.Control({{"op", "wait"}, {"session", 331}}, peer);
  while (delivery.value("kind", "") != "deliver")
    delivery = runtime.Control({{"op", "wait"}, {"session", 331}}, peer);
  Check(runtime.TakeTranslationSpeech().is_null(), "unacknowledged insertion is not read aloud");
  Check(!runtime.SetTranslationLanguages("English", "Chinese"), "issued delivery freezes its language pair");
  runtime.Control({{"op", "ack"}, {"session", 331}, {"request", delivery["request"]}, {"accepted", true}}, peer);
  Check(runtime.Snapshot()["visible"] == false && runtime.TakeTranslationSpeech()["text"] == "Spoken result.",
        "last accepted block can be read even when close-after-send hides Buffer");
  runtime.Control({{"op", "ack"}, {"session", 331}, {"request", delivery["request"]}, {"accepted", true}}, peer);
  Check(runtime.TakeTranslationSpeech().is_null(), "duplicate ack cannot replay speech");
  runtime.Protect();
  Check(runtime.Snapshot()["speech_allowed"] == false && runtime.TakeTranslationSpeech().is_null(),
        "session protection revokes speech including the final delivered block");
  runtime.SetTranslationSpeech(false); runtime.SetAutoSend(0, false);
}

void TestBackgroundTranslationResources() {
  std::atomic<unsigned> downloads{0}, generations{0};
  std::atomic<bool> ready{false}, release{false}, cancelled_download{false};
  workbench::Runtime runtime(
      [&](const workbench::Settings&, const workbench::Generation& job,
          const std::function<bool(const std::string&)>& chunk,
          const std::function<bool()>&, std::string*) {
        ++generations;
        Check(job.source == "First latest.", "resource completion translates current source, never a stale request");
        return chunk("Translation.");
      },
      {[&] { return ready.load(); },
       [&](const std::function<bool()>& cancelled, std::string*) {
         ++downloads;
         while (!release && !cancelled()) Sleep(2);
         if (cancelled()) { cancelled_download = true; return false; }
         ready = true; return true;
       }});
  auto config = runtime.Configuration();
  config.translation_provider = "local";
  Check(runtime.Configure(config, {}, false, nullptr), "enable local resource preparation");
  Wait([&] { return downloads == 1; }, "local resources prefetch without opening Buffer");
  Check(runtime.Snapshot()["visible"] == false, "background download never opens Buffer");
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 32, 32);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("First ");
  runtime.Generate(true);
  Check(!runtime.Snapshot()["busy"].get<bool>() && generations == 0,
        "resource preparation is not a generation and never publishes download text");
  Check(runtime.Snapshot()["status"].get<std::string>().find("下载") == std::string::npos &&
        runtime.PluginStatus().find("后台下载") != std::string::npos,
        "resource progress belongs to settings only");
  runtime.Paste("latest."); runtime.Close(); runtime.Cancel();
  Check(!cancelled_download, "closing or editing Buffer does not cancel resources");
  release = true;
  Wait([&] { return runtime.PluginStatus().find("已就绪") != std::string::npos; }, "resource preparation finishes independently");
  runtime.Bind(peer); runtime.Generate(true);
  Wait([&] { return runtime.Snapshot()["result"] == "Translation."; }, "prepared translator produces actual text");
  Check(downloads == 1 && !cancelled_download && generations == 1, "one resource worker serves all requests");
  runtime.Stop();
}

void TestConfiguredSendAndCancellation() {
  workbench::Runtime runtime;
  const auto original = runtime.Configuration();
  auto config = original;
  config.send_key = VK_F8; config.send_modifiers = MOD_CONTROL | MOD_SHIFT;
  Check(runtime.Configure(config, {}, false, nullptr), "save custom send chord");
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 37, 37);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("First. Second.");
  auto capture = runtime.Control({{"op", "wait"}, {"session", 37}}, peer);
  Check(capture["sendKey"] == VK_F8 && capture["sendModifiers"] == (MOD_CONTROL | MOD_SHIFT),
        "the exact saved shortcut travels with capture notifications to TSF prediction");
  core::KeyEvent key;
  key.virtual_key = VK_RETURN;
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(runtime.BeforeKey(target, key, false), "ordinary Enter stays in Buffer");
  key.event_flags = 0; runtime.BeforeKey(target, key, false);
  Check(runtime.Snapshot()["status"] == "Buffer", "ordinary Enter cannot send staged blocks");
  key.modifiers = core::ShortcutModifiers(MOD_ALT | MOD_SHIFT);
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(!runtime.BeforeKey(target, key, false), "replaced default chord is no longer intercepted");
  key.virtual_key = VK_F8;
  key.modifiers = core::ShortcutModifiers(config.send_modifiers);
  Check(runtime.IsSendChord(key) && runtime.BeforeKey(target, key, false), "custom non-Enter key arms delivery");
  key.modifiers = 0; key.event_flags = 0;
  Check(runtime.BeforeKey(target, key, false), "custom physical release is owned after modifiers lift");
  auto delivery = runtime.Control({{"op", "wait"}, {"session", 37}}, peer);
  Check(delivery["kind"] == "deliver" && delivery["text"] == "First. ", "custom tap sends one block");
  runtime.Control({{"op", "ack"}, {"session", 37}, {"request", delivery["request"]}, {"accepted", true}}, peer);
  for (const bool configure : {false, true}) {
    key.modifiers = core::ShortcutModifiers(config.send_modifiers);
    key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
    Check(runtime.BeforeKey(target, key, false), "arm next custom gesture");
    if (configure) Check(runtime.Configure(config, {}, false, nullptr), "configuration cancels the held gesture");
    else runtime.Focus({});
      Check(runtime.Snapshot()["send_hold_progress"] == -1.0, "focus and configuration cancellation clear hold feedback");
    runtime.Focus(target); runtime.Bind(peer);
    Sleep(1250); runtime.Tick();
    key.event_flags = 0;
    runtime.BeforeKey(target, key, false);
    Check(runtime.Snapshot()["source"] == "Second." && runtime.Snapshot()["status"] != "Sending...",
          "returning to the same target never revives a cancelled hold");
  }
  key.modifiers = core::ShortcutModifiers(config.send_modifiers);
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(!runtime.BeforeKey(target, key, true), "composing custom chord requests Rime settlement only");
  key.event_flags |= static_cast<unsigned>(core::KeyEventFlags::kRepeat);
  Check(runtime.BeforeKey(target, key, false), "composition-settling repeat remains owned");
  key.event_flags = 0; runtime.BeforeKey(target, key, false);
  Check(runtime.Snapshot()["source"] == "Second." && runtime.Snapshot()["status"] != "Sending...",
        "custom settling key cannot deliver on the same release");
  Check(runtime.Configure(original, {}, false, nullptr), "restore isolated configuration");
}
void TestOpeningGeometryIsBoundToOneExactSummon() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 32, 32);
  runtime.Focus(target); runtime.Bind(peer);
  auto geometry = [&] {
    return core::Json{{"op", "geometry"}, {"session", 32}, {"context", 32},
        {"openingRevision", runtime.Snapshot()["opening_revision"]},
        {"caret", {400, 500, 0, 22}}};
  };
  auto request = geometry();
  Check(runtime.Control(request, peer + 1)["kind"] == "disconnected" &&
        !runtime.PlacementCaret(target), "foreign process cannot supply opening geometry");
  request["context"] = 33;
  Check(runtime.Control(request, peer)["kind"] == "stale", "geometry cannot cross TSF contexts");
  request = geometry(); request["caret"] = {0, 0, 0, 0};
  Check(runtime.Control(request, peer)["kind"] == "error", "dummy zero caret does not move Buffer");
  request = geometry();
  Check(runtime.Control(request, peer)["kind"] == "ok" &&
        runtime.PlacementCaret(target) == workbench::BufferRect{400, 500, 0, 22},
        "fresh zero-width TSF caret is valid");
  const auto placed = runtime.Snapshot()["placement_revision"];
  Check(runtime.Control(request, peer)["kind"] == "stale" &&
        runtime.Snapshot()["placement_revision"] == placed,
        "each summon accepts geometry once, never follows typing");
  runtime.Bind(peer);
  Check(!runtime.PlacementCaret(target) && runtime.Control(request, peer)["kind"] == "stale",
        "a new summon cannot reuse the last opening's geometry");
  runtime.CancelOpeningPlacement(); request = geometry();
  Check(runtime.Control(request, peer)["kind"] == "stale",
        "manual dragging defeats a late opening reply");
  runtime.Bind(peer); request = geometry(); runtime.Paste("typed before geometry arrived");
  Check(runtime.Control(request, peer)["kind"] == "stale", "late geometry cannot jump the panel after typing");
  runtime.Bind(peer); request = geometry();
  const auto other = runtime.Register(peer, 33, 33);
  runtime.Focus(other);
  Check(runtime.Control(request, peer)["kind"] == "stale" && !runtime.PlacementCaret(target),
        "same-process focus change revokes old opening geometry");
  runtime.Stop();
}
void TestPluginAuthority() {
  std::atomic<unsigned> calls{0}, completed{0};
  std::atomic<bool> hold{false}, release{false}, accepted{false}, correct_instruction{false};
  workbench::Runtime runtime([&](const workbench::Settings&, const workbench::Generation& job,
      const std::function<bool(const std::string&)>& chunk, const std::function<bool()>& cancelled, std::string*) {
    ++calls;
    correct_instruction = job.instruction.find("English") != std::string::npos;
    if (hold) {
      const auto start = GetTickCount64();
      while (!release && GetTickCount64() - start < 2500) Sleep(2);
    }
    accepted = chunk("First. Second.");
    const bool ok = accepted && !cancelled();
    ++completed;
    return ok;
  });
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 10, 10);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("第一句。第二句。");
  runtime.Generate(false);
  Check(!runtime.Snapshot()["busy"].get<bool>() && calls == 0, "absent optional plugin cannot dispatch a network job");
  runtime.Generate(true);
  Wait([&] { return !runtime.Snapshot()["busy"].get<bool>(); }, "translation completion");
  Check(calls == 1 && correct_instruction && runtime.Snapshot()["result"] == "First. Second.", "verified package supplies runtime instructions");
  std::string error;
  Check(runtime.ManagePlugin(official::kTranslation, "disable", &error), "disable completed plugin");
  Check(runtime.Snapshot()["result"] == "" && runtime.Snapshot()["source"] == "第一句。第二句。", "disable clears old result and retains source");
  Check(runtime.ManagePlugin(official::kTranslation, "enable", &error), "reenable translation");
  hold = true; runtime.Generate(true);
  Wait([&] { return calls == 2; }, "in-flight mock started");
  Check(runtime.ManagePlugin(official::kTranslation, "disable", &error), "disable during generation");
  Check(runtime.ManagePlugin(official::kTranslation, "enable", &error), "re-enable during generation");
  release = true;
  Wait([&] { return completed == 2; }, "late callback completed");
  Check(!accepted && runtime.Snapshot()["result"] == "", "re-enable cannot authorize an old in-flight callback");
  runtime.Stop();
  // Destructor joins the test transport; its callback must reject the stale
  // grant even though the exact same package has already been re-enabled.
}
void TestReturningToInputRejectsLateResultsAndDropsQueuedTranslation() {
  std::atomic<unsigned> calls{0}, completed{0};
  std::atomic<bool> release{false}, stale_cancelled{false}, stale_accepted{false};
  workbench::Runtime runtime([&](const workbench::Settings&, const workbench::Generation&,
      const std::function<bool(const std::string&)>& chunk,
      const std::function<bool()>& cancelled, std::string*) {
    ++calls;
    chunk("Partial output.");
    while (!release) Sleep(2);
    stale_cancelled = cancelled();
    stale_accepted = chunk("Late output.");
    ++completed;
    return true;
  });
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 22, 22);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("第一句。第二句。");
  runtime.Generate(true);
  Wait([&] { return calls == 1 && runtime.Snapshot()["status"] == "Receiving..."; },
       "fake translation starts streaming");
  runtime.ReturnToInput();
  Check(!runtime.Snapshot()["translate"].get<bool>() && !runtime.Snapshot()["busy"].get<bool>() &&
        runtime.Snapshot()["status"] == "Processing stopped. Source retained.",
        "returning to input stops automatic translation and retires generation");
  // A second explicit request queues behind the first provider. A subsequent
  // mode selection must cancel that queue before the provider can dispatch it.
  runtime.Generate(true);
  Check(runtime.Snapshot()["busy"] == true && calls == 1,
        "second translation is queued");
  runtime.ReturnToInput();
  Check(runtime.Snapshot()["status"] == "Processing stopped. Source retained.",
        "cancelled queued work no longer claims to be waiting for a response");
  release = true;
  Wait([&] { return completed == 1; }, "stale fake provider returns");
  Sleep(850); runtime.Tick(); Sleep(50);
  const auto state = runtime.Snapshot();
  Check(calls == 1 && stale_cancelled && !stale_accepted,
        "mode switch drops queued work and rejects stale provider callbacks");
  Check(state["source"] == "第一句。第二句。" && state["result"] == "" &&
        state["preview"] == "" && state["busy"] == false && state["translate"] == false &&
        state["status"] == "Processing stopped. Source retained." &&
        state["capture"] == true && state["visible"] == true,
        "returning to input retains the draft and binding without restarting requests");
  runtime.Stop();
  runtime.ReturnToInput();
  Check(runtime.Snapshot()["status"] == "Buffer reset" &&
        runtime.Snapshot()["source"] == "" && runtime.Snapshot()["result"] == "",
        "mode selection cannot revive content discarded by IME shutdown");
}
void TestReturningToInputRetainsCompletedResultsAndAnIssuedDelivery() {
  std::atomic<unsigned> calls{0};
  workbench::Runtime runtime([&](const workbench::Settings&, const workbench::Generation&,
      const std::function<bool(const std::string&)>& chunk,
      const std::function<bool()>&, std::string*) {
    ++calls;
    return chunk("Translated output.");
  });
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 23, 23);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("原文。");
  runtime.Generate(true);
  Wait([&] { return runtime.Snapshot()["result"] == "Translated output."; }, "completed result arrives");
  runtime.ReturnToInput();
  Check(runtime.Snapshot()["source"] == "原文。" &&
        runtime.Snapshot()["result"] == "" && runtime.Snapshot()["plugin"] == "",
        "input mode shows source while the plugin retains reviewed output");
  Sleep(850); runtime.Tick();
  Check(calls == 1 && runtime.Snapshot()["translate"] == false,
        "selecting input does not resume or start automatic requests");
  Check(runtime.SelectBufferPlugin(official::kTranslation) && runtime.Snapshot()["result"] == "Translated output.",
        "returning to the owner restores its reviewed output");
  runtime.Send(false);
  auto delivery = runtime.Control({{"op", "wait"}, {"session", 23}}, peer);
  if (delivery.value("kind", "") == "capture")
    delivery = runtime.Control({{"op", "wait"}, {"session", 23}}, peer);
  Check(delivery["kind"] == "deliver" && delivery["text"] == "Translated output.",
        "retained result is delivered only by explicit Send");
  runtime.ReturnToInput();
  Check(runtime.Snapshot()["source"] == "原文。" &&
        runtime.Snapshot()["result"] == "Translated output." &&
        runtime.Snapshot()["status"] == "Sending...", "mode switch preserves issued delivery metadata");
  runtime.Paste("Rejected edit.");
  core::KeyEvent pending_backspace;
  pending_backspace.virtual_key = VK_BACK;
  pending_backspace.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  runtime.BeforeKey(target, pending_backspace, false);
  Check(runtime.Snapshot()["source"] == "原文。" &&
        runtime.Snapshot()["result"] == "Translated output.",
        "editing cannot alter an already-issued result delivery or its linked source");
  runtime.Control({{"op", "ack"}, {"session", 23}, {"request", delivery["request"]},
                   {"accepted", true}}, peer);
  Check(runtime.Snapshot()["source"] == "" && runtime.Snapshot()["result"] == "" &&
        runtime.Snapshot()["status"] == "Delivered", "the original ack consumes the issued result exactly once");
  runtime.Control({{"op", "ack"}, {"session", 23}, {"request", delivery["request"]},
                   {"accepted", true}}, peer);
  Check(runtime.Snapshot()["source"] == "" && runtime.Snapshot()["result"] == "",
        "duplicate ack after selection cannot repeat delivery");
  runtime.Stop();
}
void TestReturningToInputInvalidatesReadyResultOnRealEdit() {
  for (const bool backspace : {false, true}) {
    std::atomic<unsigned> calls{0};
    workbench::Runtime runtime([&](const workbench::Settings&, const workbench::Generation&,
        const std::function<bool(const std::string&)>& chunk,
        const std::function<bool()>&, std::string*) {
      ++calls;
      return chunk("Reviewed output.");
    });
    const auto peer = GetCurrentProcessId() + 1;
    const std::uint64_t session = backspace ? 25 : 24;
    const auto target = runtime.Register(peer, session, session);
    runtime.Focus(target); runtime.Bind(peer); runtime.Paste("原文。尾");
    runtime.Generate(true);
    Wait([&] { return runtime.Snapshot()["result"] == "Reviewed output."; },
         "completed output arrives before changing mode");
    runtime.ReturnToInput();
    Check(runtime.Snapshot()["result"] == "",
          "input mode does not expose another plugin result");
    if (backspace) {
      core::KeyEvent key;
      key.virtual_key = VK_BACK;
      key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
      Check(runtime.BeforeKey(target, key, false), "plain Backspace edits the bound source");
    } else {
      runtime.Paste("追加");
    }
    const auto state = runtime.Snapshot();
    Check(state["source"] == (backspace ? "原文。" : "原文。尾追加") &&
          state["result"] == "" && state["preview"] == "" &&
          state["translate"] == false && state["busy"] == false,
          "a real input-mode edit invalidates old result and linked translation state");
    runtime.Send(false);
    auto delivery = runtime.Control({{"op", "wait"}, {"session", session}}, peer);
    if (delivery.value("kind", "") == "capture")
      delivery = runtime.Control({{"op", "wait"}, {"session", session}}, peer);
    Check(delivery["kind"] == "deliver" && delivery["text"] == "原文。" && calls == 1,
          "Send after editing uses current source without requesting or sending old output");
    runtime.Stop();
  }
}
void TestChordFallback() {
  workbench::Runtime runtime;
  auto settings = runtime.Configuration(); settings.schema = "my_combo";
  std::string error;
  Check(runtime.Configure(settings, L"", false, &error), "save chord selection");
  Check(runtime.Configuration().schema == "my_combo", "enabled chord selected");
  Check(runtime.ManagePlugin(official::kChord, "disable", &error), "disable chord");
  Check(runtime.Configuration().schema == "rime_ice", "disabled chord falls back to full pinyin");
  Check(runtime.ManagePlugin(official::kChord, "enable", &error), "re-enable chord");
  Check(runtime.Configuration().schema == "my_combo", "re-enable preserves the saved choice");
}
void TestExplicitOpeningPlacementRevision() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 901, 902);
  Check(!runtime.PlacementTarget(), "hidden buffer has no geometry authority");
  runtime.Focus(target);
  runtime.Bind(peer);
  const auto first = runtime.Snapshot()["opening_revision"].get<std::uint64_t>();
  Check(first > 0 && runtime.PlacementTarget() == target,
        "explicit opening publishes its exact live target");
  runtime.PublishCaret({300, 240, 301, 258}); runtime.PublishCaret({340, 240, 341, 258});
  auto geometry_event = runtime.Control({{"op", "wait"}, {"session", 901}}, peer);
  if (geometry_event["kind"] == "capture") geometry_event = runtime.Control({{"op", "wait"}, {"session", 901}}, peer);
  Check(geometry_event["kind"] == "buffer_caret" && geometry_event["rect"][0] == 340 &&
        geometry_event["context"] == 902 && geometry_event["openingRevision"] == first,
        "Buffer caret updates coalesce and retain exact session/context/opening identity");
  runtime.Paste("Synthetic geometry test.");
  Check(runtime.Snapshot()["opening_revision"] == first,
        "ordinary content updates cannot reposition a manually placed buffer");
  runtime.PauseCapture();
  Check(!runtime.PlacementTarget(), "paused capture cannot probe an old input target");
  runtime.Bind(peer);
  Check(runtime.Snapshot()["opening_revision"] == first + 1,
        "explicit rebind creates a fresh opening placement");
  const auto other = runtime.Register(peer, 903, 904);
  runtime.Focus(other);
  Check(!runtime.PlacementTarget(), "new context revokes old placement target");
  runtime.Close();
  Check(!runtime.PlacementTarget(), "closed buffer has no placement target");
}
void TestCodexConnectorAndReturn() {
  std::atomic<unsigned> calls{0};
  std::atomic<bool> release{false}, cancelled_old{false};
  workbench::Runtime runtime([&](const workbench::Settings& config, const workbench::Generation& job,
      const std::function<bool(const std::string&)>& chunk,
      const std::function<bool()>& cancelled, std::string*) {
    Check(config.ai_connector == "codex-cli" &&
        job.plugin_id == (job.translation ? official::kTranslation : official::kCodex) &&
        !job.plugin_grant.empty() && job.connector_id == official::kCodex && !job.connector_grant.empty(), "Codex selected and granted");
    ++calls;
    while (!release && !cancelled()) Sleep(2);
    if (cancelled()) { cancelled_old = true; return false; }
    return chunk("Generated only.");
  });
  auto config = runtime.Configuration();
  Check(workbench::SelectConnection(&config, "codex"), "select saved Codex profile");
  config.translation_provider = "ai";
  Check(runtime.Configure(config, {}, false, nullptr), "save Codex configuration");
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 950, 951);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("Original request."); runtime.SelectAIMode();
  core::KeyEvent key; key.virtual_key = VK_RETURN;
  key.modifiers = core::ShortcutModifiers(MOD_ALT | MOD_SHIFT);
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(runtime.BeforeKey(target, key, false), "AI Return requests generation");
  Wait([&] { return calls == 1; }, "first Codex job issued");
  release = true;
  Wait([&] { return runtime.Snapshot()["result"] == "Generated only."; }, "fake CLI result ready");
  key.event_flags = 0; Check(runtime.BeforeKey(target, key, false), "generation key-up owned");
  Check(runtime.Snapshot()["status"] == "Ready" && runtime.Snapshot()["source"] == "Original request.", "generation press never sends on release");
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown); runtime.BeforeKey(target, key, false);
  key.event_flags = 0; runtime.BeforeKey(target, key, false);
  auto event = runtime.Control({{"op", "wait"}, {"session", 950}}, peer);
  if (event["kind"] == "capture") event = runtime.Control({{"op", "wait"}, {"session", 950}}, peer);
  Check(event["kind"] == "deliver" && event["text"] == "Generated only.", "second Return sends result, not request");
  runtime.Control({{"op", "ack"}, {"session", 950}, {"request", event["request"]}, {"accepted", true}}, peer);
  Check(runtime.Snapshot()["source"] == "" && runtime.Snapshot()["result"] == "", "final accepted result consumes frozen source");
  release = false; runtime.Paste("Another request."); runtime.Generate(false);
  Wait([&] { return calls == 2; }, "next Codex job started");
  runtime.Close(); Wait([&] { return cancelled_old.load(); }, "close cancels producer");
  Check(runtime.Snapshot()["result"] == "" && runtime.Snapshot()["source"] == "Another request.", "close cannot publish stale result or lose request");
  runtime.Bind(peer);
  Check(runtime.ManagePlugin(official::kCodex, "disable", nullptr), "disable connector");
  runtime.Generate(true);
  Check(!runtime.Snapshot().value("busy", false) && calls == 2, "translation cannot bypass a disabled connector");
  Check(runtime.ManagePlugin(official::kCodex, "enable", nullptr), "enable connector");
  release = true; runtime.Generate(true);
  Wait([&] { return runtime.Snapshot()["result"] == "Generated only."; }, "translation uses selected Codex connector");
  Check(runtime.ManagePlugin(official::kCodex, "disable", nullptr), "revoke completed connector result");
  Check(runtime.Snapshot()["result"] == "" && runtime.Snapshot()["source"] == "Another request.", "connector revocation rejects completed results from another plugin owner");
  runtime.Stop();
}
void TestStreamingInferenceAndRetirement() {
  std::atomic<bool> entered=false, release=false, finished=false, stale_body=true, stale_summary=true;
  workbench::Runtime runtime([&](const workbench::Settings&, const workbench::Generation& job,
      const std::function<bool(const std::string&)>&, const std::function<bool()>&, std::string*) {
    Check(job.model == "gpt-6-astra" && job.effort == "high", "inference profile is frozen for this request");
    Check(job.progress(true,"公开思考摘要") && job.snapshot("First. Second."), "live observers accept current request");
    entered=true; while (!release) Sleep(2);
    stale_body=job.snapshot("Late answer."); stale_summary=job.progress(true,"late summary"); finished=true; return true;
  });
  Check(runtime.ManagePlugin(official::kCodex,"enable",nullptr), "enable fixture ChatGPT");
  const auto original=runtime.Configuration(); auto config=original;
  Check(workbench::SelectConnection(&config,"codex") && runtime.Configure(config,{},false,nullptr), "select fixture ChatGPT connector");
  Check(runtime.SetChatGPTInference("gpt-6-astra","high"), "save ChatGPT profile");
  Check(!runtime.SetChatGPTInference("gpt-6-luna","ultra"), "unsupported model effort rejected");
  const auto peer=GetCurrentProcessId()+1; const auto target=runtime.Register(peer,985,985);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("Synthetic request."); runtime.SelectAIMode(); runtime.Generate(false);
  Wait([&] { return entered.load(); }, "mock stream started");
  const auto active=runtime.Snapshot();
  Check(active["stream_blocks"].size()==2 && active["result"]=="" && active["reasoning_summary"]=="公开思考摘要", "streamed blocks and summary visible before completion");
  Check(runtime.SetChatGPTInference("gpt-6-sol","medium") && runtime.Snapshot()["busy"]==true, "profile change applies to next request only");
  runtime.ReturnToInput(); release=true; Wait([&] { return finished.load(); }, "retired producer returned");
  Check(!stale_body && !stale_summary && runtime.Snapshot()["stream_blocks"].empty() && runtime.Snapshot()["reasoning_summary"]=="" &&
      runtime.Snapshot()["source"]=="Synthetic request.", "switch revokes both output channels and retains source");
  Check(runtime.Configure(original,{},false,nullptr), "restore fixture settings"); runtime.Stop();
}
void TestDisposableMaintenanceContent() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 995, 1);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("Disposable Buffer.");
  runtime.Send(true);
  runtime.DiscardBuffer();
  auto state = runtime.Snapshot();
  Check(state["source"] == "" && state["result"] == "" &&
            !runtime.Capturing(target),
        "explicit redeployment reset discards Buffer without a content gate");
  auto event = runtime.Control({{"op", "wait"}, {"session", 995}}, peer);
  Check(event.value("kind", "") == "capture",
        "reset replaces a queued delivery with revoked capture authority");
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("Next session.");
  runtime.Stop();
  state = runtime.Snapshot();
  Check(state["source"] == "" && state["result"] == "",
        "IME shutdown discards temporary UI content instead of preserving it");
}
void TestNativeShortcutAutoSendAndCopy() {
  workbench::Runtime runtime([](const workbench::Settings&, const workbench::Generation&,
      const auto& chunk, const auto&, std::string*) { return chunk("Answer. Second."); });
  auto fixture_config = workbench::Settings{}; fixture_config.close_after_send = false; fixture_config.send_hold_ms = 1200;
  Check(runtime.Configure(fixture_config, {}, false, nullptr), "reset fixture preferences");
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 998, 999);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("One. Two.");
  auto next_event = [&] {
    auto event = runtime.Control({{"op", "wait"}, {"session", 998}}, peer);
    while (event.value("kind", "") == "capture" || event.value("kind", "") == "buffer_caret")
      event = runtime.Control({{"op", "wait"}, {"session", 998}}, peer);
    return event;
  };
  auto acknowledge = [&](const core::Json& event) {
    Check(event.value("kind", "") == "deliver", "native path enqueues TSF delivery");
    runtime.Control({{"op", "ack"}, {"session", 998}, {"request", event.at("request")}, {"accepted", true}}, peer);
  };
  Check(!runtime.ShortcutDown(peer + 1), "global shortcut rejects foreign foreground process");
  Check(runtime.ShortcutDown(peer) && !runtime.Snapshot()["pending"].get<bool>(), "global key-down defers send until release or configured 1.2-second hold");
  runtime.ShortcutUp(); auto event = next_event(); Check(event["text"] == "One. ", "tap sends first block"); acknowledge(event);
  Check(runtime.Snapshot()["source"] == "Two.", "tap retains unsent suffix");
  runtime.Paste(" Three."); Check(runtime.ShortcutDown(peer), "second gesture starts");
  Sleep(1250); runtime.Tick(); runtime.ShortcutUp();
  event = next_event(); acknowledge(event); event = next_event(); acknowledge(event);
  Check(runtime.Snapshot()["source"] == "", "configured 1.2-second hold drains exactly the frozen blocks");
  runtime.Paste("Automatic."); runtime.SetAutoSend(1, true); runtime.Tick();
  Check(runtime.Snapshot()["auto_remaining_ms"].get<long long>() > 0, "automatic send exposes countdown");
  runtime.PauseCapture(); Sleep(1020); runtime.Tick(); Check(!runtime.Snapshot()["pending"].get<bool>(), "focus pause cancels countdown delivery");
  runtime.Bind(peer); runtime.Tick(); Check(!runtime.Snapshot()["pending"].get<bool>(), "rebind restarts full countdown");
  Sleep(1020); runtime.Tick(); event = next_event(); acknowledge(event);
  Check(!runtime.Snapshot()["visible"].get<bool>(), "close-after-send waits for final acknowledgement");
  auto copy_config = fixture_config; copy_config.ai_connector = "codex-cli";
  Check(runtime.Configure(copy_config, {}, false, nullptr), "use bundled fixture connector");
  runtime.Bind(peer); runtime.Paste("Request."); runtime.Generate(false);
  Wait([&] { return runtime.Snapshot()["result"] == "Answer. Second."; }, "copy fixture generated");
  auto ticket = runtime.PrepareCopy(); Check(!ticket.is_null() && runtime.ValidateCopy(ticket), "complete result copy freezes identity and text");
  Check(!runtime.CommitCopy(ticket, [] { return false; }) && runtime.Snapshot()["visible"] == true,
        "clipboard write failure does not close or consume Buffer");
  runtime.Paste("Edit."); Check(!runtime.ValidateCopy(ticket), "editing invalidates old clipboard ticket");
  runtime.Generate(false); Wait([&] { return runtime.Snapshot()["result"] == "Answer. Second."; }, "copy fixture regenerated");
  ticket = runtime.PrepareCopy(); Check(runtime.CommitCopy(ticket, [] { return true; }), "copy write committed atomically");
  Check(!runtime.Snapshot()["capture"].get<bool>() && !runtime.Snapshot()["visible"].get<bool>() &&
        runtime.Snapshot()["result"] == "Answer. Second.", "successful result copy pauses and hides while retaining session contents");
  Check(runtime.Configure(fixture_config, {}, false, nullptr), "restore fixture connector");
}
void TestPeripheralLease() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 200;
  auto target = runtime.Register(peer, 101, 201);
  auto other = runtime.Register(peer, 102, 202);
  runtime.Focus(target);
  Check(!runtime.BorrowPeripheral(peer + 1), "Capsule rejects a background process");
  runtime.Focus(target);
  Check(runtime.BorrowPeripheral(peer) && runtime.Capturing(target), "Capsule owns only the exact live context");
  engine::EngineSnapshot text;
  text.handled = true; text.commit_text = "有"; text.composition = "you'";
  runtime.Capture(target, &text);
  auto input = runtime.TakePeripheralInput();
  Check(text.commit_text.empty() && input["events"][0]["text"] == "有" &&
      input["preedit"] == "you'" && runtime.Snapshot()["source"] == "", "Capsule search never enters host or Buffer text");
  core::KeyEvent key; key.virtual_key = VK_LEFT;
  key.virtual_key = 'V';
  key.modifiers = static_cast<unsigned>(core::KeyModifiers::kAlt) | static_cast<unsigned>(core::KeyModifiers::kShift);
  key.event_flags = 0;
  Check(!runtime.BeforeKey(target, key, false) && runtime.Capturing(target),
      "releasing the summon shortcut cannot revoke Capsule input");
  key.virtual_key = VK_LEFT; key.modifiers = 0;
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(!runtime.BeforeKey(target, key, true), "Rime owns navigation during composition");
  Check(runtime.BeforeKey(target, key, false), "Capsule owns navigation after composition");
  Check(runtime.TakePeripheralInput()["events"][0]["key"] == VK_LEFT, "navigation is queued to UI thread");
  runtime.PeripheralPhysical(true); key.virtual_key = 'R';
  Check(runtime.BeforeKey(target, key, false) &&
      runtime.TakePeripheralInput()["events"][0]["physical"] == true, "credential keys bypass Rime translation");
  runtime.Focus(other);
  Check(!runtime.Capturing(target) && !runtime.Capturing(other) &&
      !runtime.TakePeripheralInput()["active"].get<bool>(), "same-process field change revokes borrowed input");
  runtime.Focus(target);
  Check(!runtime.Capturing(target), "returning to original field cannot revive the lease");
  Check(runtime.BorrowPeripheral(peer), "explicit reopening reacquires a fresh lease");
  Check(runtime.SendPeripheral("selected note") && !runtime.SendPeripheral("duplicate"),
      "Capsule delivery freezes one native insertion at a time");
  auto event = runtime.Control({{"op", "wait"}, {"session", 101}}, peer);
  if (event.value("kind", "") == "capture") event = runtime.Control({{"op", "wait"}, {"session", 101}}, peer);
  Check(event["kind"] == "deliver" && event["text"] == "selected note" && event["context"] == 201,
      "Capsule insertion uses the existing exact-context TSF delivery path");
  runtime.Control({{"op", "ack"}, {"session", 101}, {"request", event["request"]}, {"accepted", true}}, peer);
  Check(runtime.TakePeripheralInput()["events"][0]["delivered"] == true && runtime.Snapshot()["source"] == "",
      "Capsule waits for host acknowledgement without consuming Buffer");
  runtime.Protect();
  Check(!runtime.Capturing(target) && runtime.TakePeripheralInput()["events"].empty(), "protection scrubs borrowed keys and preedit");
  runtime.Stop();
}
void TestCapturedCompositionLifecycle() {
  std::uint64_t now = 100;
  workbench::Runtime runtime([](const auto&, const auto&, const auto&, const auto&, std::string*) { return false; }, {}, [&] { return now; });
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 62, 62), other = runtime.Register(peer, 63, 63);
  runtime.Focus(target); runtime.Bind(peer); runtime.SelectBufferPlugin("");
  const auto compose = [&] {
    engine::EngineSnapshot composing; composing.composing = true; composing.composition = "ni"; composing.caret_utf16 = 2;
    runtime.Capture(target, &composing);
  };
  const auto transition = [&] {
    auto event = runtime.Control({{"op", "wait"}, {"session", 62}}, peer);
    while (event.value("kind", "") == "capture" || event.value("kind", "") == "buffer_caret")
      event = runtime.Control({{"op", "wait"}, {"session", 62}}, peer);
    Check(event.value("kind", "") == "buffer_transition", "composing UI action goes to the exact owning TSF session");
    return event["request"].get<std::uint64_t>();
  };
  unsigned settled = 0;
  const auto settle = [&](engine::EngineSnapshot* result) { ++settled; result->commit_text = "你"; return true; };
  compose(); runtime.Close(); const auto close = transition();
  Check(runtime.Snapshot().value("visible", false) && runtime.Snapshot()["preedit"] == "ni" && runtime.Capturing(target),
        "close retains owned composition until its engine acknowledges settlement");
  Check(!runtime.CompleteBufferTransition(other, close, settle) && settled == 0, "another field cannot settle the captured composition");
  Check(runtime.CompleteBufferTransition(target, close, settle) && settled == 1 &&
        !runtime.Snapshot().value("visible", true) && runtime.Snapshot()["source"] == "你", "close stages engine commit before pausing");
  Check(!runtime.CompleteBufferTransition(target, close, settle) && settled == 1, "lifecycle request consumed only once");
  runtime.Bind(peer); compose();
  Check(runtime.SelectBufferPlugin(official::kTranslation), "plugin menu queues composition settlement");
  auto request = transition();
  Check(runtime.Snapshot()["plugin"] == "", "plugin stays unchanged until composition is settled");
  Check(runtime.CompleteBufferTransition(target, request, settle) && runtime.Capturing(target) &&
        runtime.Snapshot()["plugin"] == official::kTranslation && runtime.Snapshot()["source"] == "你你",
        "plugin switch preserves input authority and committed source");
  compose(); runtime.Toggle(peer); request = transition(); runtime.Focus(other);
  Check(!runtime.CompleteBufferTransition(target, request, settle) && settled == 2, "focus change revokes queued settlement");
  runtime.Focus(target); runtime.Bind(peer); compose(); runtime.Close(); request = transition();
  now += 1600; runtime.Tick();
  Check(!runtime.CompleteBufferTransition(target, request, settle) && settled == 2 &&
        runtime.Snapshot().value("visible", false) && runtime.Snapshot()["preedit"] == "ni", "old host timeout preserves composition");
  runtime.Close(); request = transition();
  Check(!runtime.CompleteBufferTransition(target, request, [](engine::EngineSnapshot*) { return false; }) &&
        runtime.Snapshot()["preedit"] == "ni", "engine failure never discards composition or stages raw spelling");
  runtime.Close(); request = transition();
  engine::EngineSnapshot changed; changed.composition = "nin"; changed.composing = true;
  runtime.Capture(target, &changed);
  Check(!runtime.CompleteBufferTransition(target, request, settle) && settled == 2, "new typing makes old UI action stale");
  Check(runtime.ManagePlugin(official::kTranslation, "disable", nullptr), "disable selected plugin during composition");
  request = transition();
  Check(runtime.CompleteBufferTransition(target, request, settle) && settled == 3 && runtime.Snapshot()["plugin"] == "",
        "disabling the selected plugin settles composition then falls back to input");
  Check(runtime.ManagePlugin(official::kTranslation, "enable", nullptr) && runtime.Snapshot()["plugin"] == "",
        "re-enabling does not change explicit input selection");
  runtime.Stop();
}
void TestPeripheralImagePasteTicket() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 200;
  const auto first = runtime.Register(peer, 701, 801), other = runtime.Register(peer, 702, 802);
  runtime.Focus(first);
  Check(!runtime.PeripheralPasteTarget(), "detached Capsule cannot invent a paste target");
  Check(runtime.BorrowPeripheral(peer), "acquire image activation target");
  const auto ticket = runtime.PeripheralPasteTarget();
  Check(ticket && !runtime.CanPastePeripheral(ticket), "paste must wait until Capsule releases borrowed input");
  runtime.ReleasePeripheral();
  Check(runtime.CanPastePeripheral(ticket), "closing Capsule preserves the exact live input ticket");
  runtime.Focus(other); runtime.Focus(first);
  Check(!runtime.CanPastePeripheral(ticket), "leaving and returning to a field cannot revive a paste ticket");
  runtime.BorrowPeripheral(peer);
  const auto foreground = runtime.PeripheralPasteTarget(); runtime.ReleasePeripheral();
  runtime.ObserveForeground(peer + 1); runtime.ObserveForeground(peer);
  Check(!runtime.CanPastePeripheral(foreground), "application switch invalidates ticket even without TSF focus events");
  runtime.BorrowPeripheral(peer);
  engine::EngineSnapshot composing; composing.composition = "ni"; composing.composing = true;
  runtime.Capture(first, &composing);
  Check(!runtime.PeripheralPasteTarget(), "unsettled Capsule composition prevents image activation");
  runtime.ReleasePeripheral(); runtime.BorrowPeripheral(peer);
  const auto protected_ticket = runtime.PeripheralPasteTarget(); runtime.ReleasePeripheral();
  runtime.Protect(); runtime.Focus(first);
  Check(!runtime.CanPastePeripheral(protected_ticket), "session protection invalidates queued image paste");
  runtime.BorrowPeripheral(peer);
  const auto removed = runtime.PeripheralPasteTarget(); runtime.ReleasePeripheral(); runtime.Remove(first);
  runtime.Register(peer, 701, 801); runtime.Focus(first);
  Check(!runtime.CanPastePeripheral(removed), "recreated session cannot revive an old ticket");
  runtime.Stop();
}
void TestBufferFocusFoldAndResume() {
  std::uint64_t now = 100;
  const auto peer = GetCurrentProcessId() + 1;
  std::uint32_t foreground = peer;
  workbench::Runtime runtime([](const auto&, const auto&, const auto& chunk, const auto&, std::string*) {
    return chunk("Retained answer.");
  }, {}, [&] { return now; }, [&] { return foreground; });
  const auto original = runtime.Configuration();
  auto config = original; config.ai_connector = "codex-cli"; config.auto_send_seconds = 0;
  Check(runtime.Configure(config, {}, false, nullptr), "configure focus-follow fixture");
  const auto first = runtime.Register(peer, 71, 81), second = runtime.Register(peer, 72, 82);
  const auto third = runtime.Register(peer + 1, 73, 83);
  runtime.Focus(first); runtime.Bind(peer); runtime.Paste("Retained request.");
  Check(runtime.SelectBufferPlugin(official::kCodex), "select focus fixture plugin"); runtime.Generate(false);
  Wait([&] { return runtime.Snapshot()["result"] == "Retained answer."; }, "prepare completed plugin output");
  const auto saved = runtime.Snapshot();
  const auto focus = [&](workbench::Target target) {
    runtime.Focus(target);
    const auto event = runtime.Control({{"op", "wait"}, {"session", target.session}}, target.process);
    Check(event.value("kind", "") == "capture", "focus challenge is scoped to capture notification");
    return core::Json{{"op", "focus_ready"}, {"session", target.session}, {"context", target.context},
        {"request", event.value("focusRequest", 0ULL)}, {"caret", {620, 320, 2, 24}}};
  };
  runtime.PauseCapture();
  now += 599; runtime.Tick();
  Check(!runtime.Snapshot().value("folded", true), "brief blur keeps rails expanded during macOS grace period");
  ++now; runtime.Tick();
  Check(runtime.Snapshot().value("folded", false) && runtime.Snapshot().value("visible", false) &&
        !runtime.Capturing(first), "sustained blur folds to toolbar without hiding or owning input");
  auto proof = focus(second);
  const auto retiring = runtime.Register(peer + 3, 74, 84);
  runtime.Remove(retiring);
  // Retiring another host must preserve this outstanding proof, not replace
  // its nonce after the focused TSF has already started reading its caret.
  Check(proof["request"] != 0 && !runtime.Capturing(second), "focus alone never revives capture");
  auto stale = proof; stale["context"] = first.context;
  Check(runtime.Control(stale, peer)["kind"] == "stale", "old same-process field is rejected");
  Check(runtime.Control(proof, peer + 1)["kind"] == "disconnected", "foreign peer cannot answer focus challenge");
  foreground = peer + 2;
  Check(runtime.Control(proof, peer)["kind"] == "stale", "background focus proof cannot expand Buffer");
  foreground = peer;
  stale = proof; stale["caret"] = {0, 0, 0, 0};
  Check(runtime.Control(stale, peer)["kind"] == "stale", "no caret is not a visible editable target");
  Check(runtime.Control(proof, peer)["kind"] == "ok", "new exact editable field resumes capture");
  const auto resumed = runtime.Snapshot();
  Check(runtime.Capturing(second) && !resumed.value("folded", true) &&
        resumed["opening_revision"].get<unsigned long long>() > saved["opening_revision"].get<unsigned long long>() &&
        resumed["source_blocks"] == saved["source_blocks"] && resumed["result_blocks"] == saved["result_blocks"] &&
        resumed["plugin"] == saved["plugin"], "resume preserves blocks, IDs and plugin while issuing a new opening");
  Check(runtime.PlacementTarget() == second && runtime.PlacementCaret(second)->x == 620 &&
        !runtime.PlacementCaret(first), "resume uses only the new field's current caret");
  Check(runtime.Control(proof, peer)["kind"] == "stale", "focus proof is single-use");
  const auto opening = resumed["opening_revision"];
  runtime.Focus(second);
  Check(runtime.Snapshot()["opening_revision"] == opening && runtime.Capturing(second), "duplicate focus does not restart an active capture");
  runtime.PauseCapture(); now += 200;
  proof = focus(first); runtime.Control(proof, peer); now += 1000; runtime.Tick();
  Check(!runtime.Snapshot().value("folded", true), "restoring during grace cancels the delayed fold");
  proof = focus(second); runtime.PauseCapture();
  Check(runtime.Control(proof, peer)["kind"] == "stale", "opening settings revokes a pending proof");
  proof = focus(second); now += 1501;
  Check(runtime.Control(proof, peer)["kind"] == "stale", "expired focus cannot reposition Buffer");
  proof = focus(first); auto third_proof = focus(third); foreground = third.process;
  Check(runtime.Control(proof, peer)["kind"] == "stale", "later focus supersedes an earlier challenge");
  Check(runtime.Control(third_proof, third.process)["kind"] == "ok" && runtime.Capturing(third), "new application can resume retained workspace");
  runtime.Send(true);
  auto delivery = runtime.Control({{"op", "wait"}, {"session", third.session}}, third.process);
  if (delivery.value("kind", "") == "capture") delivery = runtime.Control({{"op", "wait"}, {"session", third.session}}, third.process);
  Check(delivery.value("kind", "") == "deliver", "fixture owns a pending delivery before focus leaves");
  proof = focus(second); foreground = peer; runtime.Control(proof, peer);
  Check(runtime.Control({{"op", "delivery_guard"}, {"session", third.session}, {"request", delivery["request"]}}, third.process)["kind"] == "stale",
        "automatic resume cannot authorize an old queued insertion");
  runtime.Control({{"op", "ack"}, {"session", third.session}, {"request", delivery["request"]}, {"accepted", false}}, third.process);
  Check(runtime.Capturing(second) && runtime.Snapshot()["result"] == saved["result"], "rejected old delivery retains result in new field");
  proof = focus(first); runtime.Close();
  Check(runtime.Control(proof, peer)["kind"] == "stale", "explicit close disarms automatic resume");
  proof = focus(second);
  Check(proof["request"] == 0 && !runtime.Snapshot().value("visible", true), "closed Buffer stays closed on next focus");
  runtime.Bind(peer); proof = focus(first); runtime.Protect();
  Check(runtime.Control(proof, peer)["kind"] == "stale", "protection revokes pending focus");
  runtime.DiscardBuffer(); Check(runtime.StageExternal("Reviewed note."), "stage peripheral review");
  proof = focus(second);
  Check(proof["request"] == 0 && !runtime.Capturing(second), "external review cannot silently opt into following input");
  Check(runtime.Configure(original, {}, false, nullptr), "restore focus fixture configuration");
}
void TestBufferLifecycleParity() {
  std::uint64_t now = 100;
  std::atomic<unsigned> calls{0};
  workbench::Runtime runtime([&](const auto&, const workbench::Generation& job, const auto& chunk, const auto&, std::string*) {
    ++calls; return chunk(job.translation ? "First. Remaining." : "AI answer.");
  }, {}, [&] { return now; });
  const auto original = runtime.Configuration();
  auto config = original; config.ai_connector = "codex-cli"; config.auto_send_seconds = 1; config.close_after_send = true;
  Check(runtime.Configure(config, {}, false, nullptr), "configure Buffer lifecycle fixture");
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 61, 61);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("原文。"); runtime.Generate(true);
  Wait([&] { return runtime.Snapshot()["result"] == "First. Remaining."; }, "translation result ready for plugin switching");
  now += 5000; runtime.Tick(); now += 5000; runtime.Tick();
  Check(!runtime.Snapshot().value("pending", true) && runtime.Snapshot()["auto_remaining_ms"] == 0,
        "Default-buffer auto-send never sends plugin output");
  runtime.Cancel();
  Check(runtime.Snapshot()["plugin"] == official::kTranslation && !runtime.Snapshot().value("busy", true),
        "stop processing retains the selected plugin instead of changing mode");
  now += 1000; runtime.Tick(); Check(calls == 1, "stopped translation does not silently restart");
  Check(runtime.SelectBufferPlugin(official::kCodex), "switch to AI plugin");
  Check(runtime.Snapshot()["result"] == "" && runtime.Snapshot()["preview"] == "" && runtime.Capturing(target) &&
        runtime.Snapshot()["status"] == "Buffer", "new plugin has its own idle result surface without losing input focus");
  runtime.Generate(false);
  Wait([&] { return runtime.Snapshot()["result"] == "AI answer."; }, "AI result ready in its own workspace");
  const auto ai_ticket = runtime.PrepareCopy();
  Check(runtime.SelectBufferPlugin(official::kTranslation) && runtime.Snapshot()["result"] == "First. Remaining." &&
        !runtime.ValidateCopy(ai_ticket), "switching restores the owner's result and invalidates another plugin's copy ticket");
  Check(runtime.SelectBufferPlugin(official::kCodex) && runtime.Snapshot()["result"] == "AI answer.",
        "switching back restores AI result without rerunning its provider");
  runtime.SelectBufferPlugin(official::kTranslation);
  const auto next_delivery = [&] {
    auto event = runtime.Control({{"op", "wait"}, {"session", 61}}, peer);
    while (event.value("kind", "") == "capture" || event.value("kind", "") == "buffer_caret")
      event = runtime.Control({{"op", "wait"}, {"session", 61}}, peer);
    Check(event.value("kind", "") == "deliver", "fixture receives a native delivery");
    return event;
  };
  runtime.Send(false); auto event = next_delivery();
  Check(event.value("guard", false), "Broker opts into exact delivery revalidation");
  runtime.Control({{"op", "ack"}, {"session", 61}, {"request", event["request"]}, {"accepted", true}}, peer);
  Check(runtime.Snapshot()["source"] == "" && runtime.Snapshot()["result"] == "Remaining.", "first translation child retires its source");
  runtime.ReturnToInput(); runtime.Paste("新的原文。");
  Check(runtime.Snapshot()["result"] == "", "ordinary input hides parked plugin output");
  runtime.SelectBufferPlugin(official::kTranslation);
  Check(runtime.Snapshot()["result"] == "Remaining.", "retired translation output survives plugin switch and unrelated source edits");
  runtime.SelectBufferPlugin(official::kCodex);
  Check(runtime.Snapshot()["result"] == "", "changed source invalidates parked AI output before reuse");
  runtime.DiscardBuffer(); runtime.Focus(target); runtime.Bind(peer); runtime.SelectBufferPlugin("");
  runtime.Paste("One. Two."); runtime.Send(true); event = next_delivery();
  const auto guarded = [&] { return runtime.Control({{"op", "delivery_guard"}, {"session", 61}, {"request", event["request"]}}, peer)["kind"] == "ok"; };
  Check(guarded(), "current opening permits its exact pending delivery");
  runtime.Close(); Check(!guarded(), "close retires delivery authority");
  runtime.Bind(peer); Check(!guarded(), "reopening the same field cannot revive an old queued delivery");
  runtime.Control({{"op", "ack"}, {"session", 61}, {"request", event["request"]}, {"accepted", true}}, peer);
  Check(runtime.Snapshot()["source"] == "Two." && !runtime.Snapshot().value("pending", true) &&
        runtime.Snapshot().value("visible", false), "late ack consumes only confirmed content and cannot continue the old send-all chain");
  runtime.Send(false); event = next_delivery(); runtime.Close(); runtime.Bind(peer);
  runtime.Control({{"op", "ack"}, {"session", 61}, {"request", event["request"]}, {"accepted", true}}, peer);
  Check(runtime.Snapshot()["source"] == "" && runtime.Snapshot().value("visible", false),
        "late terminal ack cannot close the newly opened Buffer");
  Check(runtime.Configure(original, {}, false, nullptr), "restore lifecycle fixture preferences");
  runtime.Stop();
}
void TestRememberedBufferPlugin() {
  wchar_t previous[32768]{};
  GetEnvironmentVariableW(L"LOCALAPPDATA", previous, 32768);
  const auto root = std::filesystem::path(previous) / L"selection-fixture";
  std::filesystem::create_directories(root);
  SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
  workbench::Settings settings; settings.close_after_send = false;
  Check(workbench::SaveSettings(settings, nullptr), "isolated plugin preference fixture");
  const auto peer = GetCurrentProcessId() + 1;
  std::atomic<unsigned> generated{0};
  const auto generate = [&](const auto&, const auto&, const auto&, const auto&, std::string*) {
    ++generated; return false;
  };
  std::string cycled;
  {
    workbench::Runtime runtime(generate);
    Check(!runtime.Snapshot().value("visible", true), "preference never opens Buffer at startup");
    const auto target = runtime.Register(peer, 51, 51);
    runtime.Focus(target); runtime.Bind(peer);
    Check(runtime.Snapshot()["plugin"] == "", "first launch uses input until the user selects a plugin");
    runtime.Paste("Retained only in this process.");
    Check(runtime.SelectBufferPlugin(official::kTranslation), "select translation for next opening");
    const auto before = runtime.Snapshot();
    runtime.Close(); runtime.Cancel(); // Copy-and-close also cancels the transient mode.
    runtime.Toggle(peer);
    Check(runtime.Snapshot()["plugin"] == official::kTranslation && runtime.Capturing(target) &&
          runtime.Snapshot()["source_blocks"] == before["source_blocks"],
          "reopening restores the last plugin after transient cancellation without discarding the draft");
    runtime.Close(); runtime.Toggle(0);
    Check(runtime.Snapshot()["plugin"] == official::kTranslation && !runtime.Snapshot().value("capture", true),
          "restoring a plugin never invents input authority");
    runtime.Stop();
  }
  {
    workbench::Runtime restarted(generate);
    Check(!restarted.Snapshot().value("visible", true) && restarted.Snapshot()["source"] == "" && generated == 0,
          "restart persists only plugin identity, not text, window visibility or provider work");
    const auto target = restarted.Register(peer, 52, 52);
    restarted.Focus(target); restarted.Bind(peer);
    Check(restarted.Snapshot()["plugin"] == official::kTranslation && restarted.Capturing(target),
          "first explicit opening after restart restores translation");
    Check(restarted.PluginShortcut(peer, VK_DOWN, MOD_ALT | MOD_SHIFT), "native shortcut changes the remembered plugin");
    cycled = restarted.Snapshot()["plugin"];
    Check(workbench::OfficialPluginStore{}.LoadBufferSelection() == cycled,
          "keyboard and menu selection use the same persisted identity");
    restarted.Stop();
  }
  {
    workbench::Runtime restarted(generate);
    restarted.Bind(0);
    Check(restarted.Snapshot()["plugin"] == cycled, "shortcut selection survives a restart");
    restarted.ReturnToInput(); restarted.Close(); restarted.Bind(0);
    Check(restarted.Snapshot()["plugin"] == "", "explicit return to input replaces the remembered plugin");
    restarted.Stop();
  }
  {
    workbench::Runtime restarted(generate);
    restarted.Bind(0);
    Check(restarted.Snapshot()["plugin"] == "", "explicit input mode survives restart");
    Check(restarted.SelectBufferPlugin(official::kTranslation), "select plugin before disabling");
    Check(restarted.ManagePlugin(official::kTranslation, "disable", nullptr), "disable the remembered plugin");
    restarted.Close(); restarted.Bind(0);
    Check(restarted.Snapshot()["plugin"] == "", "disabled plugin falls back to input");
    Check(restarted.ManagePlugin(official::kTranslation, "enable", nullptr), "re-enable plugin for selection");
    restarted.Close(); restarted.Bind(0);
    Check(restarted.Snapshot()["plugin"] == "", "enabling a plugin does not override explicit input ownership");
    Check(restarted.SelectBufferPlugin(official::kCodex), "select plugin before uninstalling");
    Check(restarted.ManagePlugin(official::kCodex, "uninstall", nullptr), "uninstall remembered plugin");
    restarted.Close(); restarted.Bind(0);
    Check(restarted.Snapshot()["plugin"] == "", "uninstalled plugin falls back to input");
    Check(restarted.SelectBufferPlugin(official::kTranslation), "save selection before offline disable");
    restarted.Stop();
  }
  Check(workbench::OfficialPluginStore{}.Enable(official::kTranslation, false, nullptr), "disable plugin while runtime is stopped");
  {
    workbench::Runtime restarted(generate);
    restarted.Bind(0);
    Check(restarted.Snapshot()["plugin"] == "" && workbench::OfficialPluginStore{}.LoadBufferSelection().empty(),
          "startup revalidates saved selection and clears a disabled owner");
    restarted.Stop();
  }
  workbench::Settings unchanged;
  Check(workbench::LoadSettings(&unchanged, nullptr) && unchanged.revision == settings.revision && generated == 0,
        "selection persistence neither changes settings revisions nor launches generation");
  SetEnvironmentVariableW(L"LOCALAPPDATA", previous);
  std::filesystem::remove_all(root);
}
void TestNativePluginShortcutLease() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 41, 41);
  const auto press = [&](unsigned key, unsigned modifiers = MOD_ALT | MOD_SHIFT) {
    return runtime.PluginShortcut(peer, key, modifiers);
  };
  Check(!press(VK_DOWN), "native plugin shortcut is inactive before Buffer binding");
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("Keep native shortcut draft.");
  const auto before = runtime.Snapshot();
  const auto plugins = runtime.BufferPlugins();
  Check(plugins.size() > 1, "native cycling fixture has enabled plugins");
  Check(!runtime.PluginShortcut(peer + 1, VK_DOWN, 5), "foreign foreground cannot cycle the bound Buffer");
  for (std::size_t i = 1; i <= plugins.size(); ++i) {
    Check(press(VK_DOWN), "native Alt Shift Down switches once per dispatched hotkey");
    const auto now = runtime.Snapshot();
    Check(now["plugin"] == plugins[i % plugins.size()].id && runtime.Capturing(target) &&
          now["target_pid"] == before["target_pid"] && now["opening_revision"] == before["opening_revision"] &&
          now["source_blocks"] == before["source_blocks"] && !now.value("pending", true),
          "native switching preserves exact focus lease, placement and unsent content");
  }
  Check(press(VK_UP) && runtime.Snapshot()["plugin"] == plugins.back().id,
        "native Alt Shift Up cycles backwards and wraps");
  const auto selected = runtime.Snapshot()["plugin"];
  Check(!press(VK_DOWN, MOD_ALT | MOD_CONTROL | MOD_SHIFT) && runtime.Snapshot()["plugin"] == selected,
        "extra modifiers are not a plugin shortcut");
  engine::EngineSnapshot typing;
  typing.commit_text = " Still typing.";
  runtime.Capture(target, &typing);
  Check(typing.commit_text.empty() && runtime.Snapshot()["source"] == "Keep native shortcut draft. Still typing.",
        "typing after native plugin switching remains captured instead of leaking to host");
  typing.composition = "pending"; typing.composing = true;
  runtime.Capture(target, &typing);
  Check(!press(VK_DOWN) && runtime.Capturing(target) && runtime.Snapshot()["preedit"] == "pending",
        "native shortcut leaves unsettled composition and its lease to the TSF path");
  typing.composition.clear(); typing.composing = false;
  runtime.Capture(target, &typing);
  const auto original = runtime.Configuration();
  auto custom = original;
  custom.previous_plugin_key = 'K'; custom.previous_plugin_modifiers = MOD_CONTROL | MOD_SHIFT;
  custom.next_plugin_key = 'J'; custom.next_plugin_modifiers = MOD_CONTROL | MOD_ALT;
  Check(runtime.Configure(custom, {}, false, nullptr), "save native cycle shortcut settings");
  runtime.Focus(target); runtime.Bind(peer);
  Check(!press(VK_DOWN) && press('J', MOD_CONTROL | MOD_ALT),
        "native dispatcher rejects a queued old chord and honors committed custom settings");
  Check(runtime.Configure(original, {}, false, nullptr), "restore native cycle settings");
  runtime.Focus(target); runtime.Bind(peer);
  const auto other = runtime.Register(peer, 42, 42);
  runtime.Focus(other);
  Check(!press(VK_DOWN) && !runtime.Capturing(other), "same-process input-field change never auto-rebinds on a plugin shortcut");
  runtime.Focus(target); runtime.Bind(peer); runtime.PauseCapture();
  Check(!press(VK_DOWN), "paused capture cannot be resumed by a plugin shortcut");
  runtime.Focus(target); runtime.Bind(peer);
  Check(runtime.BorrowPeripheral(peer) && !press(VK_DOWN), "Capsule borrowed input never switches the underlying Buffer");
  runtime.ReleasePeripheral(); runtime.Focus(target); runtime.Bind(peer);
  runtime.SelectBufferPlugin(""); runtime.Send(false);
  Check(runtime.Snapshot().value("pending", false) && !press(VK_DOWN) && runtime.Capturing(target),
        "pending delivery cannot switch mode or lose its target");
  runtime.Close();
  Check(!press(VK_DOWN), "closed Buffer releases native plugin authority");
  runtime.Stop();
}
void TestPluginSwitchFocusNotifications() {
  const auto peer = GetCurrentProcessId() + 1;
  unsigned foreground = peer;
  workbench::Runtime runtime({}, {}, [] { return GetTickCount64(); }, [&] { return foreground; });
  const auto target = runtime.Register(peer, 451, 451);
  const auto background = runtime.Register(peer + 1, 452, 452);
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("Keep focus across the boundary.");
  const auto source = runtime.Snapshot()["source_blocks"];
  const auto plugins = runtime.BufferPlugins();
  Check(plugins.size() > 1, "focus fixture has a first and last plugin");
  for (int lap = 0; lap < 3; ++lap) {
    Check(runtime.SelectBufferPlugin(plugins.back().id) &&
          runtime.PluginShortcut(peer, VK_DOWN, MOD_ALT | MOD_SHIFT) &&
          runtime.Snapshot()["plugin"] == "", "last plugin wraps to Default");
    runtime.Control({{"op", "focus"}, {"session", background.session}}, background.process);
    Check(runtime.Capturing(target), "late background focus cannot revoke a plugin switch's live host");
    Check(runtime.PluginShortcut(peer, VK_UP, MOD_ALT | MOD_SHIFT) &&
          runtime.Snapshot()["plugin"] == plugins.back().id, "Default wraps to last plugin");
    Check(runtime.SelectBufferPlugin("") && runtime.Capturing(target),
          "menu selection shares the same focus-preserving transition");
    // Win32 may temporarily have no foreground window while a popup closes.
    // The same TSF document need not receive another focus callback afterward.
    foreground = 0; runtime.ObserveForeground(foreground);
    Check(!runtime.Capturing(target), "transient foreground loss immediately revokes delivery");
    foreground = peer; runtime.ObserveForeground(foreground);
    const auto challenge = runtime.Control({{"op", "wait"}, {"session", target.session}}, peer);
    Check(challenge.value("focusRequest", 0ULL) && !runtime.Capturing(target),
          "same host return requests fresh proof without blindly restoring capture");
    const auto retiring = runtime.Register(peer + 2, 460 + lap, 470 + lap);
    runtime.Remove(retiring);
    runtime.Control({{"op", "focus"}, {"session", background.session}}, background.process);
    Check(runtime.Control({{"op", "focus_ready"}, {"session", target.session},
        {"context", target.context}, {"request", challenge["focusRequest"]},
        {"caret", {620, 320, 2, 24}}}, peer)["kind"] == "ok" && runtime.Capturing(target),
        "late background focus and retirement cannot invalidate the returning host's proof");
    Check(runtime.Snapshot()["source_blocks"] == source && !runtime.Snapshot().value("folded", true),
          "first/last switch and transient focus recovery retain draft identities and expanded state");
  }
  core::KeyEvent key{}; key.virtual_key = VK_UP;
  key.modifiers = core::ShortcutModifiers(MOD_ALT | MOD_SHIFT);
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(runtime.BeforeKey(target, key, false), "cycle key down owned before unrelated host retires");
  const auto selected = runtime.Snapshot()["plugin"];
  runtime.Remove(background);
  Check(runtime.BeforeKey(target, key, false) && runtime.Snapshot()["plugin"] == selected,
        "unrelated session retirement preserves held cycle ownership instead of cycling twice");
  key.event_flags = 0; key.modifiers = 0;
  Check(runtime.BeforeKey(target, key, false), "cycle key up stays owned after unrelated retirement");
  const auto replacement = runtime.Register(background.process, 480, 480);
  foreground = background.process;
  runtime.Control({{"op", "focus"}, {"session", replacement.session}}, replacement.process);
  Check(!runtime.Capturing(target), "a real foreground host change still revokes capture");
  const auto next_challenge = runtime.Control({{"op", "wait"}, {"session", replacement.session}}, replacement.process);
  runtime.Remove(target);
  Check(runtime.Control({{"op", "focus_ready"}, {"session", replacement.session},
      {"context", replacement.context}, {"request", next_challenge["focusRequest"]},
      {"caret", {700, 400, 2, 24}}}, replacement.process)["kind"] == "ok",
      "the previously bound host retiring cannot replace the new host's pending proof");
  foreground = 0; runtime.ObserveForeground(foreground); runtime.PauseCapture();
  foreground = replacement.process; runtime.ObserveForeground(foreground);
  Check(!runtime.Capturing(replacement), "explicit settings pause does not auto-resume on a foreground timer");
  runtime.Stop();
}
void TestBufferPluginCycle() {
  workbench::Runtime runtime;
  const auto peer = GetCurrentProcessId() + 1;
  const auto target = runtime.Register(peer, 1, 1);
  core::KeyEvent key{}; key.virtual_key = VK_DOWN;
  key.modifiers = core::ShortcutModifiers(MOD_ALT | MOD_SHIFT);
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(!runtime.BeforeKey(target, key, false), "closed Buffer does not own plugin cycling");
  runtime.Focus(target); runtime.Bind(peer); runtime.Paste("Keep source.");
  const auto plugins = runtime.BufferPlugins();
  Check(!plugins.empty() && plugins.front().id.empty(), "input is first toolbar mode");
  for (std::size_t i = 1; i <= plugins.size(); ++i) {
    Check(runtime.BeforeKey(target, key, false), "cycle press owned");
    const auto selected = plugins[i % plugins.size()].id;
    Check(runtime.Snapshot()["plugin"] == selected, "cycle follows enabled toolbar order and wraps");
    Check(runtime.BeforeKey(target, key, false) && runtime.Snapshot()["plugin"] == selected,
          "held key does not race through plugins");
    key.event_flags = 0; key.modifiers = 0;
    Check(runtime.BeforeKey(target, key, false), "release consumed after modifiers released");
    key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
    key.modifiers = core::ShortcutModifiers(MOD_ALT | MOD_SHIFT);
  }
  key.virtual_key = VK_UP;
  Check(runtime.BeforeKey(target, key, false) && runtime.Snapshot()["plugin"] == plugins.back().id,
        "reverse wraps to last enabled plugin");
  key.event_flags = 0; runtime.BeforeKey(target, key, false);
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  key.modifiers |= static_cast<unsigned>(core::KeyModifiers::kControl);
  Check(!runtime.BeforeKey(target, key, false), "extra modifier stays a host shortcut");
  Check(runtime.Snapshot()["source"] == "Keep source." && !runtime.Snapshot().value("pending", false),
        "cycle neither delivers nor discards draft");
  const auto original = runtime.Configuration();
  auto config = original;
  config.previous_plugin_key = 'K'; config.previous_plugin_modifiers = MOD_CONTROL | MOD_SHIFT;
  config.next_plugin_key = 'J'; config.next_plugin_modifiers = MOD_CONTROL | MOD_ALT;
  Check(runtime.Configure(config, {}, false, nullptr), "save independent plugin shortcuts");
  runtime.Focus(target); runtime.Bind(peer);
  key.virtual_key = VK_DOWN; key.modifiers = core::ShortcutModifiers(5);
  Check(!runtime.BeforeKey(target, key, false), "removed default no longer switches");
  key.virtual_key = 'J'; key.modifiers = core::ShortcutModifiers(3);
  Check(runtime.PluginCycleDirection(key) == 1, "prediction follows custom next");
  Check(runtime.BeforeKey(target, key, false) && runtime.Snapshot()["plugin"] == "", "custom next wraps forward");
  key.event_flags |= static_cast<unsigned>(core::KeyEventFlags::kRepeat);
  Check(runtime.BeforeKey(target, key, false) && runtime.Snapshot()["plugin"] == "", "held custom key switches once");
  key.event_flags = 0; key.modifiers = 0;
  Check(runtime.BeforeKey(target, key, false), "custom release consumed without modifiers");
  key.virtual_key = 'K'; key.modifiers = core::ShortcutModifiers(6);
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(runtime.PluginCycleDirection(key) == -1, "prediction follows custom previous");
  Check(runtime.BeforeKey(target, key, true) && runtime.Snapshot()["plugin"] == "", "unsettled composition cannot switch");
  key.event_flags = 0; runtime.BeforeKey(target, key, false);
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  Check(runtime.BeforeKey(target, key, false) && runtime.Snapshot()["plugin"] == plugins.back().id, "custom previous reverses");
  key.event_flags = 0; runtime.BeforeKey(target, key, false);
  Check(runtime.Configure(original, {}, false, nullptr), "restore cycle settings");
  key.virtual_key = VK_UP; key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  runtime.PauseCapture(); key.modifiers = core::ShortcutModifiers(MOD_ALT | MOD_SHIFT);
  Check(!runtime.BeforeKey(target, key, false), "settings focus cannot cycle host Buffer");
  runtime.Toggle(peer);
  Check(!runtime.Snapshot().value("visible", true), "toggle closes visible Buffer even when capture is paused");
  runtime.Toggle(peer);
  Check(runtime.Snapshot().value("visible", false), "next toggle reopens Buffer");
  runtime.Stop();
}
void TestFreshProfileRequiresChatGPTOptIn() {
  workbench::Runtime runtime([](const auto&, const auto&, const auto&, const auto&, std::string*) { return false; });
  Check(!runtime.Configuration().chord_enabled, "new profile does not activate the chord interceptor");
  runtime.Bind(0);
  Check(runtime.Snapshot()["plugin"] == "" && !runtime.SelectBufferPlugin(official::kCodex),
        "fresh Buffer starts in Default and cannot activate uninstalled ChatGPT");
  const auto plugins = runtime.BufferPlugins();
  Check(std::none_of(plugins.begin(), plugins.end(), [](const auto& item) { return item.id == official::kCodex; }),
        "new toolbar does not expose ChatGPT before explicit installation");
}
void TestReadinessConfigurationCancellationAndWaitingClock() {
  std::atomic<std::uint64_t> now{1000};
  std::atomic<unsigned> probes{0}, generations{0}, notifications{0};
  std::atomic<bool> cancelled_first{false}, cancelled_stop{false}, generation_cancelled{false};
  workbench::Runtime runtime([&](const auto&, const auto&, const auto&,
      const std::function<bool()>& cancelled, std::string*) {
    ++generations;
    while (!cancelled()) Sleep(2);
    generation_cancelled = true;
    return false;
  }, {}, [&] { return now.load(); }, [] { return 0U; },
  [&](const workbench::Settings&, const std::function<bool()>& cancelled) -> workbench::Runtime::Readiness {
    const auto index = ++probes;
    if (index == 1 || index == 4) {
      while (!cancelled()) Sleep(2);
      if (index == 1) cancelled_first = true; else cancelled_stop = true;
      return {true, "retired readiness must not publish"};
    }
    return index == 2 ? workbench::Runtime::Readiness{false, "fixture login required"}
                      : workbench::Runtime::Readiness{true, {}};
  });
  const auto original = runtime.Configuration(); auto config = original;
  Check(workbench::SelectConnection(&config, "codex") && runtime.Configure(config, {}, false, nullptr), "select readiness fixture connector");
  runtime.SetNotify([&] { ++notifications; });
  runtime.Bind(0); runtime.Paste("Readiness request.");
  Check(runtime.SelectBufferPlugin(official::kCodex), "readiness fixture selects ChatGPT");
  Check(!runtime.Snapshot().value("generation_ready", true), "first readiness snapshot returns while async check is pending");
  Wait([&] { return probes == 1; }, "first async readiness probe starts");
  runtime.Generate(false);
  Check(generations == 0 && !runtime.Snapshot().value("busy", true), "pending readiness cannot launch a provider");
  config = runtime.Configuration(); ++config.font_size;
  Check(runtime.Configure(config, {}, false, nullptr), "configuration edit retires old readiness");
  Wait([&] { return cancelled_first.load(); }, "old readiness receives cancellation promptly");
  Wait([&] { return runtime.Snapshot()["generation_unavailable"] == "fixture login required"; }, "new configuration checks its own readiness");
  Check(probes == 2 && !runtime.Snapshot().value("generation_ready", true), "late old readiness cannot enable generation");
  runtime.Generate(false);
  Check(generations == 0, "unready connector retains source without starting generation");
  config = runtime.Configuration(); ++config.font_size;
  Check(runtime.Configure(config, {}, false, nullptr), "retry after changing connector preferences");
  Wait([&] { return runtime.Snapshot().value("generation_ready", false); }, "updated connector becomes ready");
  Check(probes == 3, "successful readiness is cached");
  runtime.Generate(false);
  Wait([&] { return generations == 1; }, "ready connector starts injected generator");
  Check(runtime.Snapshot()["waiting_seconds"] == 0, "generation starts its own elapsed clock");
  now = 4500; runtime.Tick();
  Check(runtime.Snapshot()["waiting_seconds"] == 3, "waiting clock updates without provider tokens");
  const auto notified = notifications.load(); runtime.Tick();
  Check(notifications == notified, "waiting clock does not repaint repeatedly within one second");
  runtime.Cancel();
  Wait([&] { return generation_cancelled.load(); }, "cancel retires active provider");
  Check(runtime.Snapshot()["waiting_seconds"] == 0 && !runtime.Snapshot().value("busy", true), "cancel removes elapsed waiting state");
  Check(runtime.Snapshot()["source"] == "Readiness request." && probes == 3, "cancel preserves source and cached readiness");
  now = 35001; runtime.Snapshot();
  Wait([&] { return probes == 4; }, "expired readiness refreshes asynchronously");
  runtime.Stop();
  Wait([&] { return cancelled_stop.load(); }, "shutdown cancels readiness without waiting for CLI timeout");
  Check(workbench::SaveSettings(original, nullptr), "restore readiness fixture preferences");
}
void TestResetOnApplicationSwitchPreservesExternalDrafts() {
  workbench::Runtime runtime([](const auto&, const auto&, const auto&, const auto&, std::string*) { return false; });
  const auto original = runtime.Configuration(); auto config = original;
  config.reset_on_app_switch = true;
  Check(runtime.Configure(config, {}, false, nullptr), "enable application switch reset");
  const auto peer = GetCurrentProcessId() + 1;
  const auto first = runtime.Register(peer, 2001, 1), same_app = runtime.Register(peer, 2002, 2);
  const auto other = runtime.Register(peer + 1, 2003, 3);
  runtime.Focus(first); runtime.Bind(peer);
  engine::EngineSnapshot input; input.handled = true; input.commit_text = "Local draft.";
  runtime.Capture(first, &input);
  Check(runtime.Snapshot()["source"] == "Local draft.", "native capture creates local draft");
  runtime.ObserveForeground(GetCurrentProcessId()); runtime.Focus({}); runtime.ObserveForeground(0);
  Check(runtime.Snapshot()["source"] == "Local draft.", "own settings and transient null foreground do not clear draft");
  runtime.Focus(same_app);
  Check(runtime.Snapshot()["source"] == "Local draft.", "switching fields within one application preserves draft");
  runtime.Focus(other);
  Check(runtime.Snapshot()["source"] == "" && runtime.Snapshot()["result"] == "", "another external application clears local draft");
  runtime.Bind(other.process); runtime.Paste("Clipboard draft."); runtime.Focus(first);
  Check(runtime.Snapshot()["source"] == "Clipboard draft.", "clipboard import survives changing external applications");
  runtime.DiscardBuffer();
  Check(runtime.StageExternal("Capsule draft."), "Capsule stages external draft");
  runtime.ObserveForeground(other.process);
  Check(runtime.Snapshot()["source"] == "Capsule draft.", "Capsule import keeps content until explicit disposal");
  runtime.DiscardBuffer(); runtime.Focus(first); runtime.Bind(peer);
  input.commit_text = "Local prefix. "; runtime.Capture(first, &input); runtime.Paste("External suffix.");
  runtime.ObserveForeground(other.process);
  Check(runtime.Snapshot()["source"] == "Local prefix. External suffix.", "mixed imported draft is retained as one user workspace");
  runtime.DiscardBuffer();
  config.reset_on_app_switch = false;
  Check(runtime.Configure(config, {}, false, nullptr), "disable application switch reset");
  runtime.Focus(first); runtime.Bind(peer); input.commit_text = "Kept by preference."; runtime.Capture(first, &input);
  runtime.Focus(other);
  Check(runtime.Snapshot()["source"] == "Kept by preference.", "disabled reset keeps native content across applications");
  Check(runtime.Configure(original, {}, false, nullptr), "restore application switch preferences");
}
int main() {
  const auto root = std::filesystem::temp_directory_path() /
      (L"rimes-runtime-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  std::filesystem::create_directories(root);
  Check(SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str()) != 0, "isolated runtime preferences");
  TestFreshProfileRequiresChatGPTOptIn();
  // Provider and lifecycle fixtures below explicitly use ChatGPT. This is a
  // test grant, never an implicit enable in the production Runtime constructor.
  workbench::OfficialPluginStore fixture_plugins;
  Check(fixture_plugins.Install(official::kCodex, nullptr) && fixture_plugins.Enable(official::kCodex, true, nullptr), "explicit ChatGPT fixture opt-in");
  auto fixture = workbench::Settings{}; fixture.close_after_send = false;
  const auto run = [](auto test) {
    Check(workbench::OfficialPluginStore{}.SaveBufferSelection("", nullptr), "isolate remembered selection between fixtures");
    test();
  };
  run(TestTargets);
  run(TestPeripheralLease);
  run(TestPeripheralImagePasteTicket);
  Check(workbench::SaveSettings(fixture, nullptr), "retain Buffer for multi-step runtime fixtures");
  run(TestNativeShortcutAutoSendAndCopy);
  run(TestDisposableMaintenanceContent);
  run(TestExplicitOpeningPlacementRevision);
  run(TestCommandModifiersDoNotBecomeBufferCommands);
  run(TestBufferPluginCycle);
  run(TestNativePluginShortcutLease);
  run(TestPluginSwitchFocusNotifications);
  run(TestRememberedBufferPlugin);
  run(TestBufferLifecycleParity);
  run(TestBufferFocusFoldAndResume);
  run(TestCapturedCompositionLifecycle);
  run(TestUnhandledModifierCommitIsCapturedWithoutChangingKeyOwnership);
  run(TestFullBufferBlocksShiftWithoutChangingDraft);
  run(TestPendingResultBlocksShiftAndRetainsOriginalAcknowledgement);
  run(TestOwnedSendKeepsItsLifecycleAcrossModifierChanges);
  run(TestAltShiftReturnTapHoldAndComposition);
  run(TestConfiguredSendAndCancellation);
  run(TestTranslationModeArmsAutomaticSentencePipeline);
  run(TestBackgroundTranslationResources);
  run(TestTranslationSpeechAcknowledgementAndProtection);
  run(TestTranslationLanguageChangeRejectsLateOutput);
  run(TestOpeningGeometryIsBoundToOneExactSummon);
  run(TestPluginAuthority);
  run(TestReturningToInputRejectsLateResultsAndDropsQueuedTranslation);
  run(TestReturningToInputRetainsCompletedResultsAndAnIssuedDelivery);
  run(TestReturningToInputInvalidatesReadyResultOnRealEdit);
  run(TestChordFallback);
  run(TestCodexConnectorAndReturn);
  run(TestStreamingInferenceAndRetirement);
  run(TestReadinessConfigurationCancellationAndWaitingClock);
  run(TestResetOnApplicationSwitchPreservesExternalDrafts);
  std::filesystem::remove_all(root);
  std::cout << "Runtime plugin authority and chord fallback tests passed\n";
}
