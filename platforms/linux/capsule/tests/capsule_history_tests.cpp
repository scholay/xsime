#include "capsule_hash.hpp"
#include "capsule_history.hpp"
#include "capsule_modules.hpp"
#include "capsule_rich_text.hpp"
#include "private_files.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
using namespace rimes::capsule;
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void Reject(F work, const char* message) {
    bool failed = false;
    try {
        work();
    } catch (...) {
        failed = true;
    }
    Check(failed, message);
}
int main() {
    const auto root =
        std::filesystem::temp_directory_path() / ("xsime-history-" + rimes::runtime::RandomID());
    try {
        const auto& native = ModuleContract().at("clipboard");
        Check(native.at("items") == ClipboardLimits{}.items &&
                  native.at("bytes") == ClipboardLimits{}.bytes &&
                  native.at("archiveBytes") == ClipboardArchive::maxArchiveBytes &&
                  native.at("typeBytes") == ClipboardArchive::maxTypeBytes &&
                  native.at("totalBytes") == ClipboardArchive::maxTotalBytes,
              "native clipboard limits changed");
        for (const auto& type : native.at("confidentialTypes"))
            Check(!ClipboardArchive::AllowsTypes({type.get<std::string>()}),
                  "native confidential marker changed");
        std::ifstream fixtures(NATIVE_CLIPBOARD_FIXTURES);
        const auto fixture_data = nlohmann::json::parse(fixtures);
        for (const auto& fixture : fixture_data.at("cases")) {
            ClipboardArchive archive;
            for (const auto& representation : fixture.at("representations"))
                archive.representations.push_back(
                    {representation.at("type"),
                     *Base64Decode(representation.at("base64").get<std::string>())});
            const auto projection = ProjectClipboard(archive);
            Check(projection.kind == fixture.at("kind") && projection.text == fixture.at("text") &&
                      projection.filter == fixture.at("filter"),
                  "native clipboard classification/projection differs");
        }
        ClipboardCaptureGate gate;
        Check(!gate.Observe(1), "disabled capture read");
        gate.Allow(true);
        Check(!gate.Observe(2), "resume must only baseline");
        auto generation = gate.Observe(3);
        Check(generation && gate.Current(*generation), "new offer not captured");
        Check(!gate.Observe(3), "same offer repeated");
        gate.Allow(false);
        Check(!gate.Current(*generation) && !gate.Observe(4),
              "protected in-flight result survived");
        gate.Allow(true);
        Check(!gate.Observe(4), "protected clipboard must not be caught up");
        generation = gate.Observe(5);
        Check(generation.has_value(), "capture after baseline");
        gate.Baseline(6);
        Check(!gate.Current(*generation) && !gate.Observe(6), "own copy was recorded");

        ClipboardArchive text{{{"text/plain;charset=utf-8", "中文 https://example.com"},
                               {"text/html", "<b>中文</b>"}}};
        Check(ClipboardArchive::Decode(text.Encode()).representations == text.representations,
              "raw MIME round trip");
        Check(ProjectClipboard(text).kind == "text" && ProjectClipboard(text).insertable,
              "text classification");
        ClipboardArchive image{
            {{"image/png", std::string("\0binary\xff", 8)}, {"text/plain", "image caption"}}};
        Check(ProjectClipboard(image).kind == "image" && !ProjectClipboard(image).insertable,
              "image caption must not become an insertion");
        ClipboardArchive files{
            {{"text/uri-list", "file:///tmp/a%20b.png\r\nfile:///tmp/second.pdf\r\n"},
             {"image/png", "raw"}}};
        Check(ProjectClipboard(files).kind == "files" && !ProjectClipboard(files).insertable,
              "files take precedence over image and text");
        for (const auto* marker : {"org.nspasteboard.ConcealedType",
                                   "org.nspasteboard.TransientType", "com.agilebits.onepassword",
                                   "x-kde-passwordManagerHint", "application/x-rimes-sensitive"}) {
            auto sensitive = text;
            sensitive.representations.push_back({marker, ""});
            Reject([&] { sensitive.Encode(); }, "secret MIME accepted");
        }
        auto duplicate = text;
        duplicate.representations.push_back(duplicate.representations[0]);
        Reject([&] { duplicate.Encode(); }, "duplicate MIME accepted");
        auto broken = text;
        broken.representations[0].format = 16;
        broken.representations[0].bytes = "x";
        Reject([&] { broken.Encode(); }, "misaligned selection format accepted");
        const auto encoded = image.Encode();
        for (std::size_t size = 0; size < encoded.size(); ++size)
            Reject([&] { ClipboardArchive::Decode(encoded.substr(0, size)); },
                   "truncated archive accepted");

        Check(ProjectClipboard(ClipboardArchive{{{"text/uri-list", "file:///tmp/a%20b.PNG"}}})
                      .filter == "image",
              "single image-file filter");
        Check(ProjectClipboard(ClipboardArchive{{{"text/uri-list", "file:///tmp/a%20b.PNG"}}})
                      .title == "a b.PNG",
              "URI title decoding");
        Check(ProjectClipboard(ClipboardArchive{{{"text/plain", "https://example.com\nparagraph"}}})
                      .kind == "text",
              "multiline link text misclassified");
        Check(!ProjectClipboard(ClipboardArchive{{{"text/plain", std::string("a\0b", 3)}}})
                   .insertable,
              "NUL cannot enter native commitString");
        {
            ClipboardHistoryStore store(root / "pages");
            std::string body(3000, 'x');
            body += " 深藏的全文检索";
            auto long_item = store.Capture(ClipboardArchive{{{"text/plain", body}}},
                                           {"x11", "SourceApplication", ""});
            for (int index = 0; index < 260; ++index)
                store.Capture(ClipboardArchive{{{"text/plain", "item " + std::to_string(index)}}},
                              {"x11", "", ""});
            auto first = store.Page("", "all", 0), last = store.Page("", "all", 260);
            Check(first.count == 261 && first.entries.size() == 200 && last.selected == 260 &&
                      last.offset == 160 && last.entries.size() == 101,
                  "metadata paging bounds");
            auto search = store.Page("深藏的全文检索", "text");
            Check(search.count == 1 && search.entries[0].id == long_item.id,
                  "full text outside preview lost in search");
            Check(store.Page("SourceApplication", "all").count == 1 &&
                      store.Page("", "image").count == 0,
                  "source and kind filters");
            int checks = 0;
            Reject(
                [&] {
                    store.Capture(ClipboardArchive{{{"text/plain", "cancel before commit"}}},
                                  {"x11", "", ""}, [&] { return ++checks < 3; });
                },
                "late revoked capture committed");
            Check(store.Page("", "all").count == 261, "revoked capture was not rolled back");
            auto invalid = image;
            invalid.representations[0].type = std::string("bad\xff", 4);
            Check(!ClipboardArchive::AllowsTypes({invalid.representations[0].type}),
                  "non-ASCII format requested before validation");
        }
        ClipboardEntry saved, held;
        {
            ClipboardHistoryStore store(root, {3, 1048576});
            saved = store.Capture(text, {"x11", "Fixture", "owner-1"});
            auto same = store.Capture(text, {"x11", "Fixture 2", "owner-2"});
            Check(saved.id == same.id && saved.revision != same.revision &&
                      !store.Read(saved.id, saved.revision),
                  "capture dedupe/revision");
            saved = same;
            Check(store.List("中文 example").size() == 1, "Unicode multi-term search");
            Check(store.List("% OR 1=1").empty(), "search is not SQL/wildcard syntax");
            Check(store.Favorite(saved.id, saved.revision, true), "favorite");
            held = *store.Get(saved.id);
            Check(!store.Remove(saved.id, saved.revision), "stale delete accepted");
            saved = store.Capture(image, {"wayland", "", "seat0"});
            Check(store.Read(saved.id, saved.revision)->representations == image.representations,
                  "binary persisted bytes changed");
            auto file = store.Capture(files, {"x11", "Files", "owner-3"});
            auto fourth = text;
            fourth.representations[0].bytes = "new copy";
            store.Capture(fourth, {"x11", "", ""});
            Check(store.List().size() == 3 && store.Get(held.id)->favorite,
                  "pruning deleted a favorite");
            store.ClearTemporary();
            Check(store.List().size() == 1 && store.Get(held.id), "clear must preserve favorites");
        }
        {
            ClipboardHistoryStore reopened(root);
            Check(reopened.Read(held.id, held.revision)->representations == text.representations,
                  "restart lost MIME");
            Check(reopened.Remove(held.id, held.revision) && reopened.List().empty(),
                  "explicit favorite delete");
        }
        struct stat status {};
        Check(!stat((root / "history-v1.sqlite3").c_str(), &status) &&
                  (status.st_mode & 0777) == 0600,
              "database not private");
        const auto unsafe = root / "unsafe";
        rimes::runtime::PrivateDirectory directory(unsafe);
        std::filesystem::create_symlink(root / "history-v1.sqlite3", unsafe / "history-v1.sqlite3");
        Reject([&] { ClipboardHistoryStore blocked(unsafe); }, "database symlink followed");
        const auto journal = root / "journal";
        rimes::runtime::PrivateDirectory journal_dir(journal);
        std::filesystem::create_symlink(root / "history-v1.sqlite3",
                                        journal / "history-v1.sqlite3-journal");
        Reject([&] { ClipboardHistoryStore blocked(journal); }, "journal symlink followed");
        std::filesystem::remove_all(root);
        std::cout << "Clipboard baseline, confidential types, MIME persistence, CAS, pruning and "
                     "private paths passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
