#include "capsule_hash.hpp"
#include "capsule_text.hpp"
#include "mailbox_store.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
namespace rimes::mailbox {
namespace {
using capsule::CharacterCount;
using capsule::TrimText;
void Need(bool valid, const char* message = "Mailbox 数据文件已损坏") {
    if (!valid)
        throw std::runtime_error(message);
}
bool Optional(const Json& object, const char* field) {
    return !object.contains(field) || object.at(field).is_null();
}
void Object(const Json& value) { Need(value.is_object()); }
std::string Text(const Json& value, std::size_t maximum) {
    Need(value.is_string());
    const auto result = value.get<std::string>();
    Need(result.find('\0') == result.npos && !TrimText(result).empty() &&
         CharacterCount(result) <= maximum);
    return result;
}
void OptionalText(const Json& object, const char* field, std::size_t maximum) {
    if (!Optional(object, field))
        Text(object.at(field), maximum);
}
void ID(Json& value) {
    Need(value.is_string() && capsule::LooksLikeUuid(value.get<std::string>()));
    auto id = value.get<std::string>();
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    value = id;
}
void Date(const Json& value) { Need(value.is_number() && std::isfinite(value.get<double>())); }
void OptionalDate(const Json& object, const char* field) {
    if (!Optional(object, field))
        Date(object.at(field));
}
bool Is(const std::string& value, std::initializer_list<const char*> choices) {
    return std::any_of(choices.begin(), choices.end(),
                       [&](const char* choice) { return value == choice; });
}
bool AI(const Json& source) {
    return Is(source.at("kind"), {"codexCLI", "claudeCodeCLI", "openAICompatible"});
}
void Source(Json& source) {
    Object(source);
    const auto kind = Text(source.at("kind"), 32);
    Need(Is(kind, {"codexCLI", "claudeCodeCLI", "openAICompatible", "mcp", "http", "sse", "ssh",
                   "plugin", "other"}));
    Text(source.at("displayName"), 512);
    OptionalText(source, "identifier", 512);
    OptionalText(source, "model", 512);
    Need(source.at("replyCapability") == (AI(source) ? "aiContinuation" : "localNotesOnly"));
    if (!Optional(source, "providerRoute")) {
        auto& route = source.at("providerRoute");
        Object(route);
        Need(kind == "openAICompatible" && route.at("profileRevision").is_number_integer() &&
             (route.at("profileRevision").is_number_unsigned() ||
              route.at("profileRevision").get<std::int64_t>() > 0) &&
             route.at("profileRevision").get<std::uint64_t>() > 0);
        ID(route.at("profileID"));
        ID(route.at("routeID"));
    }
}
void Message(Json& message, const Limits& limits) {
    Object(message);
    ID(message.at("id"));
    Text(message.at("body"), limits.characters);
    OptionalText(message, "author", 128);
    Date(message.at("createdAt"));
    if (!message.contains("kind") || message["kind"].is_null())
        message["kind"] = "content";
    if (!message.contains("format") || message["format"].is_null())
        message["format"] = "plain";
    const auto kind = Text(message.at("kind"), 16), role = Text(message.at("role"), 16);
    Need(Is(role, {"inbound", "user", "system"}));
    Need((kind == "content" && role != "system") || (kind == "localNote" && role == "user") ||
         (kind == "status" && role == "system"));
    Need(message.at("format").is_string());
    auto format = message.at("format").get<std::string>();
    if (!Is(format, {"plain", "markdown", "json"}))
        message["format"] = "plain";
    if (!Optional(message, "imageFileName")) {
        const auto name = Text(message.at("imageFileName"), 40);
        Need(name.size() == 40 && name.substr(36) == ".png" &&
             capsule::LooksLikeUuid(name.substr(0, 36)));
    }
}
Json NewMessage(const char* role, const char* kind, const std::string& body, double date) {
    return {{"id", capsule::NewUuidV4()}, {"role", role}, {"kind", kind},
            {"format", "plain"},          {"body", body}, {"createdAt", date}};
}
Json& Find(Json& document, const std::string& id) {
    for (auto& thread : document.at("threads"))
        if (thread.at("id") == id)
            return thread;
    throw std::runtime_error("Mailbox 会话不存在");
}
void Expected(const Json& thread, const std::string& expected) {
    Need(!expected.empty() && expected == Revision(thread), "Mailbox 会话已被更新，请刷新后重试");
}
} // namespace

double Now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
               .count() -
           978307200.0;
}
void Validate(Json& document, const Limits& limits) {
    try {
        Object(document);
        Need(limits.threads && limits.messages && limits.characters && limits.bytes);
        Need(document.at("schemaVersion").is_number_integer() && document.at("schemaVersion") == 1,
             "Mailbox 数据版本不受支持");
        Need(document.at("threads").is_array() && document.at("threads").size() <= limits.threads);
        Need(document.at("nextSequence").is_number_integer() &&
             document.at("nextSequence").get<std::int64_t>() > 0);
        std::int64_t maximum = 0;
        std::set<std::string> threads, messages;
        std::set<std::int64_t> sequences;
        for (auto& thread : document.at("threads")) {
            Object(thread);
            ID(thread.at("id"));
            Need(threads.insert(thread.at("id")).second);
            Need(thread.at("sequence").is_number_integer());
            const auto sequence = thread.at("sequence").get<std::int64_t>();
            Need(sequence > 0 && sequences.insert(sequence).second);
            maximum = std::max(maximum, sequence);
            Source(thread.at("source"));
            OptionalText(thread, "title", 256);
            Date(thread.at("createdAt"));
            Date(thread.at("updatedAt"));
            OptionalDate(thread, "archivedAt");
            Need(thread.at("unread").is_boolean());
            if (!Optional(thread, "workspace"))
                Need(Is(Text(thread.at("workspace"), 16), {"chat", "inbox"}));
            if (!Optional(thread, "terminalSessionID"))
                ID(thread.at("terminalSessionID"));
            Need(thread.at("messages").is_array() &&
                 thread.at("messages").size() <= limits.messages);
            for (auto& message : thread.at("messages")) {
                Message(message, limits);
                Need(messages.insert(message.at("id")).second);
            }
            if (!Optional(thread, "generation")) {
                auto& generation = thread.at("generation");
                Object(generation);
                ID(generation.at("id"));
                Date(generation.at("startedAt"));
                OptionalDate(generation, "finishedAt");
                OptionalText(generation, "failureMessage", 2048);
                if (!Optional(generation, "expectedFormat"))
                    Need(Is(Text(generation.at("expectedFormat"), 64),
                            {"plain", "markdown", "json"}));
                const auto phase = Text(generation.at("phase"), 16);
                if (phase == "generating")
                    Need(Optional(generation, "finishedAt") &&
                         Optional(generation, "failureMessage") &&
                         thread.at("messages").size() < limits.messages);
                else if (phase == "succeeded")
                    Need(!Optional(generation, "finishedAt") &&
                         Optional(generation, "failureMessage"));
                else if (phase == "failed")
                    Need(!Optional(generation, "finishedAt") &&
                         !Optional(generation, "failureMessage"));
                else
                    Need(false);
            }
            if (!Optional(thread, "review")) {
                Need(!AI(thread.at("source")));
                auto& review = thread.at("review");
                Object(review);
                ID(review.at("messageID"));
                const auto& rows = thread.at("messages");
                Need(std::any_of(rows.begin(), rows.end(), [&](const auto& message) {
                    return message.at("id") == review.at("messageID") &&
                           message.at("role") == "inbound" && message.at("kind") == "content";
                }));
                const auto state = Text(review.at("state"), 16);
                OptionalDate(review, "resolvedAt");
                Need((state == "pending" && Optional(review, "resolvedAt")) ||
                     (Is(state, {"accepted", "rejected"}) && !Optional(review, "resolvedAt")));
            }
        }
        Need(document.at("nextSequence").get<std::int64_t>() > maximum);
    } catch (const Json::exception&) {
        throw std::runtime_error("Mailbox 数据格式无效");
    }
}
std::string Module(const Json& thread) {
    if (!Optional(thread, "workspace"))
        return thread.at("workspace");
    if (thread.at("source").at("replyCapability") == "localNotesOnly")
        return "inbox";
    if (!Optional(thread, "title") &&
        thread.at("title").get<std::string>().rfind("图片 · ", 0) == 0)
        return "inbox";
    for (const auto& message : thread.at("messages"))
        if (!Optional(message, "imageFileName"))
            return "inbox";
    return "chat";
}
std::string Title(const Json& thread) {
    if (!Optional(thread, "title"))
        return thread.at("title");
    std::string text = thread.at("source").at("displayName");
    for (const auto& message : thread.at("messages"))
        if (message.at("kind") == "content") {
            text = message.at("body");
            break;
        }
    const auto start = text.find_first_not_of("\r\n");
    text =
        start == text.npos ? text : text.substr(start, text.find_first_of("\r\n", start) - start);
    return capsule::CharacterPrefix(text, 44);
}
std::string Preview(const Json& thread) {
    const auto& messages = thread.at("messages");
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
        if (it->at("kind") != "status")
            return it->at("body");
    return messages.empty() ? "" : messages.back().at("body").get<std::string>();
}
std::string Status(const Json& thread) {
    if (!Optional(thread, "linuxBufferTransfer"))
        return "正在加入 Buffer";
    if (!Optional(thread, "archivedAt"))
        return "已归档";
    if (!Optional(thread, "generation")) {
        if (thread.at("generation").at("phase") == "generating")
            return "生成中";
        if (thread.at("generation").at("phase") == "failed")
            return "请求中断";
    }
    if (!Optional(thread, "review") && thread.at("review").at("state") == "pending")
        return "待处理";
    return "已保存";
}
std::vector<Json> Filter(const Snapshot& state, const std::string& module, const std::string& query,
                         const std::string& filter) {
    std::vector<Json> result;
    const auto needle = capsule::LowerText(TrimText(query));
    for (const auto& thread : state.document.at("threads")) {
        if (Module(thread) != module)
            continue;
        const bool archived = !Optional(thread, "archivedAt");
        if ((filter == "archived") != archived)
            continue;
        if (filter == "unread" && !thread.at("unread").get<bool>())
            continue;
        if (filter == "attention" &&
            !((!Optional(thread, "review") && thread.at("review").at("state") == "pending") ||
              (!Optional(thread, "generation") && thread.at("generation").at("phase") == "failed")))
            continue;
        auto haystack =
            Title(thread) + "\n" + thread.at("source").at("displayName").get<std::string>();
        for (const auto& message : thread.at("messages"))
            haystack += "\n" + message.at("body").get<std::string>();
        if (!needle.empty() && capsule::LowerText(haystack).find(needle) == std::string::npos)
            continue;
        result.push_back(thread);
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.at("updatedAt") == b.at("updatedAt"))
            return a.at("sequence").template get<std::int64_t>() >
                   b.at("sequence").template get<std::int64_t>();
        return a.at("updatedAt").template get<double>() > b.at("updatedAt").template get<double>();
    });
    return result;
}
std::size_t Unread(const Snapshot& state) {
    const auto rows = Filter(state, "inbox", "", "unread");
    return rows.size();
}
std::string Revision(const Json& thread) { return runtime::Sha256(thread.dump()); }
Store::Store(std::filesystem::path root, Limits limits, Commit commit)
    : files_(root), limits_(limits), commit_(std::move(commit)) {}
Snapshot Store::Load() const {
    const auto raw = files_.Read("mailbox.json", limits_.bytes);
    Json document;
    try {
        document =
            raw ? Json::parse(*raw)
                : Json{{"schemaVersion", 1}, {"nextSequence", 1}, {"threads", Json::array()}};
    } catch (const Json::exception&) {
        throw std::runtime_error("Mailbox 数据文件已损坏，未覆盖原文件");
    }
    Validate(document, limits_);
    return {document, runtime::Sha256(raw.value_or(""))};
}
Snapshot Store::Read() const {
    // One atomic JSON publication is already a complete snapshot. Readers do
    // not take the writer lock, so viewing a receipt cannot make a save fail.
    // Referenced images are persisted before that publication; a concurrent
    // deletion may remove its original, which image readers handle as unavailable.
    return Load();
}
Json Store::Get(const std::string& id) const {
    auto state = Read();
    return Find(state.document, id);
}
void Store::Save(Json document) const {
    Validate(document, limits_);
    const auto raw = document.dump(2);
    Need(raw.size() + 1 <= limits_.bytes, "Mailbox 已达到本地容量上限");
    bool saved = false;
    const auto publish = [&] {
        Need(!saved, "Mailbox 重复提交");
        try {
            files_.Write("mailbox.json", raw + "\n");
        } catch (...) {
            // Directory fsync may fail after rename. Once the exact document is
            // visible, do not roll back its attachment or report a cancellation.
            if (files_.Read("mailbox.json", limits_.bytes) !=
                std::optional<std::string>(raw + "\n"))
                throw;
        }
        saved = true;
    };
    if (commit_)
        commit_(publish);
    else
        publish();
    Need(saved, "Mailbox 操作已取消");
}
Json Store::Create(const Json& source, const std::string& title, const std::string& body,
                   const std::string& module, const std::string& png) {
    return CreateReceipt(source, title, body, module, png, "plain", nullptr);
}
Json Store::ReceiveGateway(const Json& source, const std::string& title, const std::string& body,
                           const std::string& format, const std::string& nonce,
                           const std::string& digest) {
    Need(source.at("kind") == "http" || source.at("kind") == "mcp", "无效的网关来源");
    Need(!TrimText(body).empty() && capsule::CharacterCount(body) <= 20000 &&
             body.size() <= 256 * 1024,
         "收件正文为空或超过上限");
    Need(format == "plain" || format == "markdown" || format == "json", "无效的正文格式");
    Need((nonce.empty() && digest.empty()) ||
             (nonce.size() == 64 && digest.size() == 64 &&
              nonce.find_first_not_of("0123456789abcdef") == nonce.npos &&
              digest.find_first_not_of("0123456789abcdef") == digest.npos),
         "无效的请求标识");
    return CreateReceipt(source, title, body, "inbox", "", format,
                         Json{{"nonce", nonce}, {"digest", digest}});
}
Json Store::CreateReceipt(const Json& source, const std::string& title, const std::string& body,
                          const std::string& module, const std::string& png,
                          const std::string& format, const Json& gateway) {
    auto lock = files_.Lock(false);
    auto state = Load();
    auto& document = state.document;
    if (!gateway.is_null()) {
        std::size_t count = 0, characters = 0;
        for (const auto& row : document.at("threads")) {
            if (!gateway.at("nonce").get<std::string>().empty() &&
                row.value("linuxGateway", Json::object()).value("nonce", "") ==
                    gateway.at("nonce")) {
                Need(row.at("linuxGateway").at("digest") == gateway.at("digest"),
                     "请求标识已用于不同内容");
                return row;
            }
            if (!Optional(row, "review") && row.at("review").at("state") == "pending") {
                ++count;
                for (const auto& item : row.at("messages"))
                    if (item.at("id") == row.at("review").at("messageID"))
                        characters += capsule::CharacterCount(item.at("body").get<std::string>());
            }
        }
        Need(count < 50 && characters + capsule::CharacterCount(body) <= 1048576,
             "待审核收件已达到上限");
    }
    Need(document.at("nextSequence").get<std::int64_t>() < std::numeric_limits<std::int64_t>::max(),
         "Mailbox 会话序号超过上限");
    Need(!AI(source) && module == "inbox", "单向来源只能创建收件");
    const auto date = Now();
    Json thread{{"id", capsule::NewUuidV4()},
                {"sequence", document.at("nextSequence")},
                {"source", source},
                {"workspace", module},
                {"messages", Json::array({NewMessage("inbound", "content", body, date)})},
                {"unread", true},
                {"createdAt", date},
                {"updatedAt", date}};
    thread["messages"][0]["author"] = source.at("displayName");
    thread["messages"][0]["format"] = format;
    if (!gateway.is_null())
        thread["linuxGateway"] = gateway;
    thread["review"] = {{"messageID", thread.at("messages").at(0).at("id")}, {"state", "pending"}};
    if (!TrimText(title).empty())
        thread["title"] = title;
    document["nextSequence"] = document.at("nextSequence").get<std::int64_t>() + 1;
    std::string attachment;
    if (!png.empty()) {
        Need(png.size() >= 24 && png.size() <= 24 * 1048576 &&
                 png.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0,
             "Mailbox 仅接收不超过 24 MiB 的 PNG 附件");
        attachment = capsule::NewUuidV4() + ".png";
        runtime::PrivateDirectory(files_.Path() / "images").Write(attachment, png);
        thread["messages"][0]["imageFileName"] = attachment;
    }
    try {
        document.at("threads").push_back(thread);
        Save(document);
    } catch (...) {
        if (!attachment.empty())
            runtime::PrivateDirectory(files_.Path() / "images").Remove(attachment);
        throw;
    }
    return thread;
}
std::optional<std::string> Store::Image(const std::string& filename) const {
    Need(filename.size() == 40 && filename.substr(36) == ".png" &&
             capsule::LooksLikeUuid(filename.substr(0, 36)),
         "Mailbox 附件名称无效");
    const auto bytes =
        runtime::PrivateDirectory(files_.Path() / "images").Read(filename, 24 * 1048576);
    if (bytes)
        Need(bytes->size() >= 24 && bytes->compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0,
             "Mailbox 附件格式无效");
    return bytes;
}

Json Store::Change(const std::string& id, const std::string& expected,
                   const std::function<void(Json&)>& change) {
    auto lock = files_.Lock(false);
    auto state = Load();
    auto& thread = Find(state.document, id);
    Expected(thread, expected);
    change(thread);
    Save(state.document);
    return thread;
}
Json Store::AddNote(const std::string& id, const std::string& expected, const std::string& body) {
    return Change(id, expected, [&](Json& thread) {
        auto message = NewMessage("user", "localNote", body, Now());
        message["author"] = "你";
        thread.at("messages").push_back(message);
        thread["updatedAt"] = Now();
    });
}
Json Store::Rename(const std::string& id, const std::string& expected, const std::string& title) {
    return Change(id, expected, [&](Json& thread) {
        const auto normalized = TrimText(title);
        if (normalized.empty())
            thread.erase("title");
        else
            thread["title"] = normalized;
    });
}
Json Store::Archive(const std::string& id, const std::string& expected, bool archived) {
    return Change(id, expected, [&](Json& thread) {
        if (archived)
            thread["archivedAt"] = Now();
        else
            thread.erase("archivedAt");
    });
}
Json Store::MarkRead(const std::string& id, const std::string& expected) {
    return Change(id, expected, [&](Json& thread) { thread["unread"] = false; });
}
void Store::WithMessage(
    const std::string& id, const std::string& expected, const std::string& message,
    const std::function<void(const Json&, const Json&, const Commit&)>& action) const {
    auto lock = files_.Lock(false);
    auto state = Load();
    const auto& thread = Find(state.document, id);
    Expected(thread, expected);
    Need(Optional(thread, "generation") || thread.at("generation").at("phase") != "generating",
         "请等待消息生成完成");
    const auto& messages = thread.at("messages");
    const auto found = std::find_if(messages.begin(), messages.end(), [&](const auto& row) {
        return row.at("id") == message && row.at("role") == "inbound" &&
               row.at("kind") == "content";
    });
    Need(found != messages.end(), "收件消息已变化或不存在");
    bool applied = false;
    const Commit gate = [&](const std::function<void()>& write) {
        const auto publish = [&] {
            Need(!applied, "Mailbox 重复操作");
            write();
            applied = true;
        };
        if (commit_)
            commit_(publish);
        else
            publish();
    };
    action(thread, *found, gate);
    Need(applied, "Mailbox 操作已取消");
}
Json Store::ResolveReview(const std::string& id, const std::string& expected,
                          const std::string& message, bool accept) {
    return Change(id, expected, [&](Json& thread) {
        Need(!Optional(thread, "review") && thread.at("review").at("state") == "pending" &&
                 thread.at("review").at("messageID") == message,
             "这条外部消息已处理或不可用");
        Need(Optional(thread, "generation") || thread.at("generation").at("phase") != "generating",
             "请等待消息生成完成");
        const auto date = Now();
        thread["review"]["state"] = accept ? "accepted" : "rejected";
        thread["review"]["resolvedAt"] = date;
        auto status = NewMessage("system", "status",
                                 accept ? "已接受并加入 Buffer。" : "已拒绝该外部推送。", date);
        status["author"] = "Mailbox";
        thread.at("messages").push_back(status);
        thread["updatedAt"] = date;
    });
}
Json Store::BeginBufferTransfer(const std::string& id, const std::string& expected,
                                const std::string& message, const std::string& transfer) {
    Need(capsule::LooksLikeUuid(transfer), "无效的接收标识");
    return Change(id, expected, [&](Json& thread) {
        Need(Optional(thread, "linuxBufferTransfer"), "这条收件正在加入 Buffer");
        Need(Optional(thread, "generation") || thread.at("generation").at("phase") != "generating",
             "消息尚未完成");
        const auto& messages = thread.at("messages");
        const auto found = std::find_if(messages.begin(), messages.end(),
                                        [&](const Json& row) { return row.at("id") == message; });
        Need(found != messages.end() && found->at("kind") == "content" &&
                 found->at("role") == "inbound" && Optional(*found, "imageFileName"),
             "消息无法加入 Buffer");
        Json journal{{"id", transfer},
                     {"messageID", message},
                     {"previousUpdatedAt", thread.at("updatedAt")}};
        if (!Optional(thread, "review")) {
            Need(thread.at("review").at("state") == "pending" &&
                     thread.at("review").at("messageID") == message,
                 "这条外部消息已处理");
            const auto date = Now();
            journal["previousReview"] = thread.at("review");
            thread["review"]["state"] = "accepted";
            thread["review"]["resolvedAt"] = date;
            journal["acceptedReview"] = thread.at("review");
            auto status = NewMessage("system", "status", "已接受并加入 Buffer。", date);
            status["author"] = "Mailbox";
            journal["status"] = status;
            thread.at("messages").push_back(status);
            thread["updatedAt"] = date;
        }
        thread["linuxBufferTransfer"] = journal;
    });
}
void Store::FinishBufferTransfer(const std::string& id, const std::string& transfer,
                                 bool accepted) {
    auto lock = files_.Lock(false);
    auto state = Load();
    auto& threads = state.document.at("threads");
    auto found = std::find_if(threads.begin(), threads.end(),
                              [&](const Json& row) { return row.at("id") == id; });
    if (found == threads.end() || Optional(*found, "linuxBufferTransfer"))
        return;
    auto& row = *found;
    const auto journal = row.at("linuxBufferTransfer");
    Need(journal.at("id") == transfer, "接收操作已变化");
    if (!accepted && journal.contains("previousReview")) {
        // Undo only this transfer's review/status; retain concurrently appended
        // annotations, renamed titles, archives and their newer timestamps.
        Need(row.at("review") == journal.at("acceptedReview"), "审核状态已变化");
        auto& messages = row.at("messages");
        const auto& status = journal.at("status");
        const auto own = std::find(messages.begin(), messages.end(), status);
        Need(own != messages.end(), "审核状态已变化");
        messages.erase(own);
        row["review"] = journal.at("previousReview");
        if (row.at("updatedAt") == status.at("createdAt"))
            row["updatedAt"] = journal.at("previousUpdatedAt");
    }
    row.erase("linuxBufferTransfer");
    Save(state.document);
}
void Store::RecoverBufferTransfers() {
    const auto state = Read();
    for (const auto& thread : state.document.at("threads"))
        if (!Optional(thread, "linuxBufferTransfer"))
            FinishBufferTransfer(thread.at("id"), thread.at("linuxBufferTransfer").at("id"), false);
}
void Store::Remove(const std::string& id, const std::string& expected) {
    auto lock = files_.Lock(false);
    auto state = Load();
    const auto& removed = Find(state.document, id);
    Expected(removed, expected);
    std::set<std::string> images;
    for (const auto& message : removed.at("messages"))
        if (!Optional(message, "imageFileName"))
            images.insert(message.at("imageFileName"));
    auto& threads = state.document.at("threads");
    threads.erase(std::remove_if(threads.begin(), threads.end(),
                                 [&](const auto& thread) { return thread.at("id") == id; }),
                  threads.end());
    Save(state.document);
    // Native deletion removes only unreferenced Mailbox originals, after the
    // document is durable. Independently saved Capsule assets are never touched.
    for (const auto& thread : threads)
        for (const auto& message : thread.at("messages"))
            if (!Optional(message, "imageFileName"))
                images.erase(message.at("imageFileName"));
    for (const auto& image : images) {
        try {
            runtime::PrivateDirectory(files_.Path() / "images").Remove(image);
        } catch (const std::exception&) { /* A failed cleanup cannot undo the durable deletion. */
        }
    }
}
bool Store::RecoverInterrupted() {
    auto lock = files_.Lock(false);
    auto state = Load();
    bool changed = false;
    for (auto& thread : state.document.at("threads"))
        if (!Optional(thread, "generation") &&
            thread.at("generation").at("phase") == "generating") {
            auto& generation = thread.at("generation");
            generation["phase"] = "failed";
            generation["finishedAt"] = Now();
            generation["failureMessage"] = "应用重启，生成已中断";
            thread["unread"] = true;
            thread["updatedAt"] = Now();
            changed = true;
        }
    if (changed)
        Save(state.document);
    return changed;
}
} // namespace rimes::mailbox
