#include "capsule_resources.hpp"
#include "capsule_rich_text.hpp"
#include "capsule_text.hpp"
#include "private_files.hpp"
#include "process.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>
namespace rimes::capsule {
namespace {
struct FD {
    int value;
    ~FD() {
        if (value >= 0)
            close(value);
    }
};
} // namespace
int OpenResource(const std::filesystem::path& path) {
    const auto raw = path.string();
    if (!path.is_absolute() || raw.size() > 4096 || raw.find('\0') != raw.npos ||
        raw.find_first_of("\r\n") != raw.npos)
        return -1;
    std::vector<std::string> parts;
    for (const auto& part : path.relative_path()) {
        if (part == "..")
            return -1;
        if (part != "." && !part.empty())
            parts.push_back(part.string());
    }
    if (parts.empty())
        return -1;
    FD current{open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const int next = openat(current.value, parts[i].c_str(),
                                O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC |
                                    (i + 1 < parts.size() ? O_DIRECTORY : 0));
        if (next < 0)
            return -1;
        close(current.value);
        current.value = next;
    }
    struct stat info {};
    if (fstat(current.value, &info) || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)))
        return -1;
    const auto result = current.value;
    current.value = -1;
    return result;
}
bool ValidateResource(const Record& record, std::string* error) {
    FD fd{OpenResource(record.content)};
    struct stat info {};
    const bool regular = fd.value >= 0 && !fstat(fd.value, &info) && S_ISREG(info.st_mode);
    const bool directory = fd.value >= 0 && S_ISDIR(info.st_mode);
    bool valid = false;
    if (record.kind == Kind::Pdf)
        valid = regular &&
                LowerText(std::filesystem::path(record.content).extension().string()) == ".pdf";
    if (record.kind == Kind::Image || record.kind == Kind::Video) valid = regular;
    if (record.kind == Kind::Skill || record.kind == Kind::Resource)
        valid = regular || directory;
    if (!valid && error)
        *error = "请选择可读的本机绝对路径；不接受链接或特殊文件，文献需要 PDF 文件";
    return valid;
}
std::string ReadResource(const std::filesystem::path& path, std::size_t limit,
                         const std::atomic<bool>& cancelled) {
    FD fd{OpenResource(path)};
    struct stat before {
    }, after{};
    if (fd.value < 0 || fstat(fd.value, &before) || !S_ISREG(before.st_mode) ||
        before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > limit)
        throw std::runtime_error("文件不可用或超过预览上限");
    std::string bytes;
    std::array<char, 65536> chunk{};
    for (;;) {
        if (cancelled.load())
            throw std::runtime_error("预览已取消");
        const auto count = read(fd.value, chunk.data(), chunk.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            throw std::runtime_error("文件读取失败");
        if (!count)
            break;
        if (bytes.size() + count > limit)
            throw std::runtime_error("文件超过预览上限");
        bytes.append(chunk.data(), count);
    }
    if (fstat(fd.value, &after) || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        static_cast<std::uint64_t>(after.st_size) != bytes.size())
        throw std::runtime_error("文件已变化，请重新预览");
    return bytes;
}
std::filesystem::path ResolveResource(const Record& record, const ContentStore& store) {
    if (record.attachment.empty()) {
        std::string error;
        if (!ValidateResource(record, &error))
            throw std::runtime_error(error);
        return record.content;
    }
    std::string bytes, error;
    if (store.ReadAttachment(record.attachment, &bytes, &error) != StoreStatus::Ok)
        throw std::runtime_error("保存的附件不可用");
    auto name = std::filesystem::path(record.content).filename().string();
    if (name.empty() || name == "." || name == ".." || name.size() > 255 ||
        name.find_first_of("\r\n") != name.npos || name.find('\0') != name.npos)
        throw std::runtime_error("附件名称无效");
    const auto folder = store.root() / "files" / record.attachment;
    runtime::PrivateDirectory output(folder);
    // Only materialize on an explicit preview/copy; never replace user paths.
    if (output.Read(name, 256 * 1024 * 1024) != std::optional<std::string>(bytes))
        output.Write(name, bytes);
    return folder / name;
}
std::string ResourceUri(const std::filesystem::path& path) {
    std::string result = "file://";
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char ch : path.string()) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '/' || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            result += static_cast<char>(ch);
        else {
            result += '%';
            result += hex[ch >> 4];
            result += hex[ch & 15];
        }
    }
    return result;
}
ResourcePreview PreviewResource(const Record& record, const ContentStore& store,
                                const std::atomic<bool>& cancelled) {
    ResourcePreview preview;
    try {
        if (cancelled.load())
            return preview;
        auto path = ResolveResource(record, store);
        FD fd{OpenResource(path)};
        struct stat info {};
        if (fd.value < 0 || fstat(fd.value, &info))
            throw std::runtime_error("文件已不可用");
        if (S_ISDIR(info.st_mode)) {
            preview.message = "本机文件夹路径 · 不递归复制目录";
            if (record.kind != Kind::Skill)
                return preview;
            path /= "SKILL.md";
        }
        std::array<char, 4096> self{};
        const auto count = readlink("/proc/self/exe", self.data(), self.size() - 1);
        if (count <= 0)
            throw std::runtime_error("预览程序不可用");
        std::filesystem::path helper =
            std::filesystem::path(std::string(self.data(), count)).parent_path() /
            "rimes-capsule-preview";
        if (access(helper.c_str(), X_OK))
            helper = RIMES_CAPSULE_PREVIEW_PATH;
        const auto result = runtime::RunProcess({helper.string(), path.string()}, "", cancelled,
                                                std::chrono::seconds(8), 16 * 1024 * 1024);
        if (!result.success)
            throw std::runtime_error("无法预览此文件，或已超出时间 / 内存限制；仍可复制文件");
        const auto value = nlohmann::json::parse(result.output);
        preview.text = value.value("text", "");
        preview.message = value.value("message", "");
        preview.pages = value.value("pages", 0);
        preview.png = Base64Decode(value.value("png", "")).value_or("");
    } catch (const std::exception& error) {
        preview.message = error.what();
    }
    return preview;
}
} // namespace rimes::capsule
