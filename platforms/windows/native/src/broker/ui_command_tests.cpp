#include "broker_connection.hpp"
#include "restart.hpp"
#include "../core/control.hpp"
#include "../workbench/window.hpp"

#include <cstdlib>
#include <iostream>
#include <filesystem>

namespace rimes::windows::broker {
struct CandidatePagingRegressionProbe {
  static ClientAction Respond(BrokerConnection& connection, const core::Frame& request,
                               core::InputState state, core::Frame* response) {
    return connection.RespondWithInputState(request, std::move(state), response);
  }
};
}

namespace {
using namespace rimes::windows;
int failures = 0;
void Check(bool condition, const char* reason) {
  if (!condition) { std::cerr << "FAIL: " << reason << '\n'; ++failures; }
}
core::Frame Frame(core::MessageType type, std::vector<std::byte> payload,
                  std::uint32_t id = 1) {
  core::Frame result;
  result.header.message_type = type;
  result.header.request_id = id;
  result.payload = std::move(payload);
  return result;
}
core::Frame Hello(DWORD process, DWORD session = 42, std::uint64_t capabilities = 0) {
  core::ClientHello hello;
  hello.process_id = process;
  hello.session_id = session;
  hello.capabilities = capabilities;
  hello.client_name = "settings command test";
  std::vector<std::byte> payload;
  Check(core::EncodeClientHello(hello, &payload), "hello encodes");
  return Frame(core::MessageType::kClientHello, std::move(payload));
}
core::Frame Command(core::Json value = {{"op", "open_settings"}}) {
  return Frame(core::MessageType::kControl, core::EncodeControl(value), 2);
}
void TestAuthenticatedCommandWithoutInputSession() {
  int requests = 0;
  // No engine or Runtime: opening settings must not create an input session,
  // change host focus, capture keys or read real preferences/credentials.
  broker::BrokerConnection connection(42, nullptr, nullptr,
                                      [&] { ++requests; return true; });
  core::Frame response;
  Check(connection.Handle(Hello(100), 100, &response) ==
            broker::ClientAction::kContinue &&
        response.header.message_type == core::MessageType::kBrokerHello,
        "verified hello succeeds");
  Check(connection.Handle(Command(), 100, &response) ==
            broker::ClientAction::kContinue,
        "settings command succeeds without an engine session");
  const auto output = core::DecodeControl(response.payload);
  Check(output && output->value("kind", "") == "ok" && requests == 1,
        "accepted settings request posts once");
  connection.Handle(Command(), 100, &response);
  Check(requests == 2, "a second explicit request can foreground settings");
}
void TestCandidatePagingCapability() {
  for (const auto capabilities : {0ULL, core::kCandidatePagingCapability}) {
    broker::BrokerConnection connection(42, nullptr);
    core::Frame response;
    Check(connection.Handle(Hello(100, 42, capabilities), 100, &response) ==
        broker::ClientAction::kContinue, "candidate paging hello succeeds");
    core::InputState state;
    state.session_id = 7; state.sequence_id = 1; state.revision = 1;
    state.state_flags = static_cast<std::uint32_t>(core::InputStateFlags::kHandled) |
        static_cast<std::uint32_t>(core::InputStateFlags::kComposing) |
        static_cast<std::uint32_t>(core::InputStateFlags::kCandidatesVisible) |
        static_cast<std::uint32_t>(core::InputStateFlags::kLastCandidatePage) |
        static_cast<std::uint32_t>(core::InputStateFlags::kMinusPageUp) |
        static_cast<std::uint32_t>(core::InputStateFlags::kMinusPagingOnly) |
        static_cast<std::uint32_t>(core::InputStateFlags::kEqualPageDown);
    state.composition = "a"; state.caret_utf16 = 1; state.page_size = 1;
    state.candidates = {{0, "word", "", "1"}};
    Check(broker::CandidatePagingRegressionProbe::Respond(connection,
        Frame(core::MessageType::kKeyEvent, {}, 2), state, &response) ==
        broker::ClientAction::kContinue, "candidate paging response encodes");
    core::InputState decoded;
    Check(core::DecodeInputState(response.payload, &decoded), "candidate paging response decodes");
    Check(((decoded.state_flags & static_cast<std::uint32_t>(core::InputStateFlags::kLastCandidatePage)) != 0)
        == (capabilities != 0), "only negotiated clients receive last-page metadata; loaded legacy DLLs stay compatible");
    Check((decoded.state_flags & core::kCandidatePageKeyFlags) ==
        (capabilities ? state.state_flags & core::kCandidatePageKeyFlags : 0U),
        "only negotiated clients receive explicit symbol paging metadata");
  }
}
void TestCommandRequiresVerifiedHelloAndInteractiveHandler() {
  int requests = 0;
  core::Frame response;
  broker::BrokerConnection unverified(42, nullptr, nullptr,
                                      [&] { ++requests; return true; });
  Check(unverified.Handle(Command(), 100, &response) ==
            broker::ClientAction::kCloseAfterResponse,
        "command before hello is rejected");
  Check(requests == 0, "unverified command never reaches UI");
  broker::BrokerConnection wrong_identity(42, nullptr, nullptr,
                                         [&] { ++requests; return true; });
  Check(wrong_identity.Handle(Hello(101), 100, &response) ==
            broker::ClientAction::kCloseAfterResponse,
        "claimed PID must match pipe peer");
  Check(requests == 0, "incorrect identity never reaches UI");
  broker::BrokerConnection wrong_session(42, nullptr, nullptr,
                                        [&] { ++requests; return true; });
  Check(wrong_session.Handle(Hello(100, 41), 100, &response) ==
            broker::ClientAction::kCloseAfterResponse,
        "claimed logon session must match the broker");
  Check(requests == 0, "incorrect logon session never reaches UI");
  broker::BrokerConnection headless(42, nullptr);
  headless.Handle(Hello(100), 100, &response);
  Check(headless.Handle(Command(), 100, &response) ==
            broker::ClientAction::kCloseAfterResponse &&
        response.header.message_type == core::MessageType::kError,
        "headless broker rejects UI commands");
}
void TestLatePeerAndMalformedCommandsCannotOpenSettings() {
  int requests = 0;
  broker::BrokerConnection connection(42, nullptr, nullptr,
                                      [&] { ++requests; return true; });
  core::Frame response;
  connection.Handle(Hello(100), 100, &response);
  Check(connection.Handle(Command(), 101, &response) ==
            broker::ClientAction::kCloseAfterResponse,
        "a changed peer identity cannot use a previous hello");
  Check(requests == 0, "late mismatched peer does not reach UI");
  Check(connection.Handle(Command({{"op", "open_settings"}, {"session", 7}}),
                          100, &response) ==
            broker::ClientAction::kCloseAfterResponse,
        "UI command refuses a manufactured input target");
  Check(requests == 0, "malformed command does not reach UI");
  auto invalid = Command();
  invalid.header.request_id = 0;
  Check(connection.Handle(invalid, 100, &response) ==
            broker::ClientAction::kCloseAfterResponse,
        "zero request identity is rejected");
  Check(requests == 0, "invalid request identity does not reach UI");
}
void TestStartupRetry() {
  bool ready = false;
  int posts = 0;
  broker::BrokerConnection connection(42, nullptr, nullptr, [&] {
    if (!ready) return false;
    ++posts; return true;
  });
  core::Frame response;
  connection.Handle(Hello(100), 100, &response);
  connection.Handle(Command(), 100, &response);
  auto output = core::DecodeControl(response.payload);
  Check(output && output->value("kind", "") == "starting" && posts == 0,
        "UI startup asks for bounded retry without pretending to post");
  ready = true;
  connection.Handle(Command(), 100, &response);
  output = core::DecodeControl(response.payload);
  Check(output && output->value("kind", "") == "ok" && posts == 1,
        "retry posts only after UI becomes ready");
}
void TestInputOnlyConnectionCannotReachWorkbench() {
  workbench::Runtime runtime;
  int requests = 0;
  const auto before = runtime.Snapshot();
  for (const auto* op : {"open_settings", "snapshot", "focus", "wait", "ack",
                         "toggle", "bind", "send", "generate", "buffer_transition", "focus_ready", "delivery_guard", "unknown"}) {
    broker::BrokerConnection connection(42, nullptr, &runtime,
                                        [&] { ++requests; return true; }, true);
    core::Frame response;
    Check(connection.Handle(Hello(100), 100, &response) ==
              broker::ClientAction::kContinue, "input-only hello succeeds");
    core::BrokerHello hello;
    Check(core::DecodeBrokerHello(response.payload, &hello) &&
              (hello.capabilities & 1ULL) == 0,
          "input-only endpoint never advertises workbench capability");
    Check(connection.Handle(Command({{"op", op}}), 100, &response) ==
              broker::ClientAction::kCloseAfterResponse &&
              response.header.message_type == core::MessageType::kError,
          "input-only endpoint rejects every non-input control");
  }
  Check(requests == 0 && runtime.Snapshot() == before,
        "restricted commands cannot open UI or mutate workbench state");
  broker::BrokerConnection connection(42, nullptr, &runtime, {}, true);
  core::Frame response;
  connection.Handle(Hello(100), 100, &response);
  Check(connection.Handle(Command({{"op", "candidate_guard"}, {"session", 99},
                                   {"revision", 1}, {"index", 0}}), 100, &response) ==
            broker::ClientAction::kContinue,
        "input-only endpoint retains candidate revision guard");
  const auto output = core::DecodeControl(response.payload);
  Check(output && output->value("kind", "") == "stale",
        "candidate guard refuses a foreign input session");
}
int posted_commands = 0;
LRESULT CALLBACK FixtureProcedure(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
  if (message >= WM_APP && message < 0xc000) {
    ++posted_commands;
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
void TestUiDispatcherRetiresItsWindow() {
  workbench::UiCommands commands;
  Check(commands.RequestSettings(), "dispatcher queues a settings request before UI creation");
  Check(commands.RequestSettings(), "duplicate startup requests coalesce");
  Check(!commands.RequestExit(), "maintenance exit waits for UI creation");
  WNDCLASSW fixture{};
  fixture.lpfnWndProc = FixtureProcedure;
  fixture.hInstance = GetModuleHandleW(nullptr);
  fixture.lpszClassName = L"Rimes.UiCommandFixture";
  RegisterClassW(&fixture);
  HWND window = CreateWindowW(fixture.lpszClassName, L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, fixture.hInstance, nullptr);
  Check(window != nullptr, "disposable message-only UI destination created");
  if (!window) return;
  commands.Attach(window);
  MSG startup{};
  while (PeekMessageW(&startup, window, 0, 0, PM_REMOVE)) DispatchMessageW(&startup);
  Check(posted_commands == 1, "queued startup request reaches the UI once after attachment");
  posted_commands = 0;
  Check(commands.RequestSettings(), "ready dispatcher posts a settings request");
  Check(posted_commands == 0, "request never executes UI work on the caller thread");
  MSG message{};
  while (PeekMessageW(&message, window, 0, 0, PM_REMOVE))
    DispatchMessageW(&message);
  Check(posted_commands == 1, "one request reaches the UI thread once");
  Check(commands.RequestExit(), "maintenance exit posts without saving Buffer");
  while (PeekMessageW(&message, window, 0, 0, PM_REMOVE))
    DispatchMessageW(&message);
  Check(posted_commands == 2, "maintenance exit is handled by the UI thread");
  commands.Attach(nullptr);
  Check(!commands.RequestSettings(), "retired HWND cannot receive a late request");
  Check(!commands.RequestExit(), "retired HWND cannot receive a late exit");
  DestroyWindow(window);
  UnregisterClassW(fixture.lpszClassName, fixture.hInstance);
}
void TestRestartParentIdentityAndExitRace() {
  Check(broker::WaitForRestartParent(0), "normal startup needs no predecessor");
  Check(!broker::WaitForRestartParent(GetCurrentProcessId()), "restart cannot wait for itself");
  wchar_t own[32768]{};
  Check(GetModuleFileNameW(nullptr, own, 32768) != 0, "restart fixture resolves its own executable");
  const auto launch = [&](const std::filesystem::path& executable, const wchar_t* delay) {
    auto command = L"\"" + executable.wstring() + L"\" --restart-fixture " + delay;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0, "owned restart fixture launches hidden");
    if (process.hThread) CloseHandle(process.hThread);
    return process;
  };
  auto live = launch(own, L"700");
  if (live.hProcess) {
    Check(broker::WaitForRestartParent(live.dwProcessId), "same executable user and session predecessor is allowed to exit");
    Check(WaitForSingleObject(live.hProcess, 0) == WAIT_OBJECT_0, "successful restart wait observes actual predecessor exit");
    Check(WaitForSingleObject(live.hProcess, 3000) == WAIT_OBJECT_0, "owned predecessor exits without termination");
    // Keep the process object alive while reproducing a child that gets
    // scheduled only after the parent exited. Token queries may now fail.
    Check(broker::WaitForRestartParent(live.dwProcessId), "already-exited predecessor does not fail a token-query startup race");
    CloseHandle(live.hProcess);
  }
  // A hard link must be on the executable's volume. The unique directory is
  // confined to the disposable build tree and never alters the original file.
  const auto directory = std::filesystem::path(own).parent_path() /
      (L"rimes-restart-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const auto alias_path = directory / L"same-broker-alias.exe";
  const bool linked = CreateHardLinkW(alias_path.c_str(), own, nullptr) != 0;
  Check(linked, "restart fixture creates a different path for the same executable file");
  if (linked) {
    auto alias = launch(alias_path, L"700");
    if (alias.hProcess) {
      Check(broker::WaitForRestartParent(alias.dwProcessId),
          "same file reached through a hard-link or junction alias is an allowed predecessor");
      Check(WaitForSingleObject(alias.hProcess, 0) == WAIT_OBJECT_0,
          "alias predecessor is actually awaited before restart");
      WaitForSingleObject(alias.hProcess, 3000);
      CloseHandle(alias.hProcess);
    }
  }
  const auto foreign_path = directory / L"different-broker.exe";
  const bool copied = std::filesystem::copy_file(own, foreign_path, std::filesystem::copy_options::none, error);
  Check(copied, "restart fixture creates its own differently located executable");
  if (copied) {
    auto foreign = launch(foreign_path, L"1500");
    if (foreign.hProcess) {
      Check(WaitForSingleObject(foreign.hProcess, 0) == WAIT_TIMEOUT, "foreign identity fixture is live");
      Check(!broker::WaitForRestartParent(foreign.dwProcessId),
          "live PID belonging to another executable is refused, including a reused predecessor PID");
      Check(WaitForSingleObject(foreign.hProcess, 0) == WAIT_TIMEOUT, "identity rejection does not terminate another process");
      Check(WaitForSingleObject(foreign.hProcess, 3000) == WAIT_OBJECT_0, "owned foreign fixture exits on its own");
      CloseHandle(foreign.hProcess);
    }
  }
  std::filesystem::remove(alias_path, error);
  std::filesystem::remove(foreign_path, error);
  std::filesystem::remove(directory, error);
}
}
int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--restart-fixture") {
    Sleep(static_cast<DWORD>(std::strtoul(argv[2], nullptr, 10)));
    return EXIT_SUCCESS;
  }
  TestRestartParentIdentityAndExitRace();
  TestCandidatePagingCapability();
  TestAuthenticatedCommandWithoutInputSession();
  TestCommandRequiresVerifiedHelloAndInteractiveHandler();
  TestLatePeerAndMalformedCommandsCannotOpenSettings();
  TestStartupRetry();
  TestInputOnlyConnectionCannotReachWorkbench();
  TestUiDispatcherRetiresItsWindow();
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
