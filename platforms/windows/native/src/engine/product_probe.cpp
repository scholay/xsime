#include <atomic>
#include <filesystem>
#include <iostream>
#include <vector>

#include "rime_engine.hpp"
#include "../broker/key_translation.hpp"
#include "../broker/broker_connection.hpp"
using namespace rimes::windows::engine;

namespace {
// The Runtime owns production preferences/plugin state, so keep this probe's
// state separate from both the caller's settings and its isolated Rime userdb.
class RuntimePreferences {
 public:
  explicit RuntimePreferences(const std::filesystem::path& log_dir) {
    static std::atomic<unsigned long long> serial{0};
    wchar_t previous[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", previous, 32768);
    if (length >= 32768) return;
    if (length) previous_ = previous;
    const auto root = log_dir / (L"workbench-shift-probe-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial));
    if (!std::filesystem::create_directory(root)) return;
    active_ = SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str()) != 0;
  }
  ~RuntimePreferences() {
    if (active_)
      SetEnvironmentVariableW(L"LOCALAPPDATA",
                              previous_.empty() ? nullptr : previous_.c_str());
  }
  bool active() const { return active_; }
 private:
  std::wstring previous_;
  bool active_ = false;
};

bool TestNativeIsaac2026(RimeEngine& engine, const std::filesystem::path& log_dir) {
  using namespace rimes::windows;
  const auto guarded=engine.CreateSession(nullptr);
  EngineSnapshot before, changed; bool replaced=false;
  if(!guarded || !engine.Configure(guarded,"rime_ice",false,false,false,nullptr) ||
      !engine.ProcessChordText(guarded,"n'","",{},&before,&replaced,nullptr) ||
      !engine.ProcessKey(guarded,'a',0,&changed,nullptr) ||
      !engine.ProcessChordText(guarded,"i","ni'",before,&changed,&replaced,nullptr) || replaced) {
    std::cerr<<"Chord rewrite must reject changed engine preedit\n"; return false;
  }
  engine.DestroySession(guarded);
  RuntimePreferences preferences(log_dir);
  if (!preferences.active()) return false;
  workbench::Settings initial; initial.schema = "my_combo";
  initial.chord_enabled = true;  // This fixture explicitly exercises opt-in chord input.
  initial.close_after_send = false;
  if (!workbench::SaveSettings(initial, nullptr)) return false;
  workbench::Runtime runtime;
  if (runtime.Configuration().schema != "my_combo") return false;
  broker::BrokerConnection connection(0, &engine, &runtime);
  const auto peer = GetCurrentProcessId() + 1;
  std::uint32_t request_id = 1;
  std::uint64_t sequence = 1;
  core::Frame response;
  std::vector<std::byte> payload;
  auto exchange = [&](core::MessageType type, std::vector<std::byte> bytes) {
    core::Frame request; request.header.message_type=type; request.header.request_id=request_id++;
    request.payload=std::move(bytes);
    return connection.Handle(request,peer,&response)==broker::ClientAction::kContinue;
  };
  core::InputSessionOpened opened;
  if (!core::EncodeClientHello({peer,0,core::kModifierSnapshotsCapability | core::kPendingChordCapability,"IsaacProbe"},&payload) ||
      !exchange(core::MessageType::kClientHello,std::move(payload)) ||
      !core::EncodeOpenInputSession({1,{}},&payload) ||
      !exchange(core::MessageType::kOpenInputSession,std::move(payload)) ||
      !core::DecodeInputSessionOpened(response.payload,&opened)) return false;
  core::InputState state;
  auto key = [&](unsigned vk,bool down) {
    core::KeyEvent e; e.session_id=opened.session_id; e.sequence_id=sequence++; e.virtual_key=vk;
    e.event_flags=down?static_cast<unsigned>(core::KeyEventFlags::kKeyDown):0;
    return core::EncodeKeyEvent(e,&payload) && exchange(core::MessageType::kKeyEvent,std::move(payload)) &&
        core::DecodeInputState(response.payload,&state);
  };
  auto batch = [&](const std::string& keys) {
    for (const char c:keys) if(!key(static_cast<unsigned>(c-'a'+'A'),true)) return false;
    for (const char c:keys) if(!key(static_cast<unsigned>(c-'a'+'A'),false)) return false;
    return true;
  };
  // Exercise the production Broker path, including the guarded native rewrite
  // against a real librime session and the normal TSF commit snapshot.
  if(!batch("dv") || !batch("i") || !key(VK_SPACE,true) || state.commit_text!="你" || !key(VK_SPACE,false)) {
    std::cerr<<"Native isaac2026 adjacent batches failed\n"; return false;
  }
  if(!batch("dvi") || !key(VK_SPACE,true) || state.commit_text!="你" || !key(VK_SPACE,false)) return false;
  if(!batch("dv") || !key('I',true) || !key(VK_SPACE,true) || state.commit_text!="你" ||
      !key('I',false) || !key(VK_SPACE,false)) {
    std::cerr<<"Adjacent chord must settle before a command while a key remains held\n"; return false;
  }
  if(!key('D',true) || !(state.state_flags & static_cast<unsigned>(core::InputStateFlags::kPendingChord)) ||
      !state.composition.empty() || !key(VK_ESCAPE,true) ||
      (state.state_flags & static_cast<unsigned>(core::InputStateFlags::kPendingChord)) || !key('D',false)) {
    std::cerr<<"Pending-key routing state must not fabricate preedit or survive cancellation\n"; return false;
  }
  auto config=runtime.Configuration(); config.chord_profile=workbench::BuiltinChordProfile();
  for(auto& mapping:config.chord_profile["mappings"]) if(mapping["keys"]=="dvi") mapping["output"]="hao";
  if(!runtime.Configure(config,{},false,nullptr) || !batch("dvi") || !key(VK_SPACE,true) || state.commit_text!="好" || !key(VK_SPACE,false)) {
    std::cerr<<"Applied custom keymap did not change real Rime output\n"; return false;
  }
  const workbench::Target target{peer,opened.session_id,1};
  runtime.Focus(target); runtime.Bind(peer);
  // A command before the final key release must settle the physical batch
  // before Buffer decides whether it is sending a draft or selecting preedit.
  if(!key('D',true) || !key('V',true) || !key('I',true) || !key(VK_SPACE,true) ||
      !state.commit_text.empty() || runtime.Snapshot()["source"]!="好") {
    std::cerr<<"Pending chord did not settle into Buffer\n"; return false;
  }
  if(!key('D',false) || !key('V',false) || !key('I',false) || runtime.Snapshot()["source"]!="好") return false;
  // Closing the actual TSF session revokes unfinished chord and rewrite state.
  if(!key('D',true) || !key('V',true) || !core::EncodeCloseInputSession({opened.session_id},&payload) ||
      !exchange(core::MessageType::kCloseInputSession,std::move(payload)) ||
      !core::EncodeOpenInputSession({2,{}},&payload) ||
      !exchange(core::MessageType::kOpenInputSession,std::move(payload)) ||
      !core::DecodeInputSessionOpened(response.payload,&opened) || !batch("i") ||
      state.composition.find('n')!=std::string::npos || state.composition.find("hao")!=std::string::npos) return false;
  std::cout<<"Native isaac2026 real-Rime batches, applied keymap, Buffer and session isolation passed\n";
  return true;
}

bool TestPausedBufferShift(RimeEngine& engine,
                          const std::filesystem::path& log_dir) {
  using namespace rimes::windows;
  constexpr auto down = static_cast<std::uint32_t>(core::KeyEventFlags::kKeyDown);
  constexpr auto repeat = static_cast<std::uint32_t>(core::KeyEventFlags::kRepeat);
  constexpr auto tap = static_cast<std::uint32_t>(core::KeyEventFlags::kShiftTap);
  constexpr auto shift = static_cast<std::uint32_t>(core::KeyModifiers::kShift);
  for (const auto capabilities : {0ULL, core::kModifierSnapshotsCapability}) {
    for (const bool pending_result : {false, true}) {
      // Plugin selection is intentionally remembered across Runtime instances.
      // Each scenario needs its own preferences, otherwise the translated-result
      // case changes the following plain-input case into translation mode.
      RuntimePreferences preferences(log_dir);
      if (!preferences.active()) {
        std::cerr << "Could not isolate Buffer probe preferences";
        return false;
      }
      workbench::Runtime runtime([](const workbench::Settings&, const workbench::Generation&,
          const std::function<bool(const std::string&)>& chunk,
          const std::function<bool()>&, std::string*) {
        return chunk("Translated output.");  // Deterministic, no network/secret.
      });
      broker::BrokerConnection connection(0, &engine, &runtime);
      // A direct-frame fixture supplies the same verified peer to Hello and
      // Handle. Production pipe/SID authentication is not replaced or relaxed.
      const auto peer = GetCurrentProcessId() + 1;
      std::uint32_t request_id = 1;
      core::Frame response;
      std::vector<std::byte> payload;
      auto exchange = [&](core::MessageType type, std::vector<std::byte> bytes) {
        core::Frame request;
        request.header.message_type = type;
        request.header.request_id = request_id++;
        request.payload = std::move(bytes);
        return connection.Handle(request, peer, &response) == broker::ClientAction::kContinue;
      };
      auto fail = [&](const char* reason) {
        std::cerr << "Paused Buffer Shift failed: " << reason << " capabilities="
                  << capabilities << " pending_result=" << pending_result << '\n';
        return false;
      };
      if (!core::EncodeClientHello({peer, 0, capabilities, "PausedBufferProbe"}, &payload) ||
          !exchange(core::MessageType::kClientHello, std::move(payload))) return fail("Hello");
      core::InputSessionOpened opened;
      if (!core::EncodeOpenInputSession({1, {}}, &payload) ||
          !exchange(core::MessageType::kOpenInputSession, std::move(payload)) ||
          !core::DecodeInputSessionOpened(response.payload, &opened)) return fail("open session");
      const workbench::Target target{peer, opened.session_id, 1};
      runtime.Focus(target);
      runtime.Bind(peer);
      if (!runtime.Capturing(target)) return fail("capture binding");
      if (pending_result) {
        // Complete translation before composing another word. Switching
        // plugins with an owned preedit intentionally queues settlement, and
        // returning to plain input parks the translation rather than sending it.
        runtime.Paste("Original.");
        runtime.Generate(true);
        const auto deadline = GetTickCount64() + 3000;
        while (runtime.Snapshot()["result"] != "Translated output." && GetTickCount64() < deadline)
          Sleep(2);
        if (runtime.Snapshot()["result"] != "Translated output.") return fail("result generation");
      }
      std::uint64_t sequence = 1;
      auto key = [&](std::uint32_t vk, std::uint32_t flags,
                     std::uint32_t modifiers, core::InputState* state) {
        core::KeyEvent event;
        event.session_id = opened.session_id;
        event.sequence_id = sequence++;
        event.virtual_key = vk;
        event.event_flags = flags;
        event.modifiers = modifiers;
        event.repeat_count = (flags & repeat) ? 2U : 1U;
        return core::EncodeKeyEvent(event, &payload) &&
               exchange(core::MessageType::kKeyEvent, std::move(payload)) &&
               core::DecodeInputState(response.payload, state);
      };
      core::InputState state;
      if (!key('N', down, 0, &state) || !key('I', down, 0, &state) ||
          state.composition != "ni" || runtime.Snapshot()["preedit"] != "ni")
        return fail("initial real Rime preedit");
      const auto revision = state.revision;
      if (pending_result) {
        runtime.Send(false);
        if (runtime.Snapshot()["status"] != "Sending...") return fail("pending result delivery");
      } else {
        runtime.Paste(std::string(workbench::Model::kLimit, 'x'));
      }
      const auto before = runtime.Snapshot();
      for (const auto vk : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT}) {
        for (const auto flags : {down, down | repeat, 0U, tap}) {
          if (!key(vk, flags, (flags & down) ? shift : 0, &state) ||
              state.state_flags != 0 || state.revision != revision ||
              !state.commit_text.empty() || !state.composition.empty())
            return fail("paused Shift acquired host ownership or emitted a snapshot");
          const auto after = runtime.Snapshot();
          for (const auto* field : {"source", "result", "preedit", "source_blocks",
                                    "result_blocks", "capture", "target_pid"})
            if (after[field] != before[field]) return fail("paused Shift changed retained input");
        }
      }
      // Release either pause with the original delivery/ack contract. Do not
      // rebind/reset the engine: the same preedit must still complete below.
      if (!pending_result) runtime.Send(false);
      auto delivery = runtime.Control({{"op", "wait"}, {"session", opened.session_id}}, peer);
      if (delivery.value("kind", "") == "capture")
        delivery = runtime.Control({{"op", "wait"}, {"session", opened.session_id}}, peer);
      if (delivery.value("kind", "") != "deliver" ||
          delivery["text"] != before[pending_result ? "result" : "source"])
        return fail("original delivery changed");
      const core::Json acknowledgement{{"op", "ack"}, {"session", opened.session_id},
          {"request", delivery["request"]}, {"accepted", true}};
      runtime.Control(acknowledgement, peer);
      if (runtime.Snapshot()["source"] != "" || runtime.Snapshot()["result"] != "" ||
          runtime.Snapshot()["preedit"] != "ni") return fail("ack lost original preedit");
      const auto acknowledged = runtime.Snapshot();
      runtime.Control(acknowledgement, peer);
      if (runtime.Snapshot() != acknowledged) return fail("duplicate ack mutated retained input");
      if (!key('H', down, 0, &state) || !key('A', down, 0, &state) ||
          !key('O', down, 0, &state) || !key(VK_SPACE, down, 0, &state) ||
          !state.commit_text.empty() || runtime.Snapshot()["source"] != "你好" ||
          runtime.Snapshot()["preedit"] != "")
        return fail("ordinary input did not resume the unchanged Chinese preedit");
      std::cout << "Paused Buffer Shift passed: capabilities=" << capabilities
                << " pending_result=" << pending_result << '\n';
    }
  }
  return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 5) return 2;
  if (GetACP() != CP_UTF8) {
    std::cerr << "Engine process must declare UTF-8 paths in its manifest\n";
    return 1;
  }
  RimeEngineOptions options;
  options.dll_path = argv[1];
  options.shared_data_dir = argv[2];
  options.user_data_dir = argv[3];
  options.log_dir = argv[4];
  options.full_maintenance_check = true;
  std::filesystem::create_directories(options.user_data_dir);
  std::filesystem::create_directories(options.log_dir);
  RimeEngine engine;
  std::string error;
  if (!engine.Start(options, &error)) {
    std::cerr << error;
    return 1;
  }
  auto session = engine.CreateSession(&error);
  if (!session) return 1;
  for (const auto* schema : {"rime_ice", "double_pinyin", "double_pinyin_flypy",
                             "wubi86", "lingxi_wubi", "english", "my_combo"}) {
    if (!engine.Configure(session, schema, false, false, false, &error)) return 1;
    for (const char digit : std::string("0123456789")) {
      EngineSnapshot idle;
      if (!engine.ProcessKey(session, digit, 0, &idle, &error) || idle.handled ||
          idle.composing || !idle.commit_text.empty()) {
        std::cerr << "Idle digit contract differs for product schema " << schema;
        return 1;
      }
    }
    if (!engine.Configure(session, schema, true, false, false, &error) ||
        !engine.IsAsciiMode(session)) return 1;
    std::cout << "Idle digit / authoritative ASCII mode passed: " << schema << '\n';
  }
  struct Case {
    const char* schema;
    const char* keys;
    bool traditional;
    const char* expected;
    const char* hint = "";
  };
  const std::vector<Case> cases = {{"rime_ice", "nihao", false, "你好"},
                                   {"double_pinyin", "ni", false, "你"},
                                   {"double_pinyin_flypy", "ni", false, "你"},
                                   {"wubi86", "wq", false, "你"},
                                   {"wubi86", "zni", false, "你", "wq"},
                                   {"wubi86", "znihao", false, "你好", "wqvb"},
                                   {"lingxi_wubi", "wq", false, "你"},
                                   {"lingxi_wubi", "nihao", false, "你好", "wqvb"},
                                   {"lingxi_wubi", "zni", false, "你", "wq"},
                                   {"lingxi_wubi", "han", true, "漢"},
                                   {"english", "hello", false, "hello"},
                                   {"rime_ice", "han", true, "漢"},
                                   {"wubi86", "ic", true, "漢"}};
  for (const auto& item : cases) {
    if (!engine.Configure(session, item.schema, false, item.traditional, false,
                          &error)) {
      std::cerr << "Configure failed " << item.schema;
      return 1;
    }
    EngineSnapshot snapshot;
    std::string committed;
    for (const char* key = item.keys; *key; ++key) {
      if (!engine.ProcessKey(session, *key, 0, &snapshot, &error)) return 1;
      committed += snapshot.commit_text;
    }
    bool found = committed.find(item.expected) != std::string::npos;
    for (const auto& candidate : snapshot.candidates)
      if (candidate.text == item.expected) {
        found = true;
        if (*item.hint) {
          found = candidate.comment.size() > 2 && candidate.comment.front() == '(' &&
                  candidate.comment.back() == ')' && candidate.comment.find(item.hint) != std::string::npos;
        }
        break;
      }
    if (!found) {
      std::cerr << "Product candidate assertion failed: " << item.schema
                << " traditional=" << item.traditional
                << " candidate_count=" << snapshot.candidates.size() << '\n';
      return 1;
    }
    std::cout << "Product scheme passed: " << item.schema
              << " traditional=" << item.traditional << '\n';
  }
  for (const auto* schema : {"wubi86", "lingxi_wubi"}) {
    for (const auto* code : {"voni", "wqiy", "zfiao", "fiao"}) {
      if (std::string(code) == "fiao" && std::string(schema) == "wubi86") continue;
      if (!engine.Configure(session, schema, false, false, false, &error)) return 1;
      EngineSnapshot state; int typed = 0;
      for (const char* key = code; *key; ++key) {
        if (!engine.ProcessKey(session, *key, 0, &state, &error)) return 1;
        const std::string expected = std::string(code) == "voni" && ++typed == 4 ? "灵犀" : "";
        if (state.commit_text != expected) {
          std::cerr << "Four-key unique commit failed: " << schema << '/' << code << '\n';
          return 1;
        }
      }
    }
  }
  for (const auto* schema : {"wubi86", "lingxi_wubi"}) {
    for (bool manual : {false, true}) for (bool fifth : {false, true}) {
      const std::map<std::string, bool> overrides = {{"rimes_wubi_manual_commit", manual}, {"rimes_wubi_fifth_code", fifth}};
      if (!engine.Configure(session, schema, false, false, false, &error, 9, overrides)) return 1;
      EngineSnapshot state;
      std::string committed;
      for (const char key : std::string("voni")) {
        if (!engine.ProcessKey(session, key, 0, &state, &error)) return 1;
        committed += state.commit_text;
      }
      if (committed != (manual ? "" : "灵犀")) { std::cerr << "Independent four-code preference failed"; return 1; }
      if (manual && fifth) {
        if (!engine.ProcessKey(session, 'w', 0, &state, &error) || state.commit_text != "灵犀" || state.composition != "w") {
          std::cerr << "Fifth-code continuation failed"; return 1;
        }
      }
      if (!engine.Configure(session, schema, false, false, false, &error, 9, overrides)) return 1;
      for (const char key : std::string("znihao")) {
        if (!engine.ProcessKey(session, key, 0, &state, &error) || !state.commit_text.empty()) {
          std::cerr << "Wubi preference committed reverse lookup"; return 1;
        }
      }
    }
    if (!engine.Configure(session, schema, false, false, false, &error)) return 1;
    EngineSnapshot state;
    for (const char key : std::string("voni")) if (!engine.ProcessKey(session, key, 0, &state, &error)) return 1;
    if (state.commit_text != "灵犀") { std::cerr << "Reset to inherited Wubi default failed"; return 1; }
  }
  for (const unsigned count : {1U, 5U, 9U}) {
    if (!engine.Configure(session, "rime_ice", false, false, false, &error, count)) return 1;
    EngineSnapshot page;
    if (!engine.ProcessKey(session, 'n', 0, &page, &error) ||
        !engine.ProcessKey(session, 'i', 0, &page, &error) ||
        page.page_size != count || page.candidates.empty() || page.candidates.size() > count) {
      std::cerr << "Engine candidate page size is not the requested count: " << count
                << " actual=" << page.page_size << " candidates=" << page.candidates.size(); return 1;
    }
    std::cout << "Candidate page size passed: " << count << '\n';
  }
  if (!engine.Configure(session, "my_combo", false, false, false, &error)) {
    std::cerr << "Configure failed my_combo: " << error; return 1;
  }
  EngineSnapshot chord;
  constexpr int released = 1 << 30;
  for (const char key : {'d', 'v', 'i'}) {
    if (!engine.ProcessKey(session, key, 0, &chord, &error) || !chord.commit_text.empty()) {
      std::cerr << "Chord key-down committed prematurely"; return 1;
    }
  }
  for (const char key : {'v', 'd', 'i'}) {
    if (!engine.ProcessKey(session, key, released, &chord, &error) || !chord.commit_text.empty()) {
      std::cerr << "Chord release failed or bypassed selection"; return 1;
    }
    if (key != 'i' && !chord.candidates.empty()) {
      std::cerr << "Chord resolved before the final key-up"; return 1;
    }
  }
  if (chord.candidates.empty() || chord.candidates.front().text != "你") {
    std::cerr << "Chord d+v+i did not resolve to ni / 你"; return 1;
  }
  if (!engine.ProcessKey(session, ' ', 0, &chord, &error) || chord.commit_text != "你") {
    std::cerr << "Chord candidate failed to commit exactly once"; return 1;
  }
  if (!engine.ProcessKey(session, ' ', released, &chord, &error) || !chord.commit_text.empty()) {
    std::cerr << "Chord space release duplicated commit"; return 1;
  }
  std::cout << "Product scheme passed: my_combo chord down/up and commit\n";
  // Exercise the actual Windows-to-Rime boundary, not only hand-written
  // keysyms. A shifted slash previously remained '/' on this path (#59).
  using namespace rimes::windows;
  constexpr auto shift = static_cast<std::uint32_t>(core::KeyModifiers::kShift);
  constexpr auto caps = static_cast<std::uint32_t>(core::KeyModifiers::kCapsLock);
  auto windows_key = [&](std::uint32_t vk, std::uint32_t modifiers,
                         bool down, EngineSnapshot* snapshot) {
    core::KeyEvent event;
    event.virtual_key = vk;
    event.modifiers = modifiers;
    event.event_flags = down ? static_cast<std::uint32_t>(core::KeyEventFlags::kKeyDown) : 0;
    const auto translated = broker::TranslateWindowsKey(event);
    return translated && engine.ProcessKey(session, translated->keycode,
                                          translated->modifiers, snapshot, &error);
  };
  for (const bool ascii_punctuation : {false, true}) {
    if (!engine.Configure(session, "rime_ice", false, false, ascii_punctuation, &error)) return 1;
    EngineSnapshot punctuation;
    // ascii_punct deliberately leaves ASCII punctuation to the host. The
    // translated symbol must still be '?' so Buffer can use the same fallback.
    const auto question = broker::TranslateWindowsKey(core::KeyEvent{
        .virtual_key = 0xbf, .modifiers = shift,
        .event_flags = static_cast<std::uint32_t>(core::KeyEventFlags::kKeyDown)});
    if (!question || question->keycode != '?' ||
        !windows_key(0xbf, shift, true, &punctuation) ||
        (ascii_punctuation ? (punctuation.handled || !punctuation.commit_text.empty())
                           : (!punctuation.handled || punctuation.commit_text != "？"))) {
      std::cerr << "Shift+/ did not commit the configured question mark: ascii="
                << ascii_punctuation << " handled=" << punctuation.handled
                << " commit=" << punctuation.commit_text
                << " composition=" << punctuation.composition
                << " candidates=" << punctuation.candidates.size()
                << " error=" << error << '\n';
      for (const auto& candidate : punctuation.candidates)
        std::cerr << "Punctuation candidate=" << candidate.text << '\n';
      return 1;
    }
    // Releasing Shift first must not cause a slash or a duplicate commit.
    if (!windows_key(0xbf, 0, false, &punctuation) || !punctuation.commit_text.empty()) {
      std::cerr << "Question mark key-up duplicated text"; return 1;
    }
    if (!windows_key(0xbf, 0, true, &punctuation) ||
        (ascii_punctuation ? (punctuation.handled || !punctuation.commit_text.empty())
                           : punctuation.commit_text != "/")) {
      std::cerr << "Unshifted slash changed"; return 1;
    }
    if (!windows_key(0xbf, 0, false, &punctuation) || !punctuation.commit_text.empty()) return 1;
  }
  if (!engine.Configure(session, "english", false, false, false, &error)) return 1;
  EngineSnapshot english;
  for (const auto& [vk, modifiers] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
           {'H', shift}, {'E', 0}, {'L', shift}, {'L', 0}, {'O', 0}}) {
    EngineSnapshot release;
    if (!windows_key(vk, modifiers, true, &english) ||
        !windows_key(vk, 0, false, &release) || !release.commit_text.empty()) return 1;
  }
  bool exact_case = english.composition == "HeLlo";
  for (const auto& candidate : english.candidates)
    exact_case = exact_case || candidate.text == "HeLlo";
  if (!exact_case) {
    std::cerr << "English discarded Shift/Caps capitalization"; return 1;
  }
  // The shared English schema uses good_old_caps_lock: Caps letters are direct
  // host input rather than schema composition, with Shift inverting their case.
  for (const auto modifiers : {caps, shift | caps}) {
    if (!engine.Configure(session, "english", false, false, false, &error)) return 1;
    core::KeyEvent event;
    event.virtual_key = 'L';
    event.modifiers = modifiers;
    event.event_flags = static_cast<std::uint32_t>(core::KeyEventFlags::kKeyDown);
    const auto translated = broker::TranslateWindowsKey(event);
    EngineSnapshot direct;
    if (!translated || translated->keycode != (modifiers == caps ? 'L' : 'l') ||
        !windows_key('L', modifiers, true, &direct) || direct.handled ||
        !direct.commit_text.empty()) {
      std::cerr << "English Caps direct input changed"; return 1;
    }
    if (!windows_key('L', 0, false, &direct) || !direct.commit_text.empty()) return 1;
  }
  std::cout << "Windows printable keys passed: question/slash, Shift release, English case\n";
  // ascii_composer returns kNoop for both Shift phases even when release
  // commits existing raw input. A successful process_key call alone cannot
  // prove that the resulting text was collected promptly.
  for (const auto* schema : {"rime_ice", "double_pinyin", "double_pinyin_flypy", "wubi86", "lingxi_wubi", "english"}) {
    if (!engine.Configure(session, schema, false, false, false, &error)) return 1;
    const std::string code = std::string(schema) == "wubi86" ? "wq" : "ni";
    EngineSnapshot before;
    for (const char key : code)
      if (!engine.ProcessKey(session, key, 0, &before, &error)) return 1;
    EngineSnapshot down, up;
    if (!engine.ProcessKey(session, 0xffe1, 1, &down, &error) ||
        !engine.ProcessKey(session, 0xffe1, released, &up, &error) ||
        down.handled || up.handled || up.commit_text != code || up.composing) {
      std::cerr << "Shift raw-code snapshot lost: " << schema
                << " commit=" << up.commit_text; return 1;
    }
    EngineSnapshot ascii;
    if (!engine.ProcessKey(session, 'a', 0, &ascii, &error) || ascii.handled ||
        !ascii.commit_text.empty()) {
      std::cerr << "Shift mode did not pass ASCII through cleanly: " << schema; return 1;
    }
  }
  std::cout << "Product Shift raw-code and ASCII passthrough passed\n";

  for (const auto side : {0xffe1, 0xffe2}) {
    if (!engine.Configure(session, "my_combo", false, false, false, &error)) return 1;
    EngineSnapshot composed;
    for (const char key : {'d', 'v', 'i'})
      if (!engine.ProcessKey(session, key, 0, &composed, &error)) return 1;
    for (const char key : {'v', 'd', 'i'})
      if (!engine.ProcessKey(session, key, released, &composed, &error)) return 1;
    if (!engine.ProcessShiftTap(session, side, released, &composed, &error) ||
        composed.handled || !composed.modifier_snapshot ||
        composed.commit_text != "ni" || composed.composing) {
      std::cerr << "Chord qualified Shift tap lost raw code: " << side
                << " commit=" << composed.commit_text << " error=" << error; return 1;
    }
  }
  std::cout << "Both product chord Shift styles passed\n";
  // Leave the isolated engine's default schema in Chinese before opening
  // real BrokerConnection sessions. The direct frames exercise negotiation
  // without modifying pipe authentication or registering an input method.
  if (!engine.Configure(session, "rime_ice", false, false, false, &error)) return 1;
  engine.DestroySession(session);
  for (const auto capabilities : {0ULL, core::kModifierSnapshotsCapability,
         core::kModifierSnapshotsCapability | core::kKeyRoutingCapability}) {
    broker::BrokerConnection connection(0, &engine);
    std::uint32_t request_id = 1;
    auto exchange = [&](core::MessageType type, std::vector<std::byte> payload,
                        core::Frame* response) {
      core::Frame frame;
      frame.header.message_type = type;
      frame.header.request_id = request_id++;
      frame.payload = std::move(payload);
      return connection.Handle(frame, GetCurrentProcessId(), response) ==
             broker::ClientAction::kContinue;
    };
    core::Frame response;
    std::vector<std::byte> payload;
    core::ClientHello hello{GetCurrentProcessId(), 0, capabilities, "ProductProbe"};
    core::BrokerHello broker_hello;
    if (!core::EncodeClientHello(hello, &payload) ||
        !exchange(core::MessageType::kClientHello, std::move(payload), &response) ||
        !core::DecodeBrokerHello(response.payload, &broker_hello) ||
        !(broker_hello.capabilities & core::kModifierSnapshotsCapability)) return 1;
    core::InputSessionOpened opened;
    if (!core::EncodeOpenInputSession({1, {}}, &payload) ||
        !exchange(core::MessageType::kOpenInputSession, std::move(payload), &response) ||
        !core::DecodeInputSessionOpened(response.payload, &opened)) return 1;
    const bool routing = (capabilities & core::kKeyRoutingCapability) != 0;
    if (opened.has_key_routing != routing || opened.ascii_mode) return 1;
    std::uint64_t sequence = 1;
    auto key = [&](std::uint32_t vk, std::uint32_t flags, std::uint32_t modifiers,
                   core::InputState* state) {
      core::KeyEvent event;
      event.session_id = opened.session_id;
      event.sequence_id = sequence++;
      event.virtual_key = vk;
      event.event_flags = flags;
      event.modifiers = modifiers;
      return core::EncodeKeyEvent(event, &payload) &&
             exchange(core::MessageType::kKeyEvent, std::move(payload), &response) &&
             core::DecodeInputState(response.payload, state);
    };
    constexpr auto down = static_cast<std::uint32_t>(core::KeyEventFlags::kKeyDown);
    constexpr auto tap = static_cast<std::uint32_t>(core::KeyEventFlags::kShiftTap);
    constexpr auto modifier_state = static_cast<std::uint32_t>(core::InputStateFlags::kModifierSnapshot);
    const auto ascii_state = routing
        ? static_cast<std::uint32_t>(core::InputStateFlags::kAsciiMode) : 0U;
    core::InputState state;
    if (!key('N', down, 0, &state) || !key('I', down, 0, &state) ||
        state.composition.empty()) return 1;
    if (capabilities == 0) {
      // The old client has neither the new flag nor a Shift gesture contract.
      if (!key(VK_LSHIFT, down, shift, &state) || state.state_flags != 0 ||
          !key(VK_LSHIFT, 0, 0, &state) || state.state_flags != 0 ||
          !state.commit_text.empty()) {
        std::cerr << "Legacy client received a new modifier snapshot"; return 1;
      }
      // Even an unnegotiated gesture must not arm the engine.
      if (!key(VK_LSHIFT, tap, 0, &state) || state.state_flags != 0 ||
          !key('H', down, 0, &state) || !key('A', down, 0, &state) ||
          !key('O', down, 0, &state) || !key(VK_SPACE, down, 0, &state) ||
          state.commit_text != "你好") {
        std::cerr << "Legacy Shift modified the new Broker's input session"; return 1;
      }
    } else {
      if (!key(VK_LSHIFT, tap, 0, &state) ||
          state.state_flags != (modifier_state | ascii_state) || state.commit_text != "ni" ||
          !key('A', down, 0, &state) || state.state_flags != ascii_state ||
          !state.commit_text.empty()) {
        std::cerr << "Negotiated Shift tap failed its real Broker snapshot contract"; return 1;
      }
    }
    if (!core::EncodeCloseInputSession({opened.session_id}, &payload) ||
        !exchange(core::MessageType::kCloseInputSession, std::move(payload), &response)) return 1;
  }
  std::cout << "Real Broker legacy/new client capability negotiation passed\n";
  if (!TestPausedBufferShift(engine, options.log_dir)) return 1;
  if (!TestNativeIsaac2026(engine, options.log_dir)) return 1;
  return 0;
}
