#include "BrokerClient.h"
#include "../core/buffer_keys.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../core/broker_protocol.hpp"
#include "../core/broker_identity.hpp"
#include "Diagnostics.h"
#include "ModuleState.h"
#include "key_routing.hpp"
#include "broker_launch.hpp"
#include "input_pipe.hpp"

namespace rimes::windows::tsf {
namespace {

namespace protocol = rimes::windows::core;

// Connection work runs off the application's input thread.  Allow enough time
// for a cold Broker to accept, authenticate and create its first librime
// session instead of permanently giving up after one scheduler hiccup.
constexpr ULONGLONG kConnectBudgetMillis = 2000;
constexpr ULONGLONG kLaunchConnectBudgetMillis = 15000;
// Cold engine pages and the first user-dictionary commit can exceed 80 ms on
// 32-bit hosts. That deadline disconnected a healthy Broker and lost candidate
// selection. This is a response ceiling, not a delay for ordinary keys: return
// immediately on a reply, and still retire the connection without replay when
// an unresponsive Broker exceeds the bounded allowance.
constexpr ULONGLONG kKeyBudgetMillis = 200;
// Closing an input session is acknowledged by the Broker worker. A 10 ms
// deadline is shorter than a normal Windows scheduling quantum and spuriously
// disconnects during host composition termination. Keep session retirement
// bounded independently of engine processing; never replay a timed-out key.
constexpr ULONGLONG kCloseBudgetMillis = 80;
constexpr DWORD kConnectRetryMillis = 50;
constexpr DWORD kBusyPipeWaitMillis = 200;

class HandleCloser {
 public:
  void operator()(void* handle) const noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

class LibraryGuard {
 public:
  explicit LibraryGuard(HMODULE module) noexcept : module_(module) {}
  ~LibraryGuard() {
    if (module_ != nullptr) {
      FreeLibrary(module_);
    }
  }

  LibraryGuard(const LibraryGuard&) = delete;
  LibraryGuard& operator=(const LibraryGuard&) = delete;

 private:
  HMODULE module_;
};

DWORD RemainingWait(ULONGLONG deadline) noexcept {
  const ULONGLONG now = GetTickCount64();
  if (now >= deadline) {
    return 0;
  }
  return static_cast<DWORD>(std::min<ULONGLONG>(
      deadline - now, static_cast<ULONGLONG>(MAXDWORD - 1U)));
}

enum class RequestFailure {
  kNone,
  kInvalidArgument,
  kFrameEncodeFailed,
  kWriteTimedOut,
  kWriteFailed,
  kReadTimedOut,
  kReadFailed,
  kHeaderInvalid,
};

// Performs one bounded overlapped transfer.  On timeout/cancellation, the
// operation is cancelled and reaped before OVERLAPPED leaves the stack.
bool TransferExact(HANDLE pipe, bool write, std::span<std::byte> bytes,
                   ULONGLONG deadline, HANDLE cancellation_event,
                   bool* timed_out) noexcept {
  if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) {
    return false;
  }

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (cancellation_event != nullptr &&
        WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0) {
      return false;
    }
    if (RemainingWait(deadline) == 0) {
      if (timed_out != nullptr) {
        *timed_out = true;
      }
      return false;
    }

    ResetEvent(event.get());
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - offset,
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
    DWORD transferred = 0;
    const BOOL started = write ? WriteFile(pipe, bytes.data() + offset,
                                           requested, &transferred, &overlapped)
                               : ReadFile(pipe, bytes.data() + offset,
                                          requested, &transferred, &overlapped);
    if (!started) {
      const DWORD error = GetLastError();
      if (error != ERROR_IO_PENDING) {
        return false;
      }

      HANDLE waits[2] = {event.get(), cancellation_event};
      const DWORD wait_count = cancellation_event == nullptr ? 1U : 2U;
      const DWORD wait_result = WaitForMultipleObjects(wait_count, waits, FALSE,
                                                       RemainingWait(deadline));
      if (wait_result != WAIT_OBJECT_0) {
        if (timed_out != nullptr && wait_result == WAIT_TIMEOUT) {
          *timed_out = true;
        }
        CancelIoEx(pipe, &overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
        return false;
      }
      if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
        return false;
      }
    }
    if (transferred == 0 || transferred > requested) {
      return false;
    }
    offset += transferred;
  }
  return true;
}

bool WriteExact(HANDLE pipe, std::span<const std::byte> bytes,
                ULONGLONG deadline, HANDLE cancellation_event,
                bool* timed_out) noexcept {
  // WriteFile predates const-correct buffer annotations and never mutates this
  // buffer; keep the audited cast at this one boundary.
  return TransferExact(pipe, true,
                       {const_cast<std::byte*>(bytes.data()), bytes.size()},
                       deadline, cancellation_event, timed_out);
}

bool ReadExact(HANDLE pipe, std::span<std::byte> bytes, ULONGLONG deadline,
               HANDLE cancellation_event, bool* timed_out) noexcept {
  return TransferExact(pipe, false, bytes, deadline, cancellation_event,
                       timed_out);
}

bool RequestResponse(HANDLE pipe, protocol::MessageType request_type,
                     std::uint32_t request_id, std::vector<std::byte> payload,
                     ULONGLONG deadline, HANDLE cancellation_event,
                     protocol::Frame* response, RequestFailure* failure) {
  if (failure != nullptr) {
    *failure = RequestFailure::kNone;
  }
  if (response == nullptr || request_id == 0) {
    if (failure != nullptr) {
      *failure = RequestFailure::kInvalidArgument;
    }
    return false;
  }
  protocol::Frame request;
  request.header.message_type = request_type;
  request.header.request_id = request_id;
  request.payload = std::move(payload);
  std::vector<std::byte> encoded;
  if (!protocol::EncodeFrame(request, &encoded)) {
    if (failure != nullptr) {
      *failure = RequestFailure::kFrameEncodeFailed;
    }
    return false;
  }
  bool timed_out = false;
  if (!WriteExact(pipe, encoded, deadline, cancellation_event, &timed_out)) {
    if (failure != nullptr) {
      *failure = timed_out ? RequestFailure::kWriteTimedOut
                           : RequestFailure::kWriteFailed;
    }
    return false;
  }

  std::array<std::byte, protocol::kFrameHeaderSize> header_bytes{};
  timed_out = false;
  if (!ReadExact(pipe, header_bytes, deadline, cancellation_event,
                 &timed_out)) {
    if (failure != nullptr) {
      *failure = timed_out ? RequestFailure::kReadTimedOut
                           : RequestFailure::kReadFailed;
    }
    return false;
  }
  const protocol::HeaderDecodeResult header =
      protocol::DecodeFrameHeader(header_bytes);
  if (header.status != protocol::DecodeStatus::kComplete ||
      header.header.request_id != request_id ||
      (header.header.flags &
       static_cast<std::uint32_t>(protocol::FrameFlags::kResponse)) == 0 ||
      header.header.payload_size > protocol::kMaxPayloadSize) {
    if (failure != nullptr) {
      *failure = RequestFailure::kHeaderInvalid;
    }
    return false;
  }

  protocol::Frame decoded;
  decoded.header = header.header;
  decoded.payload.resize(header.header.payload_size);
  timed_out = false;
  if (!decoded.payload.empty() && !ReadExact(pipe, decoded.payload, deadline,
                                             cancellation_event, &timed_out)) {
    if (failure != nullptr) {
      *failure = timed_out ? RequestFailure::kReadTimedOut
                           : RequestFailure::kReadFailed;
    }
    return false;
  }
  *response = std::move(decoded);
  return true;
}

bool IsExpectedResponse(const protocol::Frame& response,
                        protocol::MessageType type) noexcept {
  return response.header.message_type == type &&
         response.header.flags ==
             static_cast<std::uint32_t>(protocol::FrameFlags::kResponse);
}

bool Utf8ToWide(std::string_view text, std::wstring* output) {
  if (output == nullptr) {
    return false;
  }
  if (text.empty()) {
    output->clear();
    return true;
  }
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  const int length = static_cast<int>(text.size());
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           text.data(), length, nullptr, 0);
  if (required <= 0) {
    return false;
  }
  std::wstring decoded(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length,
                          decoded.data(), required) != required) {
    return false;
  }
  *output = std::move(decoded);
  return true;
}

using OpenProcessTokenFunction = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
using GetTokenInformationFunction = BOOL(WINAPI*)(HANDLE,
                                                  TOKEN_INFORMATION_CLASS,
                                                  LPVOID, DWORD, PDWORD);

template <typename Function>
Function ResolveProcedure(HMODULE module, const char* name) noexcept {
  static_assert(sizeof(Function) == sizeof(FARPROC));
  const FARPROC raw = GetProcAddress(module, name);
  Function resolved = nullptr;
  std::memcpy(&resolved, &raw, sizeof(resolved));
  return resolved;
}

struct TokenIdentity {
  std::vector<std::byte> sid;
  std::wstring sid_string;
};

bool CopyAndFormatSid(PSID raw_sid, std::span<const std::byte> token_storage,
                      TokenIdentity* identity) {
  if (raw_sid == nullptr || identity == nullptr || token_storage.empty()) {
    return false;
  }
  const auto storage_begin =
      reinterpret_cast<std::uintptr_t>(token_storage.data());
  const auto storage_end = storage_begin + token_storage.size();
  const auto sid_begin = reinterpret_cast<std::uintptr_t>(raw_sid);
  if (sid_begin < storage_begin || sid_begin >= storage_end ||
      storage_end - sid_begin < offsetof(SID, SubAuthority)) {
    return false;
  }

  const auto* sid = static_cast<const SID*>(raw_sid);
  const std::size_t sub_authority_count = sid->SubAuthorityCount;
  if (sid->Revision != SID_REVISION ||
      sub_authority_count > SID_MAX_SUB_AUTHORITIES) {
    return false;
  }
  const std::size_t sid_size =
      offsetof(SID, SubAuthority) + sub_authority_count * sizeof(DWORD);
  if (sid_size > storage_end - sid_begin) {
    return false;
  }

  std::uint64_t authority = 0;
  for (const BYTE value : sid->IdentifierAuthority.Value) {
    authority = (authority << 8U) | value;
  }
  std::wostringstream formatted;
  formatted << L"S-" << static_cast<unsigned int>(sid->Revision) << L'-';
  if (sid->IdentifierAuthority.Value[0] != 0 ||
      sid->IdentifierAuthority.Value[1] != 0) {
    formatted << L"0x" << std::hex << std::setw(12) << std::setfill(L'0')
              << authority << std::dec;
  } else {
    formatted << authority;
  }
  for (std::size_t index = 0; index < sub_authority_count; ++index) {
    DWORD sub_authority = 0;
    std::memcpy(&sub_authority, &sid->SubAuthority[index], sizeof(DWORD));
    formatted << L'-' << sub_authority;
  }

  const std::size_t sid_offset =
      static_cast<std::size_t>(sid_begin - storage_begin);
  identity->sid.assign(
      token_storage.begin() + static_cast<std::ptrdiff_t>(sid_offset),
      token_storage.begin() +
          static_cast<std::ptrdiff_t>(sid_offset + sid_size));
  identity->sid_string = formatted.str();
  return !identity->sid.empty() && !identity->sid_string.empty();
}

bool ReadProcessIdentity(HANDLE process, TokenIdentity* identity) {
  if (process == nullptr || identity == nullptr) {
    return false;
  }
  HMODULE raw_advapi =
      LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (raw_advapi == nullptr) {
    return false;
  }
  LibraryGuard advapi(raw_advapi);
  const auto open_process_token = ResolveProcedure<OpenProcessTokenFunction>(
      raw_advapi, "OpenProcessToken");
  const auto get_token_information =
      ResolveProcedure<GetTokenInformationFunction>(raw_advapi,
                                                    "GetTokenInformation");
  if (open_process_token == nullptr || get_token_information == nullptr) {
    return false;
  }

  HANDLE raw_token = nullptr;
  if (!open_process_token(process, TOKEN_QUERY, &raw_token)) {
    return false;
  }
  UniqueHandle token(raw_token);
  DWORD required = 0;
  SetLastError(ERROR_SUCCESS);
  get_token_information(token.get(), TokenUser, nullptr, 0, &required);
  const DWORD sizing_error = GetLastError();
  if (required < sizeof(TOKEN_USER) || required > 64U * 1024U ||
      sizing_error != ERROR_INSUFFICIENT_BUFFER) {
    return false;
  }
  std::vector<std::byte> storage(required);
  if (!get_token_information(token.get(), TokenUser, storage.data(), required,
                             &required) ||
      required > storage.size()) {
    return false;
  }
  const auto* token_user = reinterpret_cast<const TOKEN_USER*>(storage.data());
  return CopyAndFormatSid(token_user->User.Sid, storage, identity);
}

std::uint64_t HashSid(std::wstring_view sid) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const wchar_t code_unit : sid) {
    hash ^= static_cast<std::uint16_t>(code_unit);
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool BuildEndpoint(std::wstring* endpoint, TokenIdentity* current_identity,
                   DWORD* current_session_id) {
  if (endpoint == nullptr || current_identity == nullptr ||
      current_session_id == nullptr ||
      !ReadProcessIdentity(GetCurrentProcess(), current_identity) ||
      !ProcessIdToSessionId(GetCurrentProcessId(), current_session_id)) {
    return false;
  }
  if (*current_session_id == 0) {
    // Session 0 is the services session on a real desktop. Hosted Windows
    // runners and some automated hosts still use it; opt in explicitly so a
    // production TSF host does not talk to a session-0 broker by accident.
    wchar_t allow[8]{};
    const DWORD length =
        GetEnvironmentVariableW(L"RIMES_ALLOW_SESSION_0", allow, 8);
    if (length == 0 || allow[0] == L'\0' || allow[0] == L'0') {
      return false;
    }
  }
  std::wostringstream suffix;
  suffix << L".v2.session-" << *current_session_id << L".user-" << std::hex
         << std::setw(16) << std::setfill(L'0')
         << HashSid(current_identity->sid_string);
  *endpoint = std::wstring(L"\\\\.\\pipe\\") + core::kBrokerObjectName + suffix.str();
  return true;
}

enum class ServerIdentityFailure {
  kNone,
  kProcessIdQueryFailed,
  kSessionQueryFailed,
  kSessionMismatch,
  kProcessOpenFailed,
  kTokenReadFailed,
  kSidMismatch,
};

bool VerifyServerIdentity(HANDLE pipe, DWORD expected_session_id,
                          const TokenIdentity& expected_identity,
                          DWORD* server_process_id,
                          ServerIdentityFailure* failure) {
  if (failure != nullptr) {
    *failure = ServerIdentityFailure::kNone;
  }
  if (server_process_id == nullptr) {
    if (failure != nullptr) {
      *failure = ServerIdentityFailure::kProcessIdQueryFailed;
    }
    return false;
  }
  ULONG raw_process_id = 0;
  if (!GetNamedPipeServerProcessId(pipe, &raw_process_id) ||
      raw_process_id == 0) {
    if (failure != nullptr) {
      *failure = ServerIdentityFailure::kProcessIdQueryFailed;
    }
    return false;
  }
  DWORD server_session_id = 0;
  if (!ProcessIdToSessionId(raw_process_id, &server_session_id)) {
    if (failure != nullptr) {
      *failure = ServerIdentityFailure::kSessionQueryFailed;
    }
    return false;
  }
  if (server_session_id != expected_session_id) {
    if (failure != nullptr) {
      *failure = ServerIdentityFailure::kSessionMismatch;
    }
    return false;
  }
  UniqueHandle process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, raw_process_id));
  if (!process) {
    if (failure != nullptr) {
      *failure = ServerIdentityFailure::kProcessOpenFailed;
    }
    return false;
  }
  TokenIdentity server_identity;
  if (!ReadProcessIdentity(process.get(), &server_identity)) {
    if (failure != nullptr) {
      *failure = ServerIdentityFailure::kTokenReadFailed;
    }
    return false;
  }
  if (server_identity.sid != expected_identity.sid) {
    if (failure != nullptr) {
      *failure = ServerIdentityFailure::kSidMismatch;
    }
    return false;
  }
  *server_process_id = raw_process_id;
  return true;
}

std::uint32_t ReadModifiers() noexcept {
  const auto down = [](int virtual_key) noexcept {
    return (static_cast<unsigned short>(GetKeyState(virtual_key)) & 0x8000U) !=
           0;
  };
  std::uint32_t modifiers = 0;
  if (down(VK_SHIFT)) {
    modifiers |= static_cast<std::uint32_t>(protocol::KeyModifiers::kShift);
  }
  if (down(VK_CONTROL)) {
    modifiers |= static_cast<std::uint32_t>(protocol::KeyModifiers::kControl);
  }
  if (down(VK_MENU)) {
    modifiers |= static_cast<std::uint32_t>(protocol::KeyModifiers::kAlt);
  }
  if (down(VK_LWIN) || down(VK_RWIN)) {
    modifiers |= static_cast<std::uint32_t>(protocol::KeyModifiers::kWindows);
  }
  if ((GetKeyState(VK_CAPITAL) & 1) != 0) {
    modifiers |= static_cast<std::uint32_t>(protocol::KeyModifiers::kCapsLock);
  }
  // Num Lock is a keyboard toggle, not a modifier for ordinary TSF key
  // events.  Forwarding its global state as librime's Mod2 turns a normal
  // confirmation key into Mod2+Space and prevents the selector's plain Space
  // binding from committing the highlighted candidate.  The physical
  // VK_NUMLOCK key is still translated explicitly by the Broker.
  if (down(VK_RMENU) && down(VK_CONTROL)) {
    modifiers |= static_cast<std::uint32_t>(protocol::KeyModifiers::kAltGr);
  }
  return modifiers;
}

bool IsModifierKey(WPARAM virtual_key) noexcept {
  switch (virtual_key) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_CAPITAL:
    case VK_NUMLOCK:
    case VK_SCROLL:
      return true;
    default:
      return false;
  }
}

bool IsShiftKey(WPARAM virtual_key) noexcept {
  return virtual_key == VK_SHIFT || virtual_key == VK_LSHIFT ||
         virtual_key == VK_RSHIFT;
}

bool HasCommandModifiers(std::uint32_t modifiers) noexcept {
  constexpr std::uint32_t commands =
      static_cast<std::uint32_t>(protocol::KeyModifiers::kControl) |
      static_cast<std::uint32_t>(protocol::KeyModifiers::kAlt) |
      static_cast<std::uint32_t>(protocol::KeyModifiers::kWindows) |
      static_cast<std::uint32_t>(protocol::KeyModifiers::kAltGr);
  return (modifiers & commands) != 0;
}

bool IsPrintableKey(WPARAM virtual_key) noexcept {
  return (virtual_key >= '0' && virtual_key <= '9') ||
         (virtual_key >= 'A' && virtual_key <= 'Z') ||
         (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_DIVIDE) ||
         (virtual_key >= VK_OEM_1 && virtual_key <= VK_OEM_3) ||
         (virtual_key >= VK_OEM_4 && virtual_key <= VK_OEM_8) ||
         virtual_key == VK_OEM_102 || virtual_key == VK_SPACE;
}

bool IsCompositionCommand(WPARAM virtual_key) noexcept {
  switch (virtual_key) {
    case VK_BACK:
    case VK_TAB:
    case VK_RETURN:
    case VK_ESCAPE:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_END:
    case VK_HOME:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_DELETE:
      return true;
    default:
      return false;
  }
}

bool SiblingBrokerPath(std::wstring* path) noexcept {
  if (path == nullptr) {
    return false;
  }
  HINSTANCE instance = module::Instance();
  if (instance == nullptr) {
    instance = GetModuleHandleW(nullptr);
  }
  wchar_t module_path[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(instance, module_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return false;
  }
  std::wstring directory(module_path, length);
  const std::size_t slash = directory.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return false;
  }
  directory.resize(slash + 1);
  auto parent = directory.substr(0, directory.size() - 1);
  parent = parent.substr(0, parent.find_last_of(L"\\/"));
  *path = parent + L"\\x64\\" + core::kBrokerExecutable;
  if (GetFileAttributesW(path->c_str()) == INVALID_FILE_ATTRIBUTES)
    *path = directory + core::kBrokerExecutable;
  const DWORD attributes = GetFileAttributesW(path->c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool LaunchBrokerProcess() noexcept {
  // The test runner owns the isolated Broker's data and lifetime. A failed
  // test must never launch the installed Broker or leave an untracked child.
  if (!core::kAllowBrokerAutoLaunch) return false;
  wchar_t loaded_path[MAX_PATH]{};
  const auto length = GetModuleFileNameW(module::Instance(), loaded_path, MAX_PATH);
  if (!length || length >= MAX_PATH || !IsRegisteredTsfModule(loaded_path))
    return false;
  std::wstring broker_path;
  if (!SiblingBrokerPath(&broker_path)) {
    return false;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  std::wstring command = L"\"" + broker_path + L"\"";
  std::wstring working_directory =
      broker_path.substr(0, broker_path.find_last_of(L"\\/"));
  const BOOL created =
      CreateProcessW(broker_path.c_str(), command.data(), nullptr, nullptr,
                     FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                     nullptr, working_directory.c_str(), &startup, &process);
  if (!created) {
    return false;
  }
  if (process.hThread != nullptr) {
    CloseHandle(process.hThread);
  }
  if (process.hProcess != nullptr) {
    CloseHandle(process.hProcess);
  }
  return true;
}

bool ShouldOfferKey(WPARAM virtual_key, std::uint32_t modifiers,
                    bool composing, bool capturing, bool ascii_mode) noexcept {
  if (virtual_key == 0 || virtual_key > 0xffU || IsModifierKey(virtual_key)) {
    return false;
  }
  constexpr std::uint32_t kShortcutModifiers =
      static_cast<std::uint32_t>(protocol::KeyModifiers::kControl) |
      static_cast<std::uint32_t>(protocol::KeyModifiers::kAlt) |
      static_cast<std::uint32_t>(protocol::KeyModifiers::kWindows) |
      static_cast<std::uint32_t>(protocol::KeyModifiers::kAltGr);
  if ((modifiers & kShortcutModifiers) != 0) {
    return false;
  }
  return (IsPrintableKey(virtual_key) && detail::RoutePrintableKey(
              virtual_key, modifiers, composing, capturing, ascii_mode)) ||
         (composing && IsCompositionCommand(virtual_key));
}

class NamedPipeBrokerClient final : public BrokerClient {
 public:
  NamedPipeBrokerClient() noexcept
      : stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

  ~NamedPipeBrokerClient() override {
    Disconnect();
    if (stop_event_ != nullptr) {
      CloseHandle(stop_event_);
      stop_event_ = nullptr;
    }
  }

  bool IsConnected() const noexcept override {
    return connected_.load(std::memory_order_acquire);
  }

  void SetNotificationWindow(HWND window) noexcept override {
    notification_window_.store(window);
  }
  bool Capturing() const noexcept override { return capturing_.load(); }
  bool IsBufferSendKey(WPARAM key) const noexcept override {
    const auto chord = static_cast<unsigned>(buffer_chords_.load() & 0xffffU);
    return core::IsBufferSendChord(static_cast<unsigned>(key), ReadModifiers(),
                                    chord & 0xffU, chord >> 8U);
  }
  std::uint64_t ConnectionGeneration() const noexcept override {
    return connection_generation_.load();
  }
  std::optional<core::Json> TakeNotification() override {
    std::lock_guard lock(notification_mutex_);
    if (notifications_.empty()) return {};
    auto value = std::move(notifications_.front());
    notifications_.pop_front();
    return value;
  }
  bool Control(core::Json message) noexcept override {
    try {
      std::unique_lock lock(io_mutex_, std::defer_lock);
      if (!lock.try_lock_for(std::chrono::milliseconds(2)) || !connected_.load() || !input_session_id_)
        return false;
      message["session"] = input_session_id_;
      protocol::Frame response;
      if (!RequestResponse(pipe_, protocol::MessageType::kControl,
                             next_request_id_++, core::EncodeControl(message),
                             GetTickCount64() + kKeyBudgetMillis, stop_event_,
                             &response, nullptr) ||
          !IsExpectedResponse(response, protocol::MessageType::kControlState)) {
        // An overdue reply must never become the next key's response.
        FailConnectionLocked();
        return false;
      }
      const auto decoded = core::DecodeControl(response.payload);
      if (!decoded) { FailConnectionLocked(); return false; }
      return decoded->value("kind", "") == "ok";
    } catch (...) {
      FailConnection();
      return false;
    }
  }

  bool SetContext(std::uint64_t context_id) noexcept override {
    try {
      std::unique_lock lock(io_mutex_, std::defer_lock);
      if (!lock.try_lock_for(std::chrono::milliseconds(2))) {
        // TSF fails open before HandleKey when binding cannot be confirmed.
        // Retire only the locally observed modifier, not consumed key pairs.
        CancelShiftTap();
        for (auto& held : held_shift_) held.store(false);
        return false;
      }
      if (!connected_.load() || pipe_ == INVALID_HANDLE_VALUE) {
        ScheduleReconnectLocked();
        return false;
      }
      if (context_id == context_id_) return true;
      if (!BestEffortCloseSessionLocked()) {
        FailConnectionLocked();
        return false;
      }
      input_session_id_ = 0;
      capturing_.store(false);
      {
        std::lock_guard pending(notification_mutex_);
        notifications_.clear();
      }
      composing_.store(false);
      pending_chord_.store(false);
      ascii_mode_.store(false);
      for (auto& pressed : pressed_keys_) pressed.store(false);
      for (auto& observed : observed_shift_) observed.store(false);
      for (auto& held : held_shift_) held.store(false);
      context_id_ = context_id;
      if (context_id == 0) return true;
      protocol::OpenInputSession open{context_id, {}};
      std::vector<std::byte> payload;
      protocol::Frame response;
      protocol::InputSessionOpened opened;
      if (!protocol::EncodeOpenInputSession(open, &payload) ||
          !RequestResponse(pipe_, protocol::MessageType::kOpenInputSession,
                           next_request_id_++, std::move(payload),
                           GetTickCount64() + kKeyBudgetMillis, stop_event_,
                           &response, nullptr) ||
          !IsExpectedResponse(response,
                              protocol::MessageType::kInputSessionOpened) ||
          !protocol::DecodeInputSessionOpened(response.payload, &opened)) {
        FailConnectionLocked();
        return false;
      }
      input_session_id_ = opened.session_id;
      if (opened.has_key_routing != key_routing_capable_.load()) {
        FailConnectionLocked();
        return false;
      }
      ascii_mode_.store(opened.has_key_routing && opened.ascii_mode);
      if (control_capable_.load()) {
        protocol::Frame focused;
        if (!RequestResponse(pipe_, protocol::MessageType::kControl,
                        next_request_id_++,
                        core::EncodeControl(
                            {{"op", "focus"}, {"session", input_session_id_}}),
                        GetTickCount64() + kKeyBudgetMillis, stop_event_,
                        &focused, nullptr) ||
            !IsExpectedResponse(focused, protocol::MessageType::kControlState)) {
          FailConnectionLocked();
          return false;
        }
      }
      next_sequence_id_ = 1;
      last_revision_ = 0;
      return true;
    } catch (...) {
      FailConnection();
      return false;
    }
  }

  void BeginConnect() noexcept override {
    LogDiagnosticStage(DiagnosticStage::kConnectBeginCalled);
    Disconnect();
    if (stop_event_ == nullptr) {
      LogDiagnosticStage(DiagnosticStage::kConnectStopEventUnavailable);
      return;
    }
    ResetEvent(stop_event_);
    stopping_.store(false, std::memory_order_release);
    reconnecting_.store(true);
    try {
      connect_thread_ =
          std::thread(&NamedPipeBrokerClient::ConnectWorker, this);
      control_thread_ =
          std::thread(&NamedPipeBrokerClient::ControlWorker, this);
      LogDiagnosticStage(DiagnosticStage::kConnectWorkerThreadStarted);
    } catch (...) {
      LogDiagnosticStage(DiagnosticStage::kConnectWorkerThreadStartFailed);
      stopping_.store(true, std::memory_order_release);
      SetEvent(stop_event_);
    }
  }

  void Disconnect() noexcept override {
    stopping_.store(true, std::memory_order_release);
    connected_.store(false, std::memory_order_release);
    if (stop_event_ != nullptr) {
      SetEvent(stop_event_);
    }
    if (connect_thread_.joinable()) {
      // Disconnect is owned by the activating TSF thread; the connect worker
      // never invokes it, so joining cannot self-deadlock.
      connect_thread_.join();
    }

    if (control_thread_.joinable()) control_thread_.join();
    std::lock_guard lock(io_mutex_);
    if (pipe_ != INVALID_HANDLE_VALUE) {
      BestEffortCloseSessionLocked();
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
    }
    ResetSessionLocked();
  }

  std::optional<bool> AsciiMode() const noexcept override {
    if (!connected_.load(std::memory_order_acquire) || !modifier_capable_.load()) return std::nullopt;
    return ascii_mode_.load();
  }

  BrokerKeyResult HandleKey(const BrokerKeyEvent& event,
                            BrokerInputState* state) noexcept override {
    if (state != nullptr) {
      *state = BrokerInputState{};
    }
    if (event.phase == BrokerKeyPhase::kPreservedKey ||
        event.virtual_key == 0 || event.virtual_key > 0xffU) {
      CancelShiftTap();
      return BrokerKeyResult::kPassThrough;
    }
    const bool toggle = event.phase == BrokerKeyPhase::kInputModeToggle;
    if (toggle && (event.virtual_key != VK_SHIFT || !modifier_capable_.load()))
      return BrokerKeyResult::kUnavailable;
    const bool shift = IsShiftKey(event.virtual_key);
    const std::size_t key_index = shift && event.virtual_key == VK_SHIFT
        ? (((event.key_data >> 16U) & 0xffU) == 0x36U ? VK_RSHIFT : VK_LSHIFT)
        : static_cast<std::size_t>(event.virtual_key);

    if (event.phase == BrokerKeyPhase::kTestKeyUp) {
      if (!connected_.load(std::memory_order_acquire)) {
        return BrokerKeyResult::kUnavailable;
      }
      return (pressed_keys_[key_index].load(std::memory_order_acquire) ||
              held_shift_[key_index].load(std::memory_order_acquire))
                 ? BrokerKeyResult::kConsumed
                 : BrokerKeyResult::kPassThrough;
    }

    const std::uint32_t modifiers = toggle ? 0 : ReadModifiers();
    const bool composing = composing_.load(std::memory_order_acquire) || pending_chord_.load();
    const bool observe_shift = shift && modifier_capable_.load() &&
                               !HasCommandModifiers(modifiers);
    const auto chords = buffer_chords_.load();
    const auto send_chord = static_cast<unsigned>(chords & 0xffffU);
    const auto previous_chord = static_cast<unsigned>((chords >> 16U) & 0xffffU);
    const auto next_chord = static_cast<unsigned>((chords >> 32U) & 0xffffU);
    const bool capture_key = capturing_.load() &&
        ((peripheral_input_.load() &&
          ((!HasCommandModifiers(modifiers) && (event.virtual_key == VK_UP || event.virtual_key == VK_DOWN || event.virtual_key == VK_TAB)) ||
           ((modifiers & ~static_cast<unsigned>(protocol::KeyModifiers::kCapsLock)) == static_cast<unsigned>(protocol::KeyModifiers::kControl) &&
            (event.virtual_key == 'F' || event.virtual_key == 'N')))) ||
         (!peripheral_input_.load() && core::BufferPluginCycleDirection(
             static_cast<std::uint32_t>(event.virtual_key), modifiers,
             previous_chord & 0xffU, previous_chord >> 8U,
             next_chord & 0xffU, next_chord >> 8U) != 0) ||
         core::IsBufferSendChord(static_cast<std::uint32_t>(event.virtual_key), modifiers,
                                send_chord & 0xffU, send_chord >> 8U) ||
         ((modifiers & ~static_cast<unsigned>(protocol::KeyModifiers::kCapsLock)) ==
             static_cast<unsigned>(protocol::KeyModifiers::kControl) &&
          (event.virtual_key == 'A' || event.virtual_key == 'C' || event.virtual_key == 'V')) ||
         (!HasCommandModifiers(modifiers) &&
          (event.virtual_key == VK_RETURN || event.virtual_key == VK_BACK ||
           event.virtual_key == VK_LEFT || event.virtual_key == VK_RIGHT ||
           event.virtual_key == VK_HOME || event.virtual_key == VK_END || event.virtual_key == VK_DELETE ||
           event.virtual_key == VK_ESCAPE)));
    if (event.phase == BrokerKeyPhase::kTestKeyDown) {
      // A filtered host shortcut may never receive OnKeyDown. This is local
      // gesture bookkeeping only: test callbacks never send input to Rime.
      if (!shift || HasCommandModifiers(modifiers)) CancelShiftTap();
      if (!connected_.load(std::memory_order_acquire)) {
        return BrokerKeyResult::kUnavailable;
      }
      return (observe_shift || ShouldOfferKey(event.virtual_key, modifiers, composing,
                                             capturing_.load(), ascii_mode_.load()) || capture_key)
                 ? BrokerKeyResult::kConsumed
                 : BrokerKeyResult::kPassThrough;
    }

    const bool key_down = event.phase == BrokerKeyPhase::kKeyDown;
    const bool key_up = event.phase == BrokerKeyPhase::kKeyUp || toggle;
    if (key_down && (!shift || HasCommandModifiers(modifiers))) CancelShiftTap();
    if ((!key_down && !key_up) ||
        (key_down && !observe_shift &&
         !ShouldOfferKey(event.virtual_key, modifiers, composing,
                         capturing_.load(), ascii_mode_.load()) && !capture_key) ||
        (key_up && !toggle && !pressed_keys_[key_index].load(std::memory_order_acquire) &&
         !held_shift_[key_index].load(std::memory_order_acquire))) {
      return BrokerKeyResult::kPassThrough;
    }

    try {
      std::unique_lock lock(io_mutex_, std::defer_lock);
      if (!lock.try_lock_for(std::chrono::milliseconds(2))) {
        // This physical event has already failed open. Retire its gesture;
        // a repeated release must never become a later synthetic tap.
        CancelShiftTap();
        if (shift && key_up && !toggle) held_shift_[key_index].store(false);
        return BrokerKeyResult::kUnavailable;
      }
      if (toggle) CancelShiftTap();
      const bool owned_up = key_up && !toggle && pressed_keys_[key_index].exchange(
          false, std::memory_order_acq_rel);
      const bool observed_up = key_up && !toggle && observed_shift_[key_index].exchange(
          false, std::memory_order_acq_rel);
      const bool held_up = key_up && !toggle && held_shift_[key_index].exchange(false);
      if (key_up && !toggle && !owned_up && !held_up) {
        return BrokerKeyResult::kPassThrough;
      }
      if (!connected_.load(std::memory_order_acquire) ||
          pipe_ == INVALID_HANDLE_VALUE || input_session_id_ == 0) {
        return BrokerKeyResult::kUnavailable;
      }
      if (shift && key_down) {
        const bool another_shift = held_shift_[VK_LSHIFT].load() ||
                                   held_shift_[VK_RSHIFT].load();
        if (another_shift) CancelShiftTap();
        const bool repeat = (static_cast<std::uint64_t>(event.key_data) & (1ULL << 30)) != 0 ||
                            (event.key_data & 0xffffU) > 1;
        held_shift_[key_index].store(true);
        observed_shift_[key_index].store(observe_shift && !another_shift && !repeat);
        shift_down_at_[key_index] = GetTickCount64();
        // Do not arm ascii_composer until a real, uninterrupted release is
        // known. Physical Shift remains with the host throughout the gesture.
        return BrokerKeyResult::kPassThrough;
      }
      if (shift && !toggle && (!observed_up || HasCommandModifiers(modifiers) ||
                    !modifier_capable_.load() ||
                    GetTickCount64() - shift_down_at_[key_index] >= 500))
        return BrokerKeyResult::kPassThrough;
      if (next_request_id_ == 0 ||
          next_request_id_ == std::numeric_limits<std::uint32_t>::max() ||
          next_sequence_id_ == 0 ||
          next_sequence_id_ == std::numeric_limits<std::uint64_t>::max()) {
        // Never wrap an identity that the Broker treats as strictly
        // monotonic. Reconnect to obtain a fresh input session instead.
        FailConnectionLocked();
        return BrokerKeyResult::kUnavailable;
      }

      const std::uint32_t request_id = next_request_id_++;
      const std::uint64_t sequence_id = next_sequence_id_++;
      protocol::KeyEvent key;
      key.session_id = input_session_id_;
      key.sequence_id = sequence_id;
      key.timestamp_millis = static_cast<DWORD>(GetMessageTime());
      key.virtual_key = static_cast<std::uint32_t>(event.virtual_key);
      key.scan_code =
          static_cast<std::uint32_t>((event.key_data >> 16U) & 0xffU);
      key.repeat_count =
          key_down
              ? std::max<std::uint32_t>(
                    1, static_cast<std::uint32_t>(event.key_data & 0xffffU))
              : 1;
      key.modifiers = modifiers;
      if (shift) key.event_flags |= static_cast<std::uint32_t>(protocol::KeyEventFlags::kShiftTap);
      if (key_down) {
        key.event_flags |=
            static_cast<std::uint32_t>(protocol::KeyEventFlags::kKeyDown);
      }
      if (key_down && key.repeat_count > 1) {
        key.event_flags |=
            static_cast<std::uint32_t>(protocol::KeyEventFlags::kRepeat);
      }
      if ((event.key_data & (static_cast<LPARAM>(1) << 24U)) != 0) {
        key.event_flags |=
            static_cast<std::uint32_t>(protocol::KeyEventFlags::kExtended);
      }
      if ((event.key_data & (static_cast<LPARAM>(1) << 29U)) != 0) {
        key.event_flags |=
            static_cast<std::uint32_t>(protocol::KeyEventFlags::kSystemKey);
      }

      std::vector<std::byte> payload;
      protocol::Frame response;
      if (!protocol::EncodeKeyEvent(key, &payload)) {
        LogDiagnosticStage(DiagnosticStage::kKeyRequestEncodeFailed);
        FailConnectionLocked();
        return BrokerKeyResult::kUnavailable;
      }
      RequestFailure request_failure = RequestFailure::kNone;
      if (!RequestResponse(pipe_, protocol::MessageType::kKeyEvent, request_id,
                           std::move(payload),
                           GetTickCount64() + kKeyBudgetMillis, stop_event_,
                           &response, &request_failure)) {
        const DiagnosticStage diagnostic_stage =
            request_failure == RequestFailure::kWriteTimedOut
                ? DiagnosticStage::kKeyRequestWriteTimedOut
            : request_failure == RequestFailure::kReadTimedOut
                ? DiagnosticStage::kKeyRequestReadTimedOut
                : DiagnosticStage::kKeyRequestExchangeFailed;
        LogDiagnosticStage(diagnostic_stage);
        FailConnectionLocked();
        return BrokerKeyResult::kUnavailable;
      }
      if (!IsExpectedResponse(response, protocol::MessageType::kInputState)) {
        LogDiagnosticStage(DiagnosticStage::kKeyResponseEnvelopeInvalid);
        FailConnectionLocked();
        return BrokerKeyResult::kUnavailable;
      }

      protocol::InputState decoded;
      if (!protocol::DecodeInputState(response.payload, &decoded) ||
          decoded.session_id != input_session_id_ ||
          decoded.sequence_id != sequence_id ||
          decoded.revision < last_revision_) {
        LogDiagnosticStage(DiagnosticStage::kKeyResponseDecodeFailed);
        FailConnectionLocked();
        return BrokerKeyResult::kUnavailable;
      }
      last_revision_ = decoded.revision;
      const bool ascii_mode = (decoded.state_flags & static_cast<std::uint32_t>(
          protocol::InputStateFlags::kAsciiMode)) != 0;
      if (ascii_mode && !key_routing_capable_.load()) {
        FailConnectionLocked();
        return BrokerKeyResult::kUnavailable;
      }
      if (key_routing_capable_.load()) ascii_mode_.store(ascii_mode);
      const bool pending_chord = (decoded.state_flags & static_cast<std::uint32_t>(protocol::InputStateFlags::kPendingChord)) != 0;
      if (pending_chord && !pending_chord_capable_.load()) {
        FailConnectionLocked(); return BrokerKeyResult::kUnavailable;
      }
      pending_chord_.store(pending_chord);
      const bool handled =
          (decoded.state_flags & static_cast<std::uint32_t>(
                                     protocol::InputStateFlags::kHandled)) != 0;
      const bool modifier_snapshot = (decoded.state_flags &
          static_cast<std::uint32_t>(protocol::InputStateFlags::kModifierSnapshot)) != 0;
      if (modifier_snapshot && (!modifier_capable_.load() || !shift)) {
        FailConnectionLocked();
        return BrokerKeyResult::kUnavailable;
      }
      const bool is_composing =
          (decoded.state_flags &
           static_cast<std::uint32_t>(protocol::InputStateFlags::kComposing)) !=
          0;
      if (!handled && !modifier_snapshot) {
        LogDiagnosticStage(DiagnosticStage::kKeyResponseUnhandled);
        // An unhandled response is explicitly "no mutation" in the wire
        // contract, not an empty authoritative snapshot.  In particular,
        // chord_composer commonly leaves a KeyUp unhandled while the
        // composition created by its matching KeyDown is still active.  Keep
        // the last known state so the next Space/Return remains eligible for
        // the Broker instead of leaking into the host application.
        // A real KeyUp reaches the Broker only when its matching KeyDown was
        // consumed.  Keep the pair together even if librime reports that the
        // release itself caused no additional mutation; exposing an orphan
        // KeyUp to the host breaks chord_composer and some app key state.
        // Leave `state` as the empty default (has_snapshot=false) so TSF does
        // not treat this eat-without-mutation as "cancel the composition".
        return owned_up ? BrokerKeyResult::kConsumed
                      : BrokerKeyResult::kPassThrough;
      }
      composing_.store(is_composing, std::memory_order_release);
      if (key_down && handled) {
        pressed_keys_[key_index].store(true, std::memory_order_release);
      }

      if (state != nullptr) {
        state->has_snapshot = true;
        state->buffer_capture =
            (decoded.state_flags &
             static_cast<unsigned>(
                 protocol::InputStateFlags::kBufferCapture)) != 0;
        capturing_.store(state->buffer_capture);
        state->pending_chord = pending_chord;
        state->composing = is_composing;
        state->revision = decoded.revision;
        state->caret_utf16 = decoded.caret_utf16;
        state->selection_length_utf16 = decoded.selection_length_utf16;
        if (!Utf8ToWide(decoded.composition, &state->composition) ||
            !Utf8ToWide(decoded.commit_text, &state->commit_text)) {
          LogDiagnosticStage(DiagnosticStage::kKeyResponseDecodeFailed);
          FailConnectionLocked();
          return BrokerKeyResult::kUnavailable;
        }
        state->candidates_visible =
            (decoded.state_flags &
             static_cast<std::uint32_t>(
                 protocol::InputStateFlags::kCandidatesVisible)) != 0;
        state->highlighted_candidate = decoded.highlighted_candidate;
        state->page_start = decoded.page_start;
        state->page_size = decoded.page_size;
        state->last_page = (decoded.state_flags & static_cast<std::uint32_t>(
            protocol::InputStateFlags::kLastCandidatePage)) != 0;
        state->candidate_page_keys = decoded.state_flags & protocol::kCandidatePageKeyFlags;
        state->candidates.clear();
        try {
          state->candidates.reserve(decoded.candidates.size());
          for (const protocol::Candidate& candidate : decoded.candidates) {
            BrokerCandidate item;
            item.id = candidate.id;
            if (!Utf8ToWide(candidate.text, &item.text) ||
                !Utf8ToWide(candidate.comment, &item.comment) ||
                !Utf8ToWide(candidate.label, &item.label)) {
              LogDiagnosticStage(DiagnosticStage::kKeyResponseDecodeFailed);
              FailConnectionLocked();
              return BrokerKeyResult::kUnavailable;
            }
            state->candidates.push_back(std::move(item));
          }
        } catch (...) {
          FailConnectionLocked();
          return BrokerKeyResult::kUnavailable;
        }
        if (state->commit_text.empty()) {
          LogDiagnosticStage(DiagnosticStage::kKeyResponseHandledCommitEmpty);
        } else if (state->commit_text.size() == 1) {
          LogDiagnosticStage(
              DiagnosticStage::kKeyResponseHandledCommitOneUtf16);
        } else {
          LogDiagnosticStage(
              DiagnosticStage::kKeyResponseHandledCommitMultipleUtf16);
        }
      } else {
        LogDiagnosticStage(
            decoded.commit_text.empty()
                ? DiagnosticStage::kKeyResponseHandledCommitEmpty
                : DiagnosticStage::kKeyResponseHandledCommitNonempty);
      }
      return handled || owned_up ? BrokerKeyResult::kConsumed
                                 : BrokerKeyResult::kPassThrough;
    } catch (...) {
      FailConnection();
      return BrokerKeyResult::kUnavailable;
    }
  }

 private:
  void CancelShiftTap() noexcept {
    for (auto& observed : observed_shift_) observed.store(false);
  }

  void ConnectWorker() noexcept {
    struct Finished {
      std::atomic_bool& value;
      ~Finished() { value.store(false); }
    } finished{reconnecting_};
    LogDiagnosticStage(DiagnosticStage::kConnectWorkerEntered);
    try {
      std::wstring endpoint;
      TokenIdentity current_identity;
      DWORD current_session_id = 0;
      if (!BuildEndpoint(&endpoint, &current_identity, &current_session_id)) {
        LogDiagnosticStage(DiagnosticStage::kConnectEndpointBuildFailed);
        return;
      }
      bool sandboxed = false;
      if (!ReadAppContainer(&sandboxed)) return;
      if (sandboxed) endpoint += L".input";
      if (stopping_.load(std::memory_order_acquire)) {
        LogDiagnosticStage(DiagnosticStage::kConnectCancelledAfterEndpoint);
        return;
      }

      const DWORD flags = FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                          SECURITY_IDENTIFICATION;
      bool launched_broker = false;
      ULONGLONG deadline = GetTickCount64() + kConnectBudgetMillis;
      HANDLE pipe = INVALID_HANDLE_VALUE;
      while (!stopping_.load(std::memory_order_acquire) &&
             RemainingWait(deadline) > 0) {
        const DWORD access = sandboxed
            ? FILE_READ_DATA | FILE_WRITE_DATA | READ_CONTROL | SYNCHRONIZE
            : GENERIC_READ | GENERIC_WRITE;
        pipe = CreateFileW(endpoint.c_str(), access, 0,
                           nullptr, OPEN_EXISTING, flags, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
          break;
        }
        const DWORD open_error = GetLastError();
        const DWORD remaining = RemainingWait(deadline);
        if (remaining == 0) {
          LogDiagnosticStage(DiagnosticStage::kConnectPipeDeadlineExpired);
          return;
        }
        if (open_error == ERROR_PIPE_BUSY) {
          WaitNamedPipeW(endpoint.c_str(),
                         (std::min)(remaining, kBusyPipeWaitMillis));
          continue;
        }
        if (open_error == ERROR_FILE_NOT_FOUND) {
          if (!sandboxed && !launched_broker) {
            launched_broker = LaunchBrokerProcess();
            if (launched_broker) {
              deadline = GetTickCount64() + kLaunchConnectBudgetMillis;
            }
          }
          if (WaitForSingleObject(stop_event_,
                                  (std::min)(remaining, kConnectRetryMillis)) ==
              WAIT_OBJECT_0) {
            LogDiagnosticStage(DiagnosticStage::kConnectCancelledWhileOpening);
            return;
          }
          continue;
        }
        if (open_error != ERROR_FILE_NOT_FOUND) {
          LogDiagnosticStage(open_error == ERROR_ACCESS_DENIED
                                 ? DiagnosticStage::kConnectPipeAccessDenied
                                 : DiagnosticStage::kConnectPipeOpenFailed);
          return;
        }
      }
      if (pipe == INVALID_HANDLE_VALUE) {
        LogDiagnosticStage(stopping_.load(std::memory_order_acquire)
                               ? DiagnosticStage::kConnectCancelledWhileOpening
                               : DiagnosticStage::kConnectPipeDeadlineExpired);
        return;
      }
      UniqueHandle candidate_pipe(pipe);

      DWORD server_process_id = 0;
      ServerIdentityFailure identity_failure = ServerIdentityFailure::kNone;
      const bool verified = sandboxed
          ? VerifyInputPipe(candidate_pipe.get(), current_identity.sid.data(),
                            current_session_id, &server_process_id)
          : VerifyServerIdentity(candidate_pipe.get(), current_session_id,
                                 current_identity, &server_process_id,
                                 &identity_failure);
      if (!verified) {
        DiagnosticStage diagnostic_stage =
            DiagnosticStage::kConnectServerPidQueryFailed;
        switch (identity_failure) {
          case ServerIdentityFailure::kSessionQueryFailed:
            diagnostic_stage =
                DiagnosticStage::kConnectServerSessionQueryFailed;
            break;
          case ServerIdentityFailure::kSessionMismatch:
            diagnostic_stage = DiagnosticStage::kConnectServerSessionMismatch;
            break;
          case ServerIdentityFailure::kProcessOpenFailed:
            diagnostic_stage = DiagnosticStage::kConnectServerProcessOpenFailed;
            break;
          case ServerIdentityFailure::kTokenReadFailed:
            diagnostic_stage = DiagnosticStage::kConnectServerTokenReadFailed;
            break;
          case ServerIdentityFailure::kSidMismatch:
            diagnostic_stage = DiagnosticStage::kConnectServerSidMismatch;
            break;
          case ServerIdentityFailure::kNone:
          case ServerIdentityFailure::kProcessIdQueryFailed:
            break;
        }
        LogDiagnosticStage(diagnostic_stage);
        return;
      }
      if (stopping_.load(std::memory_order_acquire)) {
        LogDiagnosticStage(DiagnosticStage::kConnectCancelledAfterIdentity);
        return;
      }

      std::uint32_t request_id = 1;
      protocol::ClientHello hello;
      hello.process_id = GetCurrentProcessId();
      hello.session_id = current_session_id;
      hello.client_name = "RimesTsf";
      hello.capabilities = protocol::kModifierSnapshotsCapability |
                           protocol::kKeyRoutingCapability | protocol::kPendingChordCapability |
                           protocol::kCandidatePagingCapability;
      std::vector<std::byte> payload;
      protocol::Frame response;
      if (!protocol::EncodeClientHello(hello, &payload)) {
        LogDiagnosticStage(DiagnosticStage::kConnectHelloEncodeFailed);
        return;
      }
      RequestFailure request_failure = RequestFailure::kNone;
      if (!RequestResponse(candidate_pipe.get(),
                           protocol::MessageType::kClientHello, request_id++,
                           std::move(payload), deadline, stop_event_, &response,
                           &request_failure)) {
        const DiagnosticStage diagnostic_stage =
            request_failure == RequestFailure::kWriteTimedOut
                ? DiagnosticStage::kConnectHelloWriteTimedOut
            : request_failure == RequestFailure::kReadTimedOut
                ? DiagnosticStage::kConnectHelloReadTimedOut
                : DiagnosticStage::kConnectHelloExchangeFailed;
        LogDiagnosticStage(diagnostic_stage);
        return;
      }
      if (!IsExpectedResponse(response, protocol::MessageType::kBrokerHello)) {
        LogDiagnosticStage(DiagnosticStage::kConnectHelloEnvelopeInvalid);
        return;
      }
      protocol::BrokerHello broker_hello;
      if (!protocol::DecodeBrokerHello(response.payload, &broker_hello)) {
        LogDiagnosticStage(DiagnosticStage::kConnectHelloDecodeFailed);
        return;
      }
      if (broker_hello.process_id != server_process_id ||
          broker_hello.session_id != current_session_id) {
        LogDiagnosticStage(DiagnosticStage::kConnectHelloIdentityMismatch);
        return;
      }

      control_capable_.store((broker_hello.capabilities & 1) != 0);
      modifier_capable_.store((broker_hello.capabilities &
                              protocol::kModifierSnapshotsCapability) != 0);
      key_routing_capable_.store((broker_hello.capabilities &
                                 protocol::kKeyRoutingCapability) != 0);
      pending_chord_capable_.store((broker_hello.capabilities & protocol::kPendingChordCapability) != 0);
      protocol::OpenInputSession open;
      open.context_id =
          (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32U) ^
          GetTickCount64();
      if (open.context_id == 0) {
        open.context_id = 1;
      }
      payload.clear();
      if (!protocol::EncodeOpenInputSession(open, &payload)) {
        LogDiagnosticStage(DiagnosticStage::kConnectOpenEncodeFailed);
        return;
      }
      request_failure = RequestFailure::kNone;
      if (!RequestResponse(candidate_pipe.get(),
                           protocol::MessageType::kOpenInputSession,
                           request_id++, std::move(payload), deadline,
                           stop_event_, &response, &request_failure)) {
        const DiagnosticStage diagnostic_stage =
            request_failure == RequestFailure::kWriteTimedOut
                ? DiagnosticStage::kConnectOpenWriteTimedOut
            : request_failure == RequestFailure::kReadTimedOut
                ? DiagnosticStage::kConnectOpenReadTimedOut
                : DiagnosticStage::kConnectOpenExchangeFailed;
        LogDiagnosticStage(diagnostic_stage);
        return;
      }
      if (!IsExpectedResponse(response,
                              protocol::MessageType::kInputSessionOpened)) {
        LogDiagnosticStage(DiagnosticStage::kConnectOpenEnvelopeInvalid);
        return;
      }
      protocol::InputSessionOpened opened;
      if (!protocol::DecodeInputSessionOpened(response.payload, &opened)) {
        LogDiagnosticStage(DiagnosticStage::kConnectOpenDecodeFailed);
        return;
      }
      if (opened.has_key_routing != key_routing_capable_.load()) return;
      if (stopping_.load(std::memory_order_acquire)) {
        LogDiagnosticStage(DiagnosticStage::kConnectCancelledBeforePublish);
        return;
      }

      std::unique_lock lock(io_mutex_);
      if (stopping_.load(std::memory_order_acquire)) {
        LogDiagnosticStage(DiagnosticStage::kConnectCancelledBeforePublish);
        return;
      }
      if (pipe_ != INVALID_HANDLE_VALUE) {
        LogDiagnosticStage(DiagnosticStage::kConnectPublishConflict);
        return;
      }
      pipe_ = candidate_pipe.release();
      input_session_id_ = opened.session_id;
      ascii_mode_.store(opened.has_key_routing && opened.ascii_mode);
      next_request_id_ = request_id;
      next_sequence_id_ = 1;
      last_revision_ = 0;
      composing_.store(false, std::memory_order_release);
      connected_.store(true, std::memory_order_release);
      reconnecting_.store(false, std::memory_order_release);
      LogDiagnosticStage(DiagnosticStage::kConnectSucceeded);
      lock.unlock();
      // Publish focus on the TSF thread even before the first key. Buffer can
      // then bind a freshly activated input field without typing into it first.
      if (const auto window = notification_window_.load())
        PostMessageW(window, kBrokerConnected, 0, 0);
    } catch (...) {
      LogDiagnosticStage(DiagnosticStage::kConnectWorkerException);
      connected_.store(false, std::memory_order_release);
    }
    reconnecting_.store(false, std::memory_order_release);
  }

  void ControlWorker() noexcept {
    try {
      UniqueHandle channel;
      std::uint32_t request = 1;
      while (!stopping_.load()) {
        std::uint64_t session = 0;
        const auto generation = connection_generation_.load();
        {
          std::lock_guard lock(io_mutex_);
          if (connected_.load() && control_capable_.load())
            session = input_session_id_;
          else if (!connected_.load())
            ScheduleReconnectLocked();
        }
        if (!session) {
          WaitForSingleObject(stop_event_, 50);
          continue;
        }
        if (!channel) {
          std::wstring endpoint;
          TokenIdentity identity;
          DWORD logon = 0;
          if (!BuildEndpoint(&endpoint, &identity, &logon)) return;
          HANDLE raw =
              CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                              SECURITY_IDENTIFICATION,
                          nullptr);
          if (raw == INVALID_HANDLE_VALUE) {
            WaitForSingleObject(stop_event_, 100);
            continue;
          }
          channel.reset(raw);
          DWORD server = 0;
          ServerIdentityFailure stage = ServerIdentityFailure::kNone;
          if (!VerifyServerIdentity(channel.get(), logon, identity, &server,
                                    &stage)) {
            channel.reset();
            return;
          }
          protocol::ClientHello hello;
          hello.process_id = GetCurrentProcessId();
          hello.session_id = logon;
          hello.client_name = "RIMES.TSF.Control";
          std::vector<std::byte> payload;
          protocol::EncodeClientHello(hello, &payload);
          protocol::Frame response;
          request = 1;
          if (!RequestResponse(channel.get(),
                               protocol::MessageType::kClientHello, request++,
                               std::move(payload), GetTickCount64() + 2000,
                               stop_event_, &response, nullptr) ||
              !IsExpectedResponse(response,
                                  protocol::MessageType::kBrokerHello)) {
            channel.reset();
            continue;
          }
        }
        protocol::Frame response;
        if (!RequestResponse(
                channel.get(), protocol::MessageType::kControl, request++,
                core::EncodeControl({{"op", "wait"}, {"session", session}}),
                GetTickCount64() + 12000, stop_event_, &response, nullptr) ||
            !IsExpectedResponse(response,
                                protocol::MessageType::kControlState)) {
          channel.reset();
          // Drop the key connection too when the broker disappears. This
          // schedules reconnect without waiting for the user's first letter.
          std::lock_guard lock(io_mutex_);
          if (connection_generation_.load() == generation)
            FailConnectionLocked();
          continue;
        }
        auto message = core::DecodeControl(response.payload);
        if (!message) {
          channel.reset();
          return;
        }
        auto kind = message->value("kind", "");
        {
          std::lock_guard state_lock(io_mutex_);
          if (input_session_id_ != session ||
              connection_generation_.load() != generation) continue;
          if (kind == "capture") {
            const auto key = message->value("sendKey", unsigned(VK_RETURN));
            const auto modifiers = message->value("sendModifiers", unsigned(MOD_ALT | MOD_SHIFT));
            const auto previous_key = message->value("previousPluginKey", unsigned(VK_UP));
            const auto previous_modifiers = message->value("previousPluginModifiers", unsigned(MOD_ALT | MOD_SHIFT));
            const auto next_key = message->value("nextPluginKey", unsigned(VK_DOWN));
            const auto next_modifiers = message->value("nextPluginModifiers", unsigned(MOD_ALT | MOD_SHIFT));
            if (!core::ValidShortcut(key, modifiers, true) ||
                !core::ValidShortcut(previous_key, previous_modifiers) ||
                !core::ValidShortcut(next_key, next_modifiers) ||
                (previous_key == next_key && previous_modifiers == next_modifiers) ||
                (key == previous_key && modifiers == previous_modifiers) ||
                (key == next_key && modifiers == next_modifiers)) {
              capturing_.store(false);
              continue;
            }
            // A single atomic snapshot avoids torn key/modifier reads on the
            // host input thread while settings are being changed.
            buffer_chords_.store(std::uint64_t(key | (modifiers << 8U)) |
                (std::uint64_t(previous_key | (previous_modifiers << 8U)) << 16U) |
                (std::uint64_t(next_key | (next_modifiers << 8U)) << 32U));
            capturing_.store(message->value("enabled", false));
            peripheral_input_.store(message->value("enabled", false) && message->value("inputRoute", 1U) >= 2);
          }
          if (kind == "capture" || kind == "deliver" || kind == "buffer_caret" || kind == "buffer_transition") {
            (*message)["connectionGeneration"] = generation;
            std::lock_guard lock(notification_mutex_);
            if (notifications_.size() >= 128) return;
            notifications_.push_back(std::move(*message));
          }
        }
        if (kind == "capture" || kind == "deliver" || kind == "buffer_caret" || kind == "buffer_transition") {
          const auto window = notification_window_.load();
          if (window) PostMessageW(window, kBrokerNotification, 0, 0);
        } else if (kind == "disconnected")
          WaitForSingleObject(stop_event_, 100);
      }
    } catch (...) {
      capturing_.store(false);
    }
  }

  bool BestEffortCloseSessionLocked() noexcept {
    if (input_session_id_ == 0) return true;
    if (next_request_id_ == 0 ||
        next_request_id_ == std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    try {
      protocol::CloseInputSession close{input_session_id_};
      std::vector<std::byte> payload;
      protocol::Frame response;
      if (!protocol::EncodeCloseInputSession(close, &payload)) {
        return false;
      }
      return RequestResponse(pipe_, protocol::MessageType::kCloseInputSession,
                      next_request_id_++, std::move(payload),
                      GetTickCount64() + kCloseBudgetMillis, nullptr, &response,
                      nullptr) &&
             IsExpectedResponse(response, protocol::MessageType::kInputSessionClosed);
    } catch (...) {
      return false;
    }
  }

  void ResetSessionLocked() noexcept {
    ++connection_generation_;
    {
      std::lock_guard pending(notification_mutex_);
      notifications_.clear();
    }
    context_id_ = 0;
    capturing_.store(false);
    input_session_id_ = 0;
    next_request_id_ = 1;
    next_sequence_id_ = 1;
    last_revision_ = 0;
    composing_.store(false, std::memory_order_release);
    pending_chord_.store(false);
    pending_chord_capable_.store(false);
    connected_.store(false, std::memory_order_release);
    modifier_capable_.store(false);
    key_routing_capable_.store(false);
    ascii_mode_.store(false);
    for (auto& pressed : pressed_keys_) {
      pressed.store(false, std::memory_order_release);
    }
    for (auto& observed : observed_shift_) observed.store(false);
    for (auto& held : held_shift_) held.store(false);
    if (const auto window = notification_window_.load())
      PostMessageW(window, kBrokerNotification, 0, 0);
  }

  void ScheduleReconnectLocked() noexcept {
    if (stopping_.load(std::memory_order_acquire) ||
        reconnecting_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      if (connect_thread_.joinable()) {
        connect_thread_.join();
      }
      if (stop_event_ != nullptr) {
        ResetEvent(stop_event_);
      }
      connect_thread_ =
          std::thread(&NamedPipeBrokerClient::ConnectWorker, this);
    } catch (...) {
      reconnecting_.store(false, std::memory_order_release);
    }
  }

  void FailConnectionLocked() noexcept {
    if (pipe_ != INVALID_HANDLE_VALUE) {
      CancelIoEx(pipe_, nullptr);
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
    }
    ResetSessionLocked();
    ScheduleReconnectLocked();
  }

  void FailConnection() noexcept {
    std::lock_guard lock(io_mutex_);
    FailConnectionLocked();
  }

  std::atomic<HWND> notification_window_{nullptr};
  std::atomic_bool capturing_{false}, control_capable_{false};
  std::atomic_bool peripheral_input_{false};
  std::atomic_uint64_t buffer_chords_{
      std::uint64_t(VK_RETURN | ((MOD_ALT | MOD_SHIFT) << 8U)) |
      (std::uint64_t(VK_UP | ((MOD_ALT | MOD_SHIFT) << 8U)) << 16U) |
      (std::uint64_t(VK_DOWN | ((MOD_ALT | MOD_SHIFT) << 8U)) << 32U)};
  std::atomic_bool modifier_capable_{false};
  std::atomic_bool pending_chord_capable_{false}, pending_chord_{false};
  std::atomic_bool key_routing_capable_{false}, ascii_mode_{false};
  std::atomic<std::uint64_t> connection_generation_{0};
  std::mutex notification_mutex_;
  std::deque<core::Json> notifications_;
  std::thread control_thread_;
  // The control worker holds this only briefly while reading/updating state.
  // A zero-wait try_lock on the input thread can drop a healthy first letter
  // during that snapshot; allow at most 2 ms, never an unbounded mutex wait.
  std::timed_mutex io_mutex_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE stop_event_ = nullptr;
  std::thread connect_thread_;
  std::atomic_bool stopping_{true};
  std::atomic_bool connected_{false};
  std::atomic_bool reconnecting_{false};
  std::atomic_bool composing_{false};
  std::array<std::atomic_bool, 256> pressed_keys_{};
  std::array<std::atomic_bool, 256> observed_shift_{};
  std::array<std::atomic_bool, 256> held_shift_{};
  // Written/read only by the owning TSF thread; worker resets only the atomic
  // eligibility flags, so a disconnect can retire but never revive a tap.
  std::array<ULONGLONG, 256> shift_down_at_{};
  std::uint64_t input_session_id_ = 0;
  std::uint32_t next_request_id_ = 1;
  std::uint64_t next_sequence_id_ = 1;
  std::uint64_t last_revision_ = 0;
  std::uint64_t context_id_ = 0;
};

}  // namespace

std::unique_ptr<BrokerClient> CreateBrokerClient() noexcept {
  return std::unique_ptr<BrokerClient>(new (std::nothrow)
                                           NamedPipeBrokerClient());
}

}  // namespace rimes::windows::tsf
