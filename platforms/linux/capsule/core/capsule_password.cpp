#include "capsule_hash.hpp"
#include "capsule_password.hpp"
#include "capsule_rich_text.hpp"
#include "capsule_structure.hpp"
#include "capsule_text.hpp"
#include "private_files.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>
#include <thread>
namespace rimes::capsule {
namespace {
using Json = nlohmann::json;
constexpr std::size_t limit = 1024 * 1024;
struct SecretGuard {
    std::string& value;
    ~SecretGuard() { EraseSecret(value); }
};
void Require(bool valid, const char* message) {
    if (!valid)
        throw std::runtime_error(message);
}
std::string ID(std::string id) {
    Require(LooksLikeUuid(id), "密码标识无效");
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return id;
}
std::string Random(std::size_t size) {
    std::string bytes(size, '\0');
    Require(RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), bytes.size()) == 1,
            "随机源不可用");
    return bytes;
}
std::vector<std::string> Files(const std::filesystem::path& root, bool* any = nullptr) {
    std::vector<std::string> files;
    std::size_t count = 0;
    for (const auto& file : std::filesystem::directory_iterator(root / "passwords")) {
        Require(++count <= 20000, "密码库记录数量超过上限");
        if (any)
            *any = true;
        if (file.path().extension() == ".md" && LooksLikeUuid(file.path().stem().string()))
            files.push_back(file.path().filename().string());
    }
    return files;
}
struct Document {
    PasswordSummary value;
    std::string ciphertext;
};
Document Parse(const std::string& text, const std::string& id) {
    Require(!text.empty() && text.size() <= limit && text.find('\0') == text.npos, "密码文档无效");
    std::istringstream stream(text);
    std::string line;
    Require(bool(std::getline(stream, line)) && line == "---", "密码文档格式无效");
    std::vector<std::string> lines;
    while (std::getline(stream, line) && line != "---")
        lines.push_back(line);
    Require(line == "---", "密码文档头缺失");
    const auto header = ParseFrontMatter(lines);
    Require(header.has_value(), "密码文档头无效");
    auto field = [&](const char* key) { return header->Scalar(key).value_or(""); };
    Require(field("capsule") == "password" && (field("version") == "1" || field("version") == "2"),
            "密码文档版本不支持");
    Document result;
    auto& r = result.value;
    r.id = ID(field("id"));
    r.title = field("title");
    r.summary = NormalizeSummary(field("summary"));
    r.category = field("category");
    if (r.category != "login" && r.category != "key")
        r.category = "other";
    r.updated_at = field("updated_at");
    r.revision = runtime::Sha256(text);
    Require(r.id == ID(id) && !r.title.empty() && CharacterCount(r.title) <= 256,
            "密码文档身份无效");
    std::tm time{};
    std::istringstream date(r.updated_at);
    date >> std::get_time(&time, "%Y-%m-%dT%H:%M:%S");
    Require(!date.fail() && r.updated_at.size() <= 40 && r.updated_at.back() == 'Z',
            "密码文档日期无效");
    bool found = false;
    while (std::getline(stream, line))
        if (line == "```capsule-password") {
            found = true;
            break;
        }
    Require(found && bool(std::getline(stream, result.ciphertext)) &&
                bool(std::getline(stream, line)) && line == "```",
            "密码密文块无效");
    result.ciphertext = TrimText(result.ciphertext);
    Require(!result.ciphertext.empty(), "密码密文缺失");
    return result;
}
Document ReadDocument(const std::filesystem::path& root, const std::string& id) {
    const auto text = runtime::PrivateDirectory(root / "passwords").Read(ID(id) + ".md", limit);
    Require(text.has_value(), "未找到密码记录");
    return Parse(*text, id);
}
std::string Key(const std::filesystem::path& root, bool create) {
    runtime::PrivateDirectory directory(root);
    if (auto key = directory.Read("master-key", 32)) {
        Require(key->size() == 32, "密码库主密钥格式无效");
        return *key;
    }
    bool any = false;
    Files(root, &any);
    Require(create && !any, "密码库主密钥缺失，已停止操作");
    auto key = Random(32);
    directory.Write("master-key", key);
    return key;
}
std::string Aad(const PasswordSummary& r) {
    return "capsule-password-v1\n" + r.id + "\n" + r.title;
}
std::string Crypt(const std::string& key, const std::string& data, const std::string& aad,
                  bool encrypt) {
    using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    Context ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    Require(ctx != nullptr && key.size() == 32 && data.size() <= limit &&
                (encrypt || data.size() >= 28),
            "密码加解密参数无效");
    const auto nonce = encrypt ? Random(12) : data.substr(0, 12);
    const auto* input = reinterpret_cast<const unsigned char*>(data.data()) + (encrypt ? 0 : 12);
    const auto bytes = static_cast<int>(data.size() - (encrypt ? 0 : 28));
    std::string output(data.size() + 32, '\0');
    SecretGuard clear{output};
    int written = 0, tail = 0;
    auto* dst = reinterpret_cast<unsigned char*>(output.data());
    Require(EVP_CipherInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr,
                              reinterpret_cast<const unsigned char*>(key.data()),
                              reinterpret_cast<const unsigned char*>(nonce.data()), encrypt) == 1 &&
                EVP_CipherUpdate(ctx.get(), nullptr, &written,
                                 reinterpret_cast<const unsigned char*>(aad.data()),
                                 aad.size()) == 1 &&
                EVP_CipherUpdate(ctx.get(), dst, &written, input, bytes) == 1,
            "密码加解密失败");
    if (!encrypt)
        Require(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, 16,
                                    const_cast<char*>(data.data() + data.size() - 16)) == 1,
                "密码认证失败");
    Require(EVP_CipherFinal_ex(ctx.get(), dst + written, &tail) == 1,
            "密码解密失败，记录可能损坏或被修改");
    output.resize(written + tail);
    if (!encrypt)
        return std::string(output);
    std::array<char, 16> tag{};
    Require(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, tag.size(), tag.data()) == 1,
            "密码认证失败");
    return nonce + output + std::string(tag.data(), tag.size());
}
bool IsLegacy(const Json& j) {
    if (!j.is_object() || !j.contains("password") || !j.at("password").is_string() ||
        !j.contains("previousPasswords") || !j.at("previousPasswords").is_array())
        return false;
    for (const auto* field : {"url", "app", "username"})
        if (j.contains(field) && !j.at(field).is_null() && !j.at(field).is_string())
            return false;
    return std::all_of(j.at("previousPasswords").begin(), j.at("previousPasswords").end(),
                       [](const auto& value) { return value.is_string(); });
}
std::string Legacy(const Json& j) {
    std::string result;
    for (auto pair : {std::pair<const char*, const char*>{"url", "网址"},
                      {"app", "App"},
                      {"username", "用户名"}})
        if (j.contains(pair.first) && j.at(pair.first).is_string() &&
            !j.at(pair.first).get<std::string>().empty())
            result +=
                "- " + std::string(pair.second) + "：" + j.at(pair.first).get<std::string>() + "\n";
    result += "- 密码：" + j.at("password").get<std::string>();
    if (j.contains("previousPasswords")) {
        std::size_t index = 0;
        for (const auto& value : j.at("previousPasswords")) {
            Require(++index <= 100 && value.is_string(), "旧密码记录无效");
            if (!value.get<std::string>().empty())
                result += "\n- 曾用密码 " + std::to_string(index) + "：" + value.get<std::string>();
        }
    }
    return result;
}
} // namespace
void CommitSecret(const SecretCommit& gate, const std::function<void()>& action) {
    bool committed = false;
    auto publish = [&] {
        Require(!committed, "重复的密码库操作");
        action();
        committed = true;
    };
    if (gate)
        gate(publish);
    else
        publish();
    Require(committed, "授权已过期或窗口已关闭，请重新验证");
}
void EraseSecret(std::string& value) {
    if (!value.empty())
        OPENSSL_cleanse(value.data(), value.size());
    value.clear();
}
PasswordStore::PasswordStore(std::filesystem::path root) : root_(std::move(root)) {
    runtime::PrivateDirectory directory(root_), records(root_ / "passwords");
}
std::vector<PasswordSummary> PasswordStore::List() const {
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock(false);
    std::vector<PasswordSummary> result;
    for (const auto& name : Files(root_)) {
        try {
            result.push_back(
                ReadDocument(root_, std::filesystem::path(name).stem().string()).value);
        } catch (const std::exception&) { /* Isolate damaged records without opening secrets. */
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.title == b.title ? a.id < b.id : a.title < b.title;
    });
    return result;
}
std::vector<PasswordSummary>
PasswordStore::ListWhileActive(const std::atomic<bool>& cancelled) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
        if (cancelled.load())
            throw std::runtime_error("密码库读取已取消");
        try {
            auto values = List();
            if (cancelled.load())
                throw std::runtime_error("密码库读取已取消");
            return values;
        } catch (const runtime::StorageBusy&) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}
PasswordSummary PasswordStore::Inspect(const std::string& id) const {
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock(false);
    return ReadDocument(root_, id).value;
}
void PasswordStore::WithAuthorization(const std::string& id, const std::string& revision,
                                      const std::string& credential,
                                      const std::function<void()>& action) const {
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock(false);
    Require(!credential.empty() && RevealCredential(root_).Revision() == credential,
            "查看口令已变化，请重新验证");
    if (!id.empty())
        Require(ReadDocument(root_, id).value.revision == revision, "密码记录已变化，请重新选择");
    action();
}
PasswordRecord PasswordStore::Read(const std::string& id, const std::string& expected,
                                   const std::string& credential) const {
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock(false);
    Require(credential.empty() || RevealCredential(root_).Revision() == credential,
            "查看口令已变化，请重新验证");
    const auto document = ReadDocument(root_, id);
    Require(document.value.revision == expected, "密码记录已变化，请重新选择");
    auto key = Key(root_, false);
    SecretGuard clear_key{key};
    auto combined = Base64Decode(document.ciphertext);
    Require(combined.has_value(), "密码密文格式无效");
    auto data = Crypt(key, *combined, Aad(document.value), false);
    SecretGuard clear{data};
    PasswordRecord result;
    try {
        const auto json = Json::parse(data);
        static_cast<PasswordSummary&>(result) = document.value;
        // Native reads a decodable v1 record before considering v2, even with a body key.
        result.body = IsLegacy(json) ? Legacy(json) : json.at("body").get<std::string>();
        Require(!TrimText(result.body).empty() && result.body.find('\0') == result.body.npos &&
                    CharacterCount(result.body) <= 65536,
                "密码正文无效");
        return result;
    } catch (const Json::exception&) {
        throw std::runtime_error("密码正文格式无效");
    }
}
PasswordSummary PasswordStore::Put(const PasswordRecord& request, const std::string& expected,
                                   const std::string& credential, const SecretCommit& gate) {
    PasswordSummary value = request;
    value.title = TrimText(value.title);
    value.summary = NormalizeSummary(value.summary);
    Require(!value.title.empty() && value.title.find('\0') == value.title.npos &&
                CharacterCount(value.title) <= 256 && !TrimText(request.body).empty() &&
                request.body.find('\0') == request.body.npos &&
                CharacterCount(request.body) <= 65536 &&
                (value.category == "login" || value.category == "key" || value.category == "other"),
            "密码标题、正文或分类无效");
    const auto lowered = LowerText(value.summary);
    for (const auto& row : FlattenSections(NoteSections(request.body)))
        if (row.type == "field" && (row.secret || ParseTotp(row.label, row.value)) &&
            CharacterCount(row.value) >= 3)
            Require(lowered.find(LowerText(row.value)) == lowered.npos, "摘要不能包含敏感字段的值");
    runtime::PrivateDirectory directory(root_), records(root_ / "passwords");
    auto lock = directory.Lock(false);
    Require(credential.empty() || RevealCredential(root_).Revision() == credential,
            "查看口令已变化，请重新验证");
    if (value.id.empty()) {
        Require(expected.empty() && Files(root_).size() < 20000, "密码记录数量超过上限");
        value.id = NewUuidV4();
    } else {
        value.id = ID(value.id);
        Require(ReadDocument(root_, value.id).value.revision == expected,
                "密码记录已被其他窗口更新");
    }
    auto key = Key(root_, true);
    SecretGuard clear_key{key};
    auto secret = Json{{"body", request.body}}.dump();
    SecretGuard clear{secret};
    const auto ciphertext = Base64Encode(Crypt(key, secret, Aad(value), true));
    value.updated_at = Iso8601UtcNow();
    const auto text = "---\ncapsule: password\nversion: 2\nid: " + JsonQuote(value.id) +
                      "\ntitle: " + JsonQuote(value.title) +
                      (value.summary.empty() ? "" : "\nsummary: " + JsonQuote(value.summary)) +
                      "\ncategory: " + JsonQuote(value.category) +
                      "\nupdated_at: " + JsonQuote(value.updated_at) +
                      "\n---\n\n<!-- RIMES Capsule encrypted password record. Do not edit the "
                      "encrypted block manually. -->\n```capsule-password\n" +
                      ciphertext + "\n```\n";
    Require(text.size() <= limit, "密码文档超过大小上限");
    value.revision = runtime::Sha256(text);
    CommitSecret(gate, [&] { records.Write(value.id + ".md", text); });
    return value;
}
void PasswordStore::Remove(const std::string& id, const std::string& expected,
                           const std::string& credential, const SecretCommit& gate) {
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock(false);
    Require(credential.empty() || RevealCredential(root_).Revision() == credential,
            "查看口令已变化，请重新验证");
    const auto value = ReadDocument(root_, id).value;
    Require(value.revision == expected, "密码记录已被其他窗口更新");
    CommitSecret(gate,
                 [&] { runtime::PrivateDirectory(root_ / "passwords").Remove(value.id + ".md"); });
}
PasswordSummary PasswordStore::MigrateNote(const std::string& id, const std::string& revision,
                                           const std::string& category,
                                           const std::string& credential,
                                           const SecretCommit& gate) {
    ContentStore notes(root_);
    Record note;
    std::string error;
    Require(notes.Get(id, &note, &error) == StoreStatus::Ok && note.kind == Kind::Note &&
                note.revision == revision,
            "笔记已变化，无法迁移");
    SecretGuard clear_note{note.content};
    PasswordRecord secret;
    secret.title = note.title;
    secret.body = note.content;
    // A note summary may disclose its contents; macOS deliberately drops it.
    secret.summary.clear();
    secret.category = category;
    auto saved = Put(secret, "", credential, gate);
    const auto remove = [&](const std::function<void()>& action) {
        // ContentStore holds the same vault root lock here. Credential changes
        // cannot race the final deletion, and cancellation is checked again.
        Require(credential.empty() || RevealCredential(root_).Revision() == credential,
                "查看口令已变化，请重新验证");
        CommitSecret(gate, action);
    };
    if (notes.Remove(id, revision, &error, remove) != StoreStatus::Ok) {
        Remove(saved.id, saved.revision);
        throw std::runtime_error("迁移已取消或笔记发生变化，已保留原文并撤销迁移");
    }
    return saved;
}
Card PasswordCard(const PasswordSummary& value) {
    Card card;
    card.id = value.id;
    card.revision = value.revision;
    card.title = value.title;
    card.module = Module::Passwords;
    card.kind = Kind::Password;
    card.filter = value.category;
    card.preview = value.summary.empty() ? "••••••••" : value.summary;
    return card;
}
} // namespace rimes::capsule
