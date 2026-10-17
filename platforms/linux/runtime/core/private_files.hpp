#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>

namespace rimes::runtime {

std::string RandomID();
std::string Sha256(std::string_view bytes);
std::filesystem::path UserRoot();

class StorageBusy : public std::runtime_error {
public:
    StorageBusy() : std::runtime_error("Cannot lock private storage") {}
};

class FileLock {
public:
    explicit FileLock(int fd) : fd_(fd) {}
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&& other) noexcept;
private:
    int fd_;
};

// All access stays relative to a verified, pinned directory descriptor. No
// operation follows a symlink, accepts a path component, or reads other users'
// files. Only this directory must be 0700; the existing Rime root may be 0755.
class PrivateDirectory {
public:
    explicit PrivateDirectory(const std::filesystem::path& path);
    ~PrivateDirectory();
    PrivateDirectory(const PrivateDirectory&) = delete;
    PrivateDirectory& operator=(const PrivateDirectory&) = delete;

    std::optional<std::string> Read(std::string_view name, std::size_t limit) const;
    void Write(std::string_view name, std::string_view bytes) const;
    void Remove(std::string_view name) const;
    FileLock Lock(bool wait = true) const;
    const std::filesystem::path& Path() const { return path_; }

private:
    static std::string Name(std::string_view name);
    void Verify() const;
    int fd_ = -1;
    std::filesystem::path path_;
};

} // namespace rimes::runtime
