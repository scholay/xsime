#include "capsule_resources.hpp"
#include "capsule_text.hpp"
#include "inbox_actions.hpp"
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace rimes::mailbox {
namespace {
void Need(bool ok, const char* text) {
    if (!ok)
        throw std::runtime_error(text);
}
struct FD {
    int value;
    ~FD() {
        if (value >= 0)
            close(value);
    }
};
bool Same(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
} // namespace
bool SaveToCapsule(Store& store, const std::string& thread, const std::string& revision,
                   const std::string& message, capsule::ContentStore& destination) {
    bool duplicate = false;
    store.WithMessage(
        thread, revision, message, [&](const Json& row, const Json& content, const Commit& gate) {
            std::string png;
            if (content.contains("imageFileName") && !content.at("imageFileName").is_null()) {
                const auto image = store.Image(content.at("imageFileName"));
                Need(image.has_value(), "图片附件不可用");
                png = *image;
            }
            std::string error;
            Need(destination.SaveReceived(capsule::CharacterPrefix(Title(row), 200),
                                          content.at("body"), png, nullptr, &duplicate, &error,
                                          gate) == capsule::StoreStatus::Ok,
                 "未能存入 Capsule，请检查存储状态");
        });
    return duplicate;
}
void ExportImage(Store& store, const std::string& thread, const std::string& revision,
                 const std::string& message, const std::filesystem::path& destination,
                 bool overwrite) {
    Need(destination.is_absolute() && destination.extension() == ".png", "请选择 PNG 保存位置");
    const auto parent = destination.parent_path();
    const auto name = destination.filename().string();
    Need(!name.empty() && name.size() <= 255 && name.find('\0') == name.npos &&
             name.find_first_of("\r\n") == name.npos,
         "保存文件名无效");
    FD folder{capsule::OpenResource(parent)};
    struct stat directory {};
    Need(folder.value >= 0 && !fstat(folder.value, &directory) && S_ISDIR(directory.st_mode),
         "保存目录不可用");
    struct stat original {};
    const bool existed = !fstatat(folder.value, name.c_str(), &original, AT_SYMLINK_NOFOLLOW);
    Need((existed && overwrite && S_ISREG(original.st_mode)) || (!existed && errno == ENOENT),
         "保存位置已经存在或不是普通文件");
    store.WithMessage(
        thread, revision, message, [&](const Json&, const Json& content, const Commit& gate) {
            Need(content.contains("imageFileName") && content.at("imageFileName").is_string(),
                 "这条收件没有图片");
            const auto image = store.Image(content.at("imageFileName"));
            Need(image.has_value(), "图片附件不可用");
            const auto temporary = ".mailbox-" + runtime::RandomID() + ".tmp";
            FD file{openat(folder.value, temporary.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
            Need(file.value >= 0, "无法创建图片文件");
            struct Cleanup {
                int folder;
                std::string name;
                ~Cleanup() { unlinkat(folder, name.c_str(), 0); }
            } cleanup{folder.value, temporary};
            for (std::size_t offset = 0; offset < image->size();) {
                const auto size = write(file.value, image->data() + offset, image->size() - offset);
                if (size < 0 && errno == EINTR)
                    continue;
                Need(size > 0, "图片写入失败");
                offset += size;
            }
            Need(!fsync(file.value), "图片写入失败");
            gate([&] {
                // Keep the user-selected directory pinned; never chmod that directory.
                FD current{capsule::OpenResource(parent)};
                struct stat now {};
                Need(current.value >= 0 && !fstat(current.value, &now) &&
                         now.st_dev == directory.st_dev && now.st_ino == directory.st_ino,
                     "保存目录已变化");
                const bool exists = !fstatat(folder.value, name.c_str(), &now, AT_SYMLINK_NOFOLLOW);
                Need(existed ? (exists && S_ISREG(now.st_mode) && Same(original, now))
                             : (!exists && errno == ENOENT),
                     "保存文件已变化，请重新选择");
                if (existed)
                    Need(!renameat(folder.value, temporary.c_str(), folder.value, name.c_str()),
                         "无法保存图片");
                else
                    Need(!syscall(SYS_renameat2, folder.value, temporary.c_str(), folder.value,
                                  name.c_str(), RENAME_NOREPLACE),
                         "无法保存图片");
                // Atomic no-replace rename also works on vfat/exFAT without hard links.
                // Rename is the commit point. A directory fsync failure must not
                // report cancellation after the user-visible file was published.
                (void)fsync(folder.value);
            });
        });
}
} // namespace rimes::mailbox
