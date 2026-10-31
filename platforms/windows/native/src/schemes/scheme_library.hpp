#pragma once
#include "scheme_import.hpp"
#include "../core/product_brand.hpp"
#include "../workbench/provider.hpp"
#include <Windows.h>
#include <stdexcept>

namespace rimes::windows::schemes {
inline rimes::schemes::Library Library() {
  wchar_t root[32768]{};
  const DWORD size = GetEnvironmentVariableW(L"APPDATA", root, 32768);
  if (!size || size >= 32768)
    throw std::runtime_error("APPDATA is unavailable");
  return rimes::schemes::Library(std::filesystem::path(root) / L"RIMES" /
                                 L"schemes");
}
// Shared during engine use, exclusive during removal. The lock lives beside
// learning data, which survives removal, so checking it cannot delete a live
// map.
class UsageLock {
public:
  UsageLock(const std::filesystem::path &learning, bool exclusive) {
    std::filesystem::create_directories(learning);
    handle_ = CreateFileW((learning / L".rimes-scheme-use").c_str(),
                          GENERIC_READ | GENERIC_WRITE,
                          exclusive ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
      throw std::runtime_error(workbench::Utf8(rimes::branding::Text(
          L"方案仍在使用或正在维护，请先退出 {brand} 后重试。")));
  }
  ~UsageLock() { CloseHandle(handle_); }
  UsageLock(const UsageLock &) = delete;
  UsageLock &operator=(const UsageLock &) = delete;

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};
// Maintenance modes are handled before a Broker mutex, UI or TSF session
// exists.
int RunManager(int argc, wchar_t **argv);
} // namespace rimes::windows::schemes
