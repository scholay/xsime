#include "capsule_text.hpp"
#include "mailbox_store.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace rimes::mailbox;
using rimes::runtime::PrivateDirectory;
namespace {
void Check(bool ok, const char* reason) {
    if (!ok)
        throw std::runtime_error(reason);
}
template <class F> void Reject(F action, const char* reason) {
    bool failed = false;
    try {
        action();
    } catch (const std::exception&) {
        failed = true;
    }
    Check(failed, reason);
}
Json Source() {
    return {{"kind", "http"},
            {"displayName", "公开 Linux 测试来源"},
            {"replyCapability", "localNotesOnly"}};
}
void Put(const std::filesystem::path& path, const Json& value) {
    PrivateDirectory(path).Write("mailbox.json", value.dump());
}
} // namespace
int main(int argc, char** argv) {
    const auto root =
        std::filesystem::temp_directory_path() / ("xsime-mailbox-core-" + std::to_string(getpid()));
    try {
        Check(!std::filesystem::exists(root), "fixture directory already exists");
        const auto fixture = Json::parse(std::ifstream(RIMES_MAILBOX_FIXTURE));
        auto native = fixture.at("document");
        Validate(native);
        for (const auto& item : fixture.at("cases")) {
            Json ids = Json::array();
            for (const auto& thread : Filter({native, "fixture"}, item.at("module"),
                                             item.at("query"), item.at("filter")))
                ids.push_back(thread.at("id"));
            Check(ids == item.at("ids"), "workspace rules differ from native fixture");
        }
        auto legacy = fixture.at("legacy");
        Validate(legacy);
        for (const auto& thread : legacy.at("threads")) {
            Check(Module(thread) == (thread.at("source").at("replyCapability") == "localNotesOnly"
                                         ? "inbox"
                                         : "chat"),
                  "legacy module fallback differs");
            for (const auto& message : thread.at("messages"))
                Check(message.contains("kind") && message.contains("format"),
                      "legacy message defaults missing");
        }
        Put(root, fixture.at("document"));
        Store store(root);
        Check(store.Read().document == native, "native document read differs");
        const auto raw_before = PrivateDirectory(root).Read("mailbox.json", 16 * 1048576);
        Check(raw_before && Json::parse(*raw_before) == fixture.at("document"),
              "read rewrote native document");
        auto thread = store.Create(Source(), "Linux 创建的公开收件", "正文第一行\n第二行🙂");
        Check(Module(thread) == "inbox" && thread.at("unread") == true &&
                  thread.at("review").at("state") == "pending",
              "inbound semantics differ");
        const auto stale = Revision(thread);
        thread = store.AddNote(thread.at("id"), stale, "这是一条本地备注，不会发送给来源。");
        Check(thread.at("messages").back().at("kind") == "localNote" &&
                  thread.at("messages").back().at("author") == "你",
              "note semantics differ");
        auto before = store.Read();
        Reject([&] { store.Rename(thread.at("id"), stale, "stale write"); },
               "stale revision replaced newer note");
        Check(store.Read().revision == before.revision, "conflict changed durable data");
        thread = store.MarkRead(thread.at("id"), Revision(thread));
        Check(thread.at("unread") == false, "read marker missing");
        const auto content_date = thread.at("updatedAt");
        thread = store.Rename(thread.at("id"), Revision(thread), "  ");
        Check(!thread.contains("title") && Title(thread) == "正文第一行", "blank title did not restore native title fallback");
        thread = store.Rename(thread.at("id"), Revision(thread), "  Linux 往返验证  ");
        Check(thread.at("title") == "Linux 往返验证", "rename normalization differs");
        thread = store.Archive(thread.at("id"), Revision(thread), true);
        Check(thread.contains("archivedAt") && Status(thread) == "已归档", "archive not persisted");
        thread = store.Archive(thread.at("id"), Revision(thread), false);
        Check(!thread.contains("archivedAt"), "unarchive not persisted");
        Check(thread.at("updatedAt") == content_date, "rename/archive reordered unchanged content unlike macOS");
        Check(Store(root).Get(thread.at("id")) == thread, "restart lost messages");
        {
            auto lock = PrivateDirectory(root).Lock();
            const auto started = std::chrono::steady_clock::now();
            Check(store.Get(thread.at("id")) == thread,
                  "reader lost its atomic snapshot while writer holds lock");
            Reject([&] { store.Rename(thread.at("id"), Revision(thread), "blocked write"); },
                   "concurrent writer lock ignored");
            Check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(200),
                  "store blocks UI on locked file");
        }
        auto future = store.Read().document;
        future["futureRoot"] = {{"keep", true}};
        for (auto& row : future.at("threads"))
            if (row.at("id") == thread.at("id")) {
                row["futureThread"] = 42;
                row["messages"][0]["futureMessage"] = "preserve";
            }
        Put(root, future);
        thread = store.Get(thread.at("id"));
        thread = store.AddNote(thread.at("id"), Revision(thread), "unknown fields retained");
        Check(store.Read().document.at("futureRoot").at("keep") == true &&
                  thread.at("futureThread") == 42 &&
                  thread.at("messages")[0].at("futureMessage") == "preserve",
              "forward fields discarded by mutation");
        const auto valid = store.Read().document;
        auto invalid = [&](const std::function<void(Json&)>& change) {
            auto value = valid;
            change(value);
            Put(root, value);
            const auto bytes = PrivateDirectory(root).Read("mailbox.json", 16 * 1048576);
            Reject([&] { store.Create(Source(), "no overwrite", "public test"); },
                   "corrupt document accepted");
            Check(PrivateDirectory(root).Read("mailbox.json", 16 * 1048576) == bytes,
                  "corrupt document overwritten");
            Put(root, valid);
        };
        invalid([](Json& d) { d["schemaVersion"] = 2; });
        invalid([](Json& d) { d["schemaVersion"] = 1.0; });
        invalid([](Json& d) { d["nextSequence"] = 1; });
        invalid([](Json& d) { d["threads"].push_back(d["threads"][0]); });
        invalid([](Json& d) { d["threads"][0]["source"]["replyCapability"] = "localNotesOnly"; });
        invalid([](Json& d) { d["threads"][0]["messages"][0]["body"] = "  \n  "; });
        invalid(
            [](Json& d) { d["threads"][0]["messages"][0]["imageFileName"] = "../private.png"; });
        invalid([](Json& d) { d["threads"][0]["messages"][0]["role"] = "system"; });
        invalid([](Json& d) {
            d["threads"][1]["review"]["messageID"] = "11111111-2222-3333-4444-555555555555";
        });
        auto small = Limits{};
        small.threads = valid.at("threads").size();
        before = store.Read();
        Reject([&] { Store(root, small).Create(Source(), "capacity", "public"); },
               "thread capacity exceeded");
        Check(store.Read().revision == before.revision, "capacity error changed data");
        auto active = native;
        auto& generation = active["threads"][0]["generation"];
        generation["phase"] = "generating";
        generation.erase("finishedAt");
        generation.erase("failureMessage");
        Put(root / "recovery", active);
        Store recovery(root / "recovery");
        auto reserved = Limits{};
        reserved.messages = active["threads"][0]["messages"].size();
        Reject([&] { Store(root / "recovery", reserved).Read(); },
               "active generation lost reserved response slot");
        Check(recovery.RecoverInterrupted() && !recovery.RecoverInterrupted(),
              "startup recovery not exactly once");
        const auto recovered = recovery.Read().document.at("threads")[0];
        Check(recovered.at("generation").at("phase") == "failed" && recovered.at("unread") == true,
              "interrupted generation not marked");
        const auto remove = store.Create(Source(), "remove", "public");
        store.Remove(remove.at("id"), Revision(remove));
        Reject([&] { store.Get(remove.at("id")); }, "deleted thread remains");
        const auto durable_before_cancel = store.Read();
        Reject(
            [&] {
                Store(root, {}, [](const std::function<void()>&) {
                }).AddNote(thread.at("id"), Revision(thread), "cancelled note");
            },
            "cancelled publication wrote a note");
        Check(store.Read().revision == durable_before_cancel.revision,
              "cancellation changed durable data");
        const auto image_path =
            std::filesystem::path(RIMES_MAILBOX_FIXTURE).parent_path() / "public-pixel.png";
        std::ifstream image_file(image_path, std::ios::binary);
        const std::string png((std::istreambuf_iterator<char>(image_file)), {});
        Reject(
            [&] {
                Store(root, {}, [](const std::function<void()>&) {
                }).Create(Source(), "cancelled PNG", "public attachment", "inbox", png);
            },
            "cancelled attachment published a thread");
        for (const auto& file : std::filesystem::directory_iterator(root / "images"))
            Check(file.path().extension() != ".png", "cancelled publication orphaned attachment");
        auto image_thread =
            store.Create(Source(), "Linux 附件往返验证", "一张公开测试图片", "inbox", png);
        const std::string image_name = image_thread.at("messages")[0].at("imageFileName");
        Check(store.Image(image_name) == png, "private image bytes changed");
        Reject([&] { store.Image("../mailbox.json"); }, "attachment escaped private directory");
        Reject([&] { store.Create(Source(), "invalid image", "public", "inbox", "not a PNG"); },
               "invalid attachment accepted");
        struct stat mode {};
        Check(stat((root / "mailbox.json").c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600,
              "document permissions not private");
        Check(stat(root.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0700,
              "directory permissions not private");
        if (argc == 2) {
            const auto output = std::filesystem::path(argv[1]);
            Check(!std::filesystem::exists(output), "output fixture already exists");
            auto document = store.Read().document;
            document.erase("futureRoot");
            for (auto& row : document.at("threads")) {
                row.erase("futureThread");
                for (auto& message : row.at("messages"))
                    message.erase("futureMessage");
            }
            Put(output / "mailbox", document);
            PrivateDirectory(output / "mailbox" / "images").Write(image_name, png);
            PrivateDirectory(output).Write(
                "linux-public-fixture.json",
                Json{{"purpose", "public-parity-fixture"},
                     {"threads", document.at("threads")},
                     {"images", {{image_name, rimes::runtime::Sha256(png)}}}}
                    .dump());
        }
        std::filesystem::remove_all(root);
        std::cout << "Mailbox native 48 filters, legacy schema, CRUD/CAS, capacity, corruption, "
                     "private files and recovery passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        std::filesystem::remove_all(root);
        return 1;
    }
}
