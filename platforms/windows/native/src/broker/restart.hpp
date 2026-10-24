#pragma once
#include "win32_security.hpp"
#include <memory>

namespace rimes::windows::broker {
// A new Broker waits for a live, verified predecessor before acquiring the
// engine lifetime lock. A predecessor may finish before this child starts;
// that needs no wait. No application process is terminated or DLL overwritten.
inline bool WaitForRestartParent(DWORD pid) {
  if (!pid) return true;
  if (pid == GetCurrentProcessId()) return false;
  struct Close { void operator()(void* p) const { if (p && p != INVALID_HANDLE_VALUE) CloseHandle(p); } };
  using Handle = std::unique_ptr<void, Close>;
  Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
  if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
  const auto exited = [&] { return WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0; };
  if (exited()) return true;
  wchar_t own[32768]{}, parent[32768]{}; DWORD length = 32768;
  const auto own_length = GetModuleFileNameW(nullptr, own, 32768);
  if (!own_length || own_length >= 32768 ||
      !QueryFullProcessImageNameW(process.get(), 0, parent, &length)) return exited();
  // GetModuleFileName can retain a junction or hard-link alias while the
  // process query reports another path. Compare the underlying file, not its
  // spelling; a byte-for-byte copy elsewhere is still a different image.
  const auto open_image = [](const wchar_t* path) {
    return CreateFileW(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, 0, nullptr);
  };
  Handle own_image(open_image(own)), parent_image(open_image(parent));
  BY_HANDLE_FILE_INFORMATION own_info{}, parent_info{};
  if (own_image.get() == INVALID_HANDLE_VALUE || parent_image.get() == INVALID_HANDLE_VALUE ||
      !GetFileInformationByHandle(own_image.get(), &own_info) ||
      !GetFileInformationByHandle(parent_image.get(), &parent_info) ||
      own_info.dwVolumeSerialNumber != parent_info.dwVolumeSerialNumber ||
      own_info.nFileIndexHigh != parent_info.nFileIndexHigh ||
      own_info.nFileIndexLow != parent_info.nFileIndexLow) return exited();
  UserSecurityContext security; std::wstring error;
  DWORD session = 0;
  if (!security.Initialize(&error) || !ProcessIdToSessionId(pid, &session) ||
      session != security.session_id()) return exited();
  HANDLE raw = nullptr;
  if (!OpenProcessToken(process.get(), TOKEN_QUERY, &raw)) return exited();
  Handle token(raw); DWORD size = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  if (!size || size > 65536) return exited();
  std::vector<std::byte> buffer(size);
  if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) ||
      !EqualSid(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, security.sid())) return exited();
  // The verified predecessor can be finishing an uncancellable dictionary
  // transfer. This child owns no engine or lifetime gate yet; let that work
  // finish instead of timing out and leaving the completed restart without a
  // successor. The retained process handle also prevents PID-reuse confusion.
  return WaitForSingleObject(process.get(), INFINITE) == WAIT_OBJECT_0;
}
}
