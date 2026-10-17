#include "capsule_capture.hpp"
#include "capsule_hash.hpp"
#include "capsule_resources.hpp"
#include "capsule_text.hpp"
#include "private_files.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <openssl/evp.h>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace rimes::capsule {
namespace {
using Json = nlohmann::json;
struct FD {
    int value;
    ~FD() {
        if (value >= 0)
            close(value);
    }
};
std::string Name(const CaptureRecord& r) {
    return r.kind == "video" ? "original.webm" : "original.png";
}
Json Encode(const CaptureRecord& r) {
    return {{"schemaVersion", 1},        {"id", r.id},
            {"title", r.title},          {"kind", r.kind},
            {"createdAt", r.created},    {"source", r.source},
            {"original", r.original},    {"output", r.output},
            {"width", r.width},          {"height", r.height},
            {"duration", r.duration},    {"bytes", r.bytes},
            {"sha256", r.sha256},        {"collectionID", r.collection_id},
            {"incomplete", r.incomplete}};
}
CaptureRecord Decode(const std::string& raw, const std::string& id) {
    const auto j = Json::parse(raw);
    CaptureRecord r;
    if (j.at("schemaVersion") != 1)
        throw std::runtime_error("捕获格式不支持");
    r.id = j.at("id");
    r.title = j.at("title");
    r.kind = j.at("kind");
    r.created = j.at("createdAt");
    r.source = j.at("source");
    r.original = j.at("original");
    r.output = j.at("output");
    r.width = j.at("width");
    r.height = j.at("height");
    r.duration = j.at("duration");
    r.bytes = j.at("bytes");
    r.sha256 = j.at("sha256");
    r.collection_id = j.value("collectionID", "");
    r.incomplete = j.value("incomplete", false);
    r.revision = runtime::Sha256(raw);
    if (r.id != id || !LooksLikeUuid(id) || (r.kind != "image" && r.kind != "video") ||
        r.original != Name(r) || r.output != r.original || r.title.size() > 4096 ||
        r.source.size() > 4096 || !std::isfinite(r.created) || r.created < 0 ||
        !std::isfinite(r.duration) || r.duration < 0 || r.width <= 0 || r.height <= 0 ||
        r.width > 32768 || r.height > 32768 || r.bytes > CaptureStore::kMaxFileBytes ||
        r.sha256.size() != 64 || r.sha256.find_first_not_of("0123456789abcdef") != r.sha256.npos ||
        (!r.collection_id.empty() && !LooksLikeUuid(r.collection_id)))
        throw std::runtime_error("捕获索引包含无效数据");
    return r;
}
std::optional<CaptureRecord> Read(const std::filesystem::path& root, const std::string& id) {
    if (!LooksLikeUuid(id))
        return {};
    try {
        if (!std::filesystem::exists(root / id))
            return {};
        const auto raw = runtime::PrivateDirectory(root / id).Read("record.json", 32768);
        if (raw)
            return Decode(*raw, id);
    } catch (const std::exception&) {
    }
    return {};
}
std::vector<CaptureRecord> ReadAll(const std::filesystem::path& root) {
    std::vector<CaptureRecord> values;
    std::size_t inspected = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (++inspected > 25000)
            throw std::runtime_error("捕获目录条目过多");
        const auto id = entry.path().filename().string();
        if (auto record = Read(root, id))
            values.push_back(*record);
    }
    std::sort(values.begin(), values.end(),
              [](const auto& a, const auto& b) { return a.created > b.created; });
    return values;
}
CaptureRecord Write(const std::filesystem::path& root, CaptureRecord r) {
    const auto raw = Encode(r).dump();
    r = Decode(raw, r.id);
    runtime::PrivateDirectory(root / r.id).Write("record.json", raw);
    return r;
}
std::set<std::filesystem::path> References(const std::filesystem::path& root,
                                           std::set<std::string>* live_ids = nullptr) {
    ContentStore library(root.parent_path());
    std::vector<Record> entries;
    std::string error;
    if (library.ListAll(&entries, &error) != StoreStatus::Ok)
        throw std::runtime_error("无法检查收藏引用");
    std::set<std::filesystem::path> paths;
    for (const auto& r : entries) {
        if (live_ids)
            live_ids->insert(r.id);
        if (r.kind == Kind::Image || r.kind == Kind::Video)
            paths.insert(std::filesystem::path(r.content).lexically_normal());
    }
    return paths;
}
bool Retire(const std::filesystem::path& root, const CaptureRecord& r) {
    FD media{OpenResource(root / r.id / r.output)};
    if (media.value >= 0 && flock(media.value, LOCK_EX | LOCK_NB))
        return false;
    // Mark the manifest absent first. Only the two owned names are removed;
    // unexpected files are preserved and prevent the final rmdir.
    runtime::PrivateDirectory folder(root / r.id);
    folder.Remove("record.json");
    folder.Remove(r.original);
    std::error_code ignored;
    std::filesystem::remove(root / r.id, ignored);
    return true;
}
} // namespace
std::string CaptureDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds >= 2147483647)
        return "--:--";
    const auto whole = static_cast<int>(seconds);
    char label[32];
    std::snprintf(label, sizeof(label), "%02d:%02d", whole / 60, whole % 60);
    return label;
}
CaptureStore::CaptureStore(std::filesystem::path root) : root_(std::move(root)) {
    runtime::PrivateDirectory directory(root_);
}
std::filesystem::path CaptureStore::DefaultRoot() {
    return ContentStore::DefaultRoot() / "captures";
}
std::vector<CaptureRecord> CaptureStore::List() const {
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock();
    return ReadAll(root_);
}
std::optional<CaptureRecord> CaptureStore::Get(const std::string& id,
                                               const std::string& revision) const {
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock();
    auto r = Read(root_, id);
    if (r && !revision.empty() && r->revision != revision)
        return {};
    return r;
}
std::optional<CaptureAsset> CaptureStore::Acquire(const std::string& id,
                                                  const std::string& revision) const {
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock();
    auto record = Read(root_, id);
    if (!record || record->revision != revision)
        return {};
    const auto path = File(*record);
    auto media = std::shared_ptr<FD>(new FD{OpenResource(path)});
    struct stat info {};
    if (media->value < 0 || fstat(media->value, &info) || !S_ISREG(info.st_mode) ||
        static_cast<std::uint64_t>(info.st_size) != record->bytes ||
        flock(media->value, LOCK_SH | LOCK_NB))
        throw std::runtime_error("捕获文件不可用");
    return CaptureAsset{*record, path, std::move(media)};
}
std::filesystem::path CaptureStore::File(const CaptureRecord& r) const {
    if (!LooksLikeUuid(r.id) || r.output != Name(r))
        throw std::runtime_error("无效捕获路径");
    return root_ / r.id / r.output;
}
CaptureRecord
CaptureStore::Import(const std::filesystem::path& path, CaptureRecord r,
                     const std::atomic<bool>& cancelled,
                     const std::function<bool(const std::function<void()>&)>& publish) {
    if (cancelled.load())
        throw std::runtime_error("捕获已取消");
    if (r.kind != "image" && r.kind != "video")
        throw std::runtime_error("捕获类型无效");
    FD input{OpenResource(path)};
    struct stat before {
    }, after{};
    if (input.value < 0 || fstat(input.value, &before) || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 || static_cast<std::uint64_t>(before.st_size) > kMaxFileBytes)
        throw std::runtime_error("捕获文件不可用或超过 1 GiB");
    std::array<unsigned char, 8> signature{};
    if (pread(input.value, signature.data(), signature.size(), 0) != 8 ||
        (r.kind == "image" &&
         signature != std::array<unsigned char, 8>{137, 80, 78, 71, 13, 10, 26, 10}) ||
        (r.kind == "video" && (signature[0] != 0x1a || signature[1] != 0x45 ||
                               signature[2] != 0xdf || signature[3] != 0xa3)))
        throw std::runtime_error("捕获格式不是 PNG 或 WebM");
    if (!std::isfinite(r.created) || r.created < 0 || r.created > 253402300799.0)
        throw std::runtime_error("捕获时间无效");
    r.id = NewUuidV4();
    r.original = r.output = Name(r);
    r.collection_id.clear();
    if (!std::isfinite(r.duration) || r.duration < 0)
        r.duration = 0;
    if (r.created == 0)
        r.created =
            std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                .count();
    if (r.title.empty()) {
        const auto seconds = static_cast<time_t>(r.created);
        std::tm local{};
        char date[64]{};
        if (!localtime_r(&seconds, &local) ||
            !std::strftime(date, sizeof(date), "%Y/%m/%d %H:%M", &local))
            throw std::runtime_error("捕获时间无效");
        r.title = std::string(r.kind == "video" ? "录屏 " : "截图 ") + date;
    }
    const auto stage = root_ / (".import-" + r.id);
    runtime::PrivateDirectory directory(root_);
    // Stage outside the archive lock. Publication orders the package grant
    // before this lock, just like collection/removal in the host.
    runtime::PrivateDirectory transaction(stage);
    try {
        FD output{open((stage / r.original).c_str(),
                       O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL | O_NOFOLLOW, 0600)};
        if (output.value < 0)
            throw std::runtime_error("无法保存捕获");
        auto hash = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(),
                                                                            EVP_MD_CTX_free);
        if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("摘要不可用");
        std::array<char, 65536> chunk{};
        r.bytes = 0;
        for (;;) {
            if (cancelled.load())
                throw std::runtime_error("捕获已取消");
            const auto count = read(input.value, chunk.data(), chunk.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                throw std::runtime_error("捕获读取失败");
            if (!count)
                break;
            r.bytes += count;
            if (r.bytes > kMaxFileBytes)
                throw std::runtime_error("捕获超过 1 GiB");
            if (EVP_DigestUpdate(hash.get(), chunk.data(), count) != 1)
                throw std::runtime_error("摘要失败");
            for (ssize_t offset = 0; offset < count;) {
                const auto written = write(output.value, chunk.data() + offset, count - offset);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    throw std::runtime_error("捕获保存失败");
                offset += written;
            }
        }
        if (fstat(input.value, &after) || before.st_size != after.st_size ||
            before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
            before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
            r.bytes != static_cast<std::uint64_t>(after.st_size) || fsync(output.value))
            throw std::runtime_error("捕获文件在读取期间变化");
        std::array<unsigned char, 32> digest{};
        unsigned int length = 0;
        if (EVP_DigestFinal_ex(hash.get(), digest.data(), &length) != 1 || length != 32)
            throw std::runtime_error("摘要失败");
        constexpr char hex[] = "0123456789abcdef";
        r.sha256.clear();
        for (const auto c : digest) {
            r.sha256 += hex[c >> 4];
            r.sha256 += hex[c & 15];
        }
        const auto raw = Encode(r).dump();
        r = Decode(raw, r.id);
        transaction.Write("record.json", raw);
        if (cancelled.load())
            throw std::runtime_error("捕获已取消");
        const auto commit = [&] {
            auto lock = directory.Lock();
            if (cancelled.load())
                throw std::runtime_error("捕获已取消");
            std::filesystem::rename(stage, root_ / r.id);
        };
        if (publish) {
            if (!publish(commit))
                throw std::runtime_error("捕获已取消");
        } else
            commit();
        FD parent{OpenResource(root_)};
        if (parent.value >= 0)
            fsync(parent.value);
        return r;
    } catch (...) {
        transaction.Remove("record.json");
        transaction.Remove(r.original);
        std::error_code ignored;
        std::filesystem::remove(stage, ignored);
        throw;
    }
}
CaptureRecord CaptureStore::Collect(const std::string& id, const std::string& revision) {
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock();
    auto r = Read(root_, id);
    if (!r || r->revision != revision)
        throw std::runtime_error("捕获已变化，请重新选择");
    ContentStore library(root_.parent_path());
    Record entry;
    std::string error;
    auto status = library.Get(id, &entry, &error);
    if (status == StoreStatus::NotFound) {
        entry.id = id;
        entry.kind = r->kind == "video" ? Kind::Video : Kind::Image;
        entry.title = r->title;
        entry.content = File(*r).string();
        entry.summary = r->source;
        if (library.Put(entry, "", &entry, &error) != StoreStatus::Ok)
            throw std::runtime_error("收藏保存失败");
    } else if (status != StoreStatus::Ok || entry.content != File(*r).string())
        throw std::runtime_error("收藏标识冲突");
    r->collection_id = id;
    return Write(root_, *r);
}
bool CaptureStore::Remove(const std::string& id, const std::string& revision) {
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock();
    auto r = Read(root_, id);
    if (!r || r->revision != revision)
        return false;
    if (!r->collection_id.empty() || References(root_).count(File(*r)))
        throw std::runtime_error("已收藏内容请在 Capsule 管理中处理");
    if (!Retire(root_, *r))
        throw std::runtime_error("捕获正在预览或复制，请稍后再删除");
    return true;
}
std::size_t CaptureStore::Prune(double now, double retention, std::uint64_t capacity) {
    if (!std::isfinite(now) || now < 0 || !std::isfinite(retention) || retention < 0)
        return 0;
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock();
    std::set<std::string> live_ids;
    const auto references = References(root_, &live_ids);
    auto records = ReadAll(root_);
    // A deleted library entry no longer makes its capture permanent. Keep all
    // live path references, even when their content-library IDs differ.
    for (auto& r : records)
        if (!r.collection_id.empty() && !live_ids.count(r.collection_id)) {
            r.collection_id.clear();
            r = Write(root_, r);
        }
    std::uint64_t total = 0;
    std::size_t removed = 0;
    for (const auto& r : records)
        if (r.collection_id.empty() && !references.count(File(r)))
            total += r.bytes;
    for (auto i = records.rbegin(); i != records.rend(); ++i) {
        if (!i->collection_id.empty() || references.count(File(*i)))
            continue;
        if (now - i->created > retention || total > capacity) {
            if (Retire(root_, *i)) {
                total -= i->bytes;
                ++removed;
            }
        }
    }
    return removed;
}
Card CaptureCard(const CaptureRecord& r) {
    Card card;
    card.id = r.id;
    card.revision = r.revision;
    card.module = Module::Capture;
    card.kind = r.kind == "video" ? Kind::Video : Kind::Image;
    card.filter = r.kind;
    card.title = r.title;
    card.source = r.source;
    card.favorite = !r.collection_id.empty();
    card.preview = std::string(r.source.find("导入") != std::string::npos ? "导入 · " : "捕获 · ") +
                   (r.kind == "video" ? "录屏" : "截图");
    if (r.kind == "video")
        card.preview += " · " + CaptureDuration(r.duration);
    if (card.favorite)
        card.preview += " · 已收藏";
    if (r.incomplete)
        card.preview += " · 未完成";
    return card;
}
} // namespace rimes::capsule
