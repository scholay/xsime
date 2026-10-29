#pragma once
#include <windows.h>
#include <atomic>
#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace rimes::windows::update_storage {
struct CloseHandleValue { void operator()(void* value) const { if (value) CloseHandle(value); } };
using Handle = std::unique_ptr<void, CloseHandleValue>;
inline Handle Owned(HANDLE value) { return Handle(value == INVALID_HANDLE_VALUE ? nullptr : value); }
inline void CheckCancelled(const std::atomic<bool>& cancelled) {
    if (cancelled.load()) throw std::runtime_error("Update cancelled");
}

// Sharing only reads pins the verified inode and denies both mutation and
// replacement until CreateProcess has opened the executable image.
class LockedFile {
public:
    explicit LockedFile(const std::filesystem::path& path)
        : handle_(Owned(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr))) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!handle_ || !GetFileInformationByHandle(handle_.get(), &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            info.nNumberOfLinks != 1 || GetFileType(handle_.get()) != FILE_TYPE_DISK)
            throw std::runtime_error("Unsafe update file");
    }
    std::string Read(std::size_t limit) const {
        LARGE_INTEGER size{}, zero{};
        if (!GetFileSizeEx(handle_.get(), &size) || size.QuadPart < 0 ||
            static_cast<unsigned long long>(size.QuadPart) > limit ||
            !SetFilePointerEx(handle_.get(), zero, nullptr, FILE_BEGIN))
            throw std::runtime_error("Update file exceeds size limit");
        std::string result; std::array<char, 8192> buffer{}; DWORD count = 0;
        for (;;) {
            if (!ReadFile(handle_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
                throw std::runtime_error("Cannot read update file");
            if (!count) return result;
            if (result.size() + count > limit) throw std::runtime_error("Update file exceeds size limit");
            result.append(buffer.data(), count);
        }
    }
private:
    Handle handle_;
};

// An exclusive file handle is shared by all processes and logon sessions using
// this cache. Keep the directory open without delete sharing as well.
class StorageLease {
public:
    StorageLease(const std::filesystem::path& root, const std::atomic<bool>& cancelled)
        : directory_(Owned(CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr))) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!directory_ || !GetFileInformationByHandle(directory_.get(), &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Unsafe update directory");
        const auto path = root / L".lock";
        for (;;) {
            CheckCancelled(cancelled);
            auto handle = Owned(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (handle) {
                if (!GetFileInformationByHandle(handle.get(), &info) || info.nNumberOfLinks != 1 ||
                    (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
                    throw std::runtime_error("Unsafe update lock");
                lock_ = std::move(handle); return;
            }
            if (GetLastError() != ERROR_SHARING_VIOLATION && GetLastError() != ERROR_LOCK_VIOLATION)
                throw std::runtime_error("Cannot lock update storage");
            Sleep(50);
        }
    }
private:
    Handle directory_, lock_;
};
} // namespace rimes::windows::update_storage
