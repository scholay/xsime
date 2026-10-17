#include "private_files.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rimes::runtime {
namespace {
struct FD {
    int value;
    explicit FD(int fd) : value(fd) {}
    ~FD() { if (value >= 0) close(value); }
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;
};
void SafeFile(int fd) {
    struct stat status{};
    if (fstat(fd, &status) || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0777) != 0600 || status.st_nlink != 1)
        throw std::runtime_error("Unsafe private file");
}
std::string Hex(const unsigned char* bytes, std::size_t count) {
    std::string result;
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i != count; ++i) {
        result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15];
    }
    return result;
}
}

std::string RandomID() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw std::runtime_error("Random identity unavailable");
    return Hex(bytes.data(), bytes.size());
}

std::string Sha256(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Checksum unavailable");
    return Hex(digest.data(), size);
}

std::filesystem::path UserRoot() {
    const char* direct = std::getenv("RIMES_USER_DIR");
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path path;
    if (direct && *direct) path = direct;
    else if (data && *data && std::filesystem::path(data).is_absolute()) path = std::filesystem::path(data) / "rimes";
    else if (home && *home) path = std::filesystem::path(home) / ".local/share/rimes";
    if (!path.is_absolute()) throw std::runtime_error("Absolute user data directory required");
    return path;
}

PrivateDirectory::PrivateDirectory(const std::filesystem::path& path) : path_(path) {
    if (!path.is_absolute() || path == path.root_path()) throw std::runtime_error("Invalid private directory");
    FD parent(open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY));
    if (parent.value < 0) throw std::runtime_error("Cannot open directory root");
    for (const auto& part : path.relative_path()) {
        const auto component = part.string();
        if (component.empty() || component == "." || component == "..") throw std::runtime_error("Invalid directory component");
        int next = openat(parent.value, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT) {
            if (mkdirat(parent.value, component.c_str(), 0700) && errno != EEXIST)
                throw std::runtime_error("Cannot create private directory");
            next = openat(parent.value, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) throw std::runtime_error("Unsafe directory component");
        close(parent.value); parent.value = next;
    }
    struct stat status{};
    if (fstat(parent.value, &status) || status.st_uid != geteuid() || (status.st_mode & 0777) != 0700)
        throw std::runtime_error("Private directory must be owned by this user with mode 0700");
    fd_ = parent.value; parent.value = -1;
}
PrivateDirectory::~PrivateDirectory() { if (fd_ >= 0) close(fd_); }
void PrivateDirectory::Verify() const {
    struct stat status{};
    if (fstat(fd_, &status) || !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0777) != 0700 || status.st_nlink == 0)
        throw std::runtime_error("Private directory permissions changed");
}

std::string PrivateDirectory::Name(std::string_view name) {
    if (name.empty() || name.size() > 240 || name == "." || name == ".." ||
        name.find('/') != std::string_view::npos || name.find('\0') != std::string_view::npos)
        throw std::runtime_error("Invalid private filename");
    return std::string(name);
}

std::optional<std::string> PrivateDirectory::Read(std::string_view name, std::size_t limit) const {
    Verify();
    const auto filename = Name(name);
    FD file(openat(fd_, filename.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if (file.value < 0) {
        if (errno == ENOENT) return std::nullopt;
        throw std::runtime_error("Cannot read private file");
    }
    SafeFile(file.value);
    struct stat status{};
    if (fstat(file.value, &status) || status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > limit)
        throw std::runtime_error("Private file exceeds size limit");
    std::string result;
    std::array<char, 8192> block{};
    for (;;) {
        const auto count = read(file.value, block.data(), block.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::runtime_error("Cannot read private file");
        if (!count) return result;
        if (result.size() + static_cast<std::size_t>(count) > limit) throw std::runtime_error("Private file exceeds size limit");
        result.append(block.data(), static_cast<std::size_t>(count));
    }
}

void PrivateDirectory::Write(std::string_view name, std::string_view bytes) const {
    Verify();
    const auto filename = Name(name);
    struct stat existing{};
    if (!fstatat(fd_, filename.c_str(), &existing, AT_SYMLINK_NOFOLLOW)) {
        FD old(openat(fd_, filename.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
        if (old.value < 0) throw std::runtime_error("Unsafe existing private file");
        SafeFile(old.value);
    } else if (errno != ENOENT) throw std::runtime_error("Cannot inspect private file");
    const auto temp = ".write-" + RandomID();
    FD file(openat(fd_, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.value < 0) throw std::runtime_error("Cannot create private file");
    try {
        if (fchmod(file.value, 0600)) throw std::runtime_error("Cannot protect private file");
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto written = write(file.value, bytes.data() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("Cannot write private file");
            offset += static_cast<std::size_t>(written);
        }
        if (fsync(file.value) || renameat(fd_, temp.c_str(), fd_, filename.c_str()))
            throw std::runtime_error("Cannot publish private file");
        if (fsync(fd_)) throw std::runtime_error("Cannot sync private directory");
    } catch (...) { unlinkat(fd_, temp.c_str(), 0); throw; }
}

void PrivateDirectory::Remove(std::string_view name) const {
    Verify();
    const auto filename = Name(name);
    FD file(openat(fd_, filename.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (file.value < 0 && errno == ENOENT) return;
    if (file.value < 0) throw std::runtime_error("Unsafe private file");
    SafeFile(file.value);
    if (unlinkat(fd_, filename.c_str(), 0) || fsync(fd_)) throw std::runtime_error("Cannot remove private file");
}

FileLock PrivateDirectory::Lock(bool wait) const {
    Verify();
    FD file(openat(fd_, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
    if (file.value < 0) throw std::runtime_error("Cannot open private lock");
    SafeFile(file.value);
    while (flock(file.value, LOCK_EX | (wait ? 0 : LOCK_NB))) {
        if (errno == EINTR) continue;
        if (!wait && (errno == EWOULDBLOCK || errno == EAGAIN)) throw StorageBusy();
        throw std::runtime_error("Cannot lock private storage");
    }
    const auto fd = file.value; file.value = -1;
    return FileLock(fd);
}
FileLock::~FileLock() { if (fd_ >= 0) { flock(fd_, LOCK_UN); close(fd_); } }
FileLock::FileLock(FileLock&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

} // namespace rimes::runtime
