#include "capsule_hash.hpp"
#include "capsule_history.hpp"
#include "capsule_store.hpp"
#include "capsule_text.hpp"
#include "private_files.hpp"
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace rimes::capsule {
namespace {
using Json = nlohmann::json;
void Require(bool value) {
    if (!value)
        throw std::runtime_error("Clipboard history data unavailable or invalid");
}
bool SafeType(const std::string& type) {
    return !type.empty() && type.size() <= 4096 &&
           std::none_of(type.begin(), type.end(),
                        [](unsigned char c) { return c < 32 || c >= 127; });
}
void ValidateArchive(const ClipboardArchive& archive) {
    Require(!archive.representations.empty() && archive.representations.size() <= 128);
    std::set<std::string> unique;
    std::vector<std::string> types;
    std::size_t total = 0;
    for (const auto& value : archive.representations) {
        Require(SafeType(value.type) && unique.insert(value.type).second);
        Require(value.format == 8 || value.format == 16 || value.format == 32);
        Require(value.bytes.size() <= ClipboardArchive::maxTypeBytes &&
                value.bytes.size() % (value.format / 8) == 0);
        total += value.bytes.size();
        types.push_back(value.type);
        Require(total <= ClipboardArchive::maxTotalBytes);
    }
    Require(ClipboardArchive::AllowsTypes(types));
}
std::string Text(const ClipboardArchive& archive, std::initializer_list<const char*> preferred) {
    for (const auto* type : preferred)
        for (const auto& value : archive.representations)
            if (value.type == type && value.format == 8) {
                try {
                    (void)CharacterCount(value.bytes);
                    return value.bytes;
                } catch (...) {
                }
            }
    return {};
}
std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
struct Statement {
    sqlite3_stmt* value = nullptr;
    Statement(sqlite3* db, const char* sql) {
        Require(sqlite3_prepare_v2(db, sql, -1, &value, nullptr) == SQLITE_OK);
    }
    ~Statement() { sqlite3_finalize(value); }
    void Bind(int at, const std::string& text) {
        Require(sqlite3_bind_text64(value, at, text.data(), text.size(), SQLITE_TRANSIENT,
                                    SQLITE_UTF8) == SQLITE_OK);
    }
    void Number(int at, std::int64_t value_) {
        Require(sqlite3_bind_int64(value, at, value_) == SQLITE_OK);
    }
    bool Next() {
        const auto result = sqlite3_step(value);
        Require(result == SQLITE_ROW || result == SQLITE_DONE);
        return result == SQLITE_ROW;
    }
    std::string String(int at) const {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(value, at));
        return text ? std::string(text, sqlite3_column_bytes(value, at)) : "";
    }
};
constexpr const char* columns = "id,revision,kind,title,preview,backend,application,source,"
                                "captured,bytes,favorite,insertable,filter";
ClipboardEntry Entry(const Statement& row) {
    ClipboardEntry value;
    value.id = row.String(0);
    value.revision = row.String(1);
    value.kind = row.String(2);
    value.title = row.String(3);
    value.preview = row.String(4);
    value.source = {row.String(5), row.String(6), row.String(7)};
    value.captured = sqlite3_column_int64(row.value, 8);
    value.bytes = sqlite3_column_int64(row.value, 9);
    value.favorite = sqlite3_column_int(row.value, 10);
    value.insertable = sqlite3_column_int(row.value, 11);
    value.filter = row.String(12);
    Require(LooksLikeUuid(value.id) && value.revision.size() == 64 && value.title.size() <= 16384 &&
            value.preview.size() <= 16384);
    return value;
}
} // namespace

bool ClipboardArchive::AllowsTypes(const std::vector<std::string>& types) {
    static const std::set<std::string> denied{
        "org.nspasteboard.ConcealedType", "org.nspasteboard.TransientType",
        "com.agilebits.onepassword",      "application/x-kde-passwordManagerHint",
        "x-kde-passwordManagerHint",      "application/x-keepassxc",
        "application/x-rimes-sensitive",  "application/x-rimes-transient"};
    return std::none_of(types.begin(), types.end(),
                        [&](const auto& type) { return !SafeType(type) || denied.count(type); });
}
void ClipboardArchive::Validate() const { ValidateArchive(*this); }
std::string ClipboardArchive::Encode() const {
    Validate();
    Json values = Json::array();
    for (const auto& value : representations)
        values.push_back({{"type", value.type},
                          {"format", value.format},
                          {"data", Json::binary(std::vector<std::uint8_t>(value.bytes.begin(),
                                                                          value.bytes.end()))}});
    const auto bytes = Json::to_cbor(
        Json{{"format", "com.rimes.linux-clipboard"}, {"version", 1}, {"representations", values}});
    Require(bytes.size() <= maxArchiveBytes);
    return {bytes.begin(), bytes.end()};
}
ClipboardArchive ClipboardArchive::Decode(const std::string& bytes) {
    Require(bytes.size() <= maxArchiveBytes);
    const auto json = Json::from_cbor(bytes);
    Require(json.is_object() && json.size() == 3 &&
            json.at("format") == "com.rimes.linux-clipboard" && json.at("version") == 1);
    Require(json.at("representations").is_array() && json.at("representations").size() <= 128);
    ClipboardArchive archive;
    for (const auto& value : json.at("representations")) {
        Require(value.is_object() && value.size() == 3 && value.at("data").is_binary());
        const auto& raw = value.at("data").get_binary();
        archive.representations.push_back(
            {value.at("type"), {raw.begin(), raw.end()}, value.at("format")});
    }
    ValidateArchive(archive);
    return archive;
}
ClipboardProjection ProjectClipboard(const ClipboardArchive& archive) {
    ValidateArchive(archive);
    ClipboardProjection result;
    auto text = Text(archive, {"text/plain;charset=utf-8", "text/plain;charset=UTF-8",
                               "UTF8_STRING", "text/plain", "public.utf8-plain-text"});
    const auto uris = Text(archive, {"text/uri-list", "x-special/gnome-copied-files"});
    bool files = false, image = false;
    for (const auto& value : archive.representations) {
        image |= value.type.rfind("image/", 0) == 0;
    }
    std::istringstream lines(uris);
    std::string line, links, names;
    std::size_t file_count = 0;
    while (std::getline(lines, line)) {
        line = TrimText(line);
        if (line.empty() || line[0] == '#' || line == "copy" || line == "cut")
            continue;
        if (line.rfind("file://", 0) == 0) {
            files = true;
            ++file_count;
        }
        if (!links.empty())
            links += '\n';
        links += line;
        if (!names.empty())
            names += '\n';
        auto name = line.substr(line.rfind('/') == line.npos ? 0 : line.rfind('/') + 1);
        std::string decoded;
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (name[i] == '%' && i + 2 < name.size() && hex(name[i + 1]) >= 0 &&
                hex(name[i + 2]) >= 0) {
                const char value = hex(name[i + 1]) * 16 + hex(name[i + 2]);
                if (value >= 32 || static_cast<unsigned char>(value) >= 128) {
                    decoded += value;
                    i += 2;
                    continue;
                }
            }
            decoded += name[i];
        }
        try {
            (void)CharacterCount(decoded);
            names += decoded;
        } catch (...) {
            names += name;
        }
    }
    if (files) {
        result.kind = "files";
        result.text = links;
        result.title = names.empty() ? "文件" : names;
    } else if (image) {
        result.kind = "image";
        result.title = "图像";
    } else if (!links.empty()) {
        result.kind = "link";
        result.text = links.empty() ? text : links;
        result.title = result.text;
        result.insertable = result.text.find('\0') == result.text.npos;
    } else if (!text.empty()) {
        result.kind = "text";
        result.text = text;
        result.title = text;
        result.insertable = result.text.find('\0') == result.text.npos;
    } else {
        result.kind = "unknown";
        result.title = "未知剪贴板内容";
    }
    if (result.kind == "text" || result.kind == "link")
        result.filter = "text";
    if (result.kind == "image")
        result.filter = "image";
    if (files && file_count == 1 && links.find('\n') == links.npos) {
        static const std::set<std::string> images{"png", "jpg",  "jpeg", "heic", "webp",
                                                  "tif", "tiff", "gif",  "bmp"};
        const auto extension = names.rfind('.');
        if (extension != names.npos && images.count(LowerText(names.substr(extension + 1))))
            result.filter = "image";
    }
    auto preview = [](std::string value, std::size_t characters) {
        std::replace(value.begin(), value.end(), '\0', ' ');
        value = CharacterPrefix(OneLine(value), characters);
        if (value.size() > 4096) {
            std::size_t end = 4096;
            while (end && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80)
                --end;
            value.resize(end);
        }
        return value;
    };
    result.preview = preview(result.title, 280);
    result.title = preview(result.title, 256);
    result.search = result.text;
    if (text != result.text)
        result.search += "\n" + text;
    if (links != result.text && links != text)
        result.search += "\n" + links;
    result.search = LowerText(result.search + "\n" + result.title);
    if (result.kind == "text")
        result.search += " text plain 文本 文字";
    if (result.kind == "link")
        result.search += " link url 链接 网址";
    if (result.kind == "image")
        result.search += " image photo picture screenshot 图片 图像 照片 截图";
    if (result.kind == "files")
        result.search += " file files 文件";
    return result;
}
void ClipboardCaptureGate::Allow(bool allowed) {
    if (allowed_ != allowed) {
        allowed_ = allowed;
        baseline_ = true;
        ++generation_;
    }
}
std::optional<std::uint64_t> ClipboardCaptureGate::Observe(std::uint64_t identity) {
    if (!allowed_)
        return {};
    if (baseline_) {
        Baseline(identity);
        return {};
    }
    if (identity == observed_)
        return {};
    observed_ = identity;
    return ++generation_;
}
bool ClipboardCaptureGate::Current(std::uint64_t generation) const {
    return allowed_ && generation == generation_;
}
void ClipboardCaptureGate::Baseline(std::uint64_t identity) {
    observed_ = identity;
    baseline_ = false;
    ++generation_;
}
void ClipboardCaptureGate::Cancel() { ++generation_; }

struct ClipboardHistoryStore::Impl {
    runtime::PrivateDirectory files;
    ClipboardLimits limits;
    sqlite3* db = nullptr;
    void Exec(const char* sql) {
        Require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    }
    Impl(const std::filesystem::path& root, ClipboardLimits bounds) : files(root), limits(bounds) {
        Require(limits.items > 0 && limits.items <= 100000 && limits.bytes > 0 &&
                limits.bytes <= 64ll * 1024 * 1024 * 1024);
        auto lock = files.Lock();
        const auto path = root / "history-v1.sqlite3";
        // Precreate privately; SQLite also refuses symlinks when opening it.
        int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno == EEXIST)
            fd = open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        struct stat stat {};
        bool safe = fd >= 0 && !fstat(fd, &stat) && S_ISREG(stat.st_mode) &&
                    stat.st_uid == geteuid() && (stat.st_mode & 0777) == 0600 && stat.st_nlink == 1;
        if (fd >= 0)
            close(fd);
        Require(safe);
        for (const char* suffix : {"-journal", "-wal", "-shm"}) {
            struct stat info {};
            const auto name = path.string() + suffix;
            if (!lstat(name.c_str(), &info))
                Require(S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
                        (info.st_mode & 0777) == 0600 && info.st_nlink == 1);
            else
                Require(errno == ENOENT);
        }
        try {
            Require(sqlite3_open_v2(path.c_str(), &db,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW |
                                        SQLITE_OPEN_FULLMUTEX,
                                    nullptr) == SQLITE_OK);
            sqlite3_busy_timeout(db, 3000);
            sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
            sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 2 * ClipboardArchive::maxArchiveBytes);
            Exec("PRAGMA trusted_schema=OFF; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
            Statement version(db, "PRAGMA user_version");
            Require(version.Next());
            const int schema = sqlite3_column_int(version.value, 0);
            Require(schema == 0 || schema == 1);
            if (!schema)
                Exec("BEGIN IMMEDIATE; CREATE TABLE history(id TEXT PRIMARY KEY,revision TEXT NOT "
                     "NULL,kind TEXT NOT NULL,title TEXT NOT NULL,preview TEXT NOT NULL,backend "
                     "TEXT NOT NULL,application TEXT NOT NULL,source TEXT NOT NULL,captured "
                     "INTEGER NOT NULL,bytes INTEGER NOT NULL,favorite INTEGER NOT NULL,insertable "
                     "INTEGER NOT NULL,search TEXT NOT NULL,checksum TEXT UNIQUE NOT NULL,payload "
                     "BLOB NOT NULL,filter TEXT NOT NULL); CREATE INDEX history_recent ON "
                     "history(captured DESC,id); "
                     "PRAGMA user_version=1; COMMIT;");
        } catch (...) {
            if (db)
                sqlite3_close(db);
            db = nullptr;
            throw;
        }
    }
    ~Impl() {
        if (db)
            sqlite3_close(db);
    }
};
ClipboardHistoryStore::ClipboardHistoryStore(const std::filesystem::path& root,
                                             ClipboardLimits limits)
    : impl_(std::make_unique<Impl>(root, limits)) {}
ClipboardHistoryStore::~ClipboardHistoryStore() = default;
std::filesystem::path ClipboardHistoryStore::DefaultRoot() {
    return ContentStore::DefaultRoot() / "clipboard";
}
ClipboardEntry ClipboardHistoryStore::Capture(const ClipboardArchive& archive,
                                              const ClipboardSource& source,
                                              const std::function<bool()>& current) {
    Require(!current || current());
    for (const auto& value : {source.backend, source.application, source.identity})
        Require(value.size() <= 512 && CharacterCount(value) <= 512);
    const auto payload = archive.Encode();
    const auto projection = ProjectClipboard(archive);
    const auto checksum = Sha256Hex(payload);
    Require(static_cast<std::int64_t>(payload.size()) <= impl_->limits.bytes);
    auto lock = impl_->files.Lock();
    Require(!current || current());
    impl_->Exec("BEGIN IMMEDIATE");
    try {
        std::string id = NewUuidV4();
        bool favorite = false;
        {
            Statement prior(impl_->db, "SELECT id,favorite FROM history WHERE checksum=?");
            prior.Bind(1, checksum);
            if (prior.Next()) {
                id = prior.String(0);
                favorite = sqlite3_column_int(prior.value, 1);
            }
        }
        const auto revision = Sha256Hex(runtime::RandomID() + checksum);
        const auto time = Now();
        Statement insert(impl_->db,
                         "INSERT OR REPLACE INTO history VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        insert.Bind(1, id);
        insert.Bind(2, revision);
        insert.Bind(3, projection.kind);
        insert.Bind(4, projection.title);
        insert.Bind(5, projection.preview);
        insert.Bind(6, source.backend);
        insert.Bind(7, source.application);
        insert.Bind(8, source.identity);
        insert.Number(9, time);
        insert.Number(10, payload.size());
        insert.Number(11, favorite);
        insert.Number(12, projection.insertable);
        insert.Bind(13, projection.search + "\n" + LowerText(source.application));
        insert.Bind(14, checksum);
        Require(sqlite3_bind_blob64(insert.value, 15, payload.data(), payload.size(),
                                    SQLITE_TRANSIENT) == SQLITE_OK);
        insert.Bind(16, projection.filter);
        insert.Next();
        for (;;) {
            Statement bounds(impl_->db, "SELECT COUNT(*),COALESCE(SUM(bytes),0) FROM history");
            Require(bounds.Next());
            if (sqlite3_column_int64(bounds.value, 0) <=
                    static_cast<std::int64_t>(impl_->limits.items) &&
                sqlite3_column_int64(bounds.value, 1) <= impl_->limits.bytes)
                break;
            Statement trim(impl_->db, "DELETE FROM history WHERE id=(SELECT id FROM history WHERE "
                                      "favorite=0 AND id!=? ORDER BY captured,id LIMIT 1)");
            trim.Bind(1, id);
            trim.Next();
            Require(sqlite3_changes(impl_->db) > 0); // All remaining entries are favorites:
                                                     // preserve them and reject the new capture.
        }
        const auto result = *Get(id);
        Require(!current || current());
        impl_->Exec("COMMIT");
        return result;
    } catch (...) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
std::vector<ClipboardEntry> ClipboardHistoryStore::List(std::string query, std::size_t limit,
                                                        std::size_t offset,
                                                        std::string filter) const {
    Require(limit <= 100000 && offset <= 100000 && query.size() <= 4096);
    std::string sql = "SELECT " + std::string(columns) + " FROM history WHERE 1";
    Require(filter == "all" || filter == "text" || filter == "image" || filter == "other");
    if (filter != "all")
        sql += " AND filter=\'" + filter + "\'";
    std::istringstream terms(OneLine(LowerText(query)));
    std::vector<std::string> tokens;
    std::string token;
    while (terms >> token) {
        Require(tokens.size() < 128);
        tokens.push_back(token);
        sql += " AND instr(search,?)>0";
    }
    sql += " ORDER BY captured DESC,id LIMIT ? OFFSET ?";
    Statement rows(impl_->db, sql.c_str());
    int at = 1;
    for (const auto& term : tokens)
        rows.Bind(at++, term);
    rows.Number(at++, limit);
    rows.Number(at, offset);
    std::vector<ClipboardEntry> result;
    while (rows.Next())
        result.push_back(Entry(rows));
    return result;
}
ClipboardPage ClipboardHistoryStore::Page(std::string query, std::string filter,
                                          int selected) const {
    Require(query.size() <= 4096 &&
            (filter == "all" || filter == "text" || filter == "image" || filter == "other"));
    // Count and page are one read snapshot. Opaque payloads never enter this query.
    impl_->Exec("BEGIN");
    try {
        std::string sql = "SELECT COUNT(*) FROM history WHERE 1";
        if (filter != "all")
            sql += " AND filter='" + filter + "'";
        std::istringstream terms(OneLine(LowerText(query)));
        std::vector<std::string> tokens;
        std::string token;
        while (terms >> token) {
            Require(tokens.size() < 128);
            tokens.push_back(token);
            sql += " AND instr(search,?)>0";
        }
        ClipboardPage page;
        {
            Statement count(impl_->db, sql.c_str());
            int at = 1;
            for (const auto& term : tokens)
                count.Bind(at++, term);
            Require(count.Next());
            page.count = sqlite3_column_int(count.value, 0);
            Require(page.count >= 0 && page.count <= 100000);
        }
        page.selected = std::clamp(selected, 0, std::max(0, page.count - 1));
        page.offset = std::max(0, page.selected - 100);
        page.entries = List(query, 200, page.offset, filter);
        impl_->Exec("COMMIT");
        return page;
    } catch (...) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
std::optional<ClipboardEntry> ClipboardHistoryStore::Get(const std::string& id) const {
    Require(LooksLikeUuid(id));
    const auto sql = "SELECT " + std::string(columns) + " FROM history WHERE id=?";
    Statement row(impl_->db, sql.c_str());
    row.Bind(1, id);
    if (!row.Next())
        return {};
    return Entry(row);
}
std::optional<ClipboardArchive> ClipboardHistoryStore::Read(const std::string& id,
                                                            const std::string& revision) const {
    Require(LooksLikeUuid(id));
    Statement row(impl_->db, "SELECT payload,checksum FROM history WHERE id=? AND revision=?");
    row.Bind(1, id);
    row.Bind(2, revision);
    if (!row.Next())
        return {};
    const auto size = sqlite3_column_bytes(row.value, 0);
    Require(size > 0 && static_cast<std::size_t>(size) <= ClipboardArchive::maxArchiveBytes);
    const auto* bytes = static_cast<const char*>(sqlite3_column_blob(row.value, 0));
    Require(bytes);
    const std::string payload(bytes, size);
    Require(Sha256Hex(payload) == row.String(1));
    return ClipboardArchive::Decode(payload);
}
bool ClipboardHistoryStore::Favorite(const std::string& id, const std::string& revision,
                                     bool value) {
    Require(LooksLikeUuid(id));
    auto lock = impl_->files.Lock();
    Statement update(impl_->db,
                     "UPDATE history SET favorite=?,revision=? WHERE id=? AND revision=?");
    update.Number(1, value);
    update.Bind(2, Sha256Hex(runtime::RandomID()));
    update.Bind(3, id);
    update.Bind(4, revision);
    update.Next();
    return sqlite3_changes(impl_->db) == 1;
}
bool ClipboardHistoryStore::Remove(const std::string& id, const std::string& revision) {
    Require(LooksLikeUuid(id));
    auto lock = impl_->files.Lock();
    Statement remove(impl_->db, "DELETE FROM history WHERE id=? AND revision=?");
    remove.Bind(1, id);
    remove.Bind(2, revision);
    remove.Next();
    return sqlite3_changes(impl_->db) == 1;
}
void ClipboardHistoryStore::ClearTemporary() {
    auto lock = impl_->files.Lock();
    impl_->Exec("DELETE FROM history WHERE favorite=0");
}
} // namespace rimes::capsule
