#include "application_update_storage.hpp"
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>

namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
int wmain(int argc, wchar_t** argv) {
    using namespace rimes::windows::update_storage;
    if (argc == 2 && std::wstring(argv[1]) == L"--child-exit") return 0;
    const auto root = std::filesystem::temp_directory_path() /
        (L"xsime-update-storage-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        Check(std::filesystem::create_directory(root), "fixture directory already exists");
        std::atomic<bool> cancelled{false};
        {
            StorageLease owner(root, cancelled);
            auto waiter = std::async(std::launch::async, [&] {
                try { StorageLease other(root, cancelled); return false; }
                catch (const std::runtime_error&) { return cancelled.load(); }
            });
            Check(waiter.wait_for(std::chrono::milliseconds(120)) == std::future_status::timeout, "cache permits overlapping transactions");
            cancelled = true;
            Check(waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready && waiter.get(), "cache wait ignores cancellation");
        }
        cancelled = false;
        { StorageLease retry(root, cancelled); }
        const auto package = root / L"package.exe", replacement = root / L"replacement.exe";
        { std::ofstream(package, std::ios::binary) << "verified bytes"; std::ofstream(replacement, std::ios::binary) << "replacement"; }
        {
            LockedFile verified(package);
            Check(verified.Read(128) == "verified bytes", "file snapshot differs");
            auto writer = Owned(CreateFileW(package.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
            Check(!writer, "verified executable is writable during handoff");
            Check(!DeleteFileW(package.c_str()), "verified executable can be unlinked during handoff");
            Check(!MoveFileExW(replacement.c_str(), package.c_str(), MOVEFILE_REPLACE_EXISTING), "verified executable can be replaced during handoff");
        }
        Check(MoveFileExW(replacement.c_str(), package.c_str(), MOVEFILE_REPLACE_EXISTING), "file remains locked after handoff release");
        const auto alias = root / L"hardlink.exe";
        Check(CreateHardLinkW(alias.c_str(), package.c_str(), nullptr), "cannot create hardlink fixture");
        bool rejected = false;
        try { LockedFile linked(package); } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "hardlinked cache file accepted");
        Check(DeleteFileW(alias.c_str()), "hardlink fixture cleanup failed");
        Check(DeleteFileW(package.c_str()), "fixture executable cleanup failed");
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        Check(length > 0 && length < executable.size(), "test executable path unavailable");
        Check(CopyFileW(executable.data(), package.c_str(), TRUE), "cannot copy launch fixture");
        {
            LockedFile verified(package);
            const auto bytes = verified.Read(16 * 1024 * 1024);
            Check(!bytes.empty(), "empty launch fixture");
            auto command = L"\"" + package.wstring() + L"\" --child-exit";
            STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
            Check(CreateProcessW(package.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &startup, &process),
                  "read-only pinned executable cannot be started");
            auto thread = Owned(process.hThread), child = Owned(process.hProcess);
            Check(WaitForSingleObject(child.get(), 10000) == WAIT_OBJECT_0, "launch fixture did not finish");
            DWORD code = 1;
            Check(GetExitCodeProcess(child.get(), &code) && code == 0, "launch fixture failed");
        }
        std::filesystem::remove_all(root);
        std::cout << "Update cache contention and pinned executable launch passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored; std::filesystem::remove_all(root, ignored);
        std::cerr << error.what() << '\n'; return 1;
    }
}
