#include "capsule_capture.hpp"
#include "capsule_hash.hpp"
#include "capsule_resources.hpp"
#include "capsule_text.hpp"
#include "capture_process.hpp"
#include "private_files.hpp"
#include <cairo.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <sys/stat.h>
using namespace rimes::capsule;
namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void Reject(F action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    Check(rejected, message);
}
} // namespace
int main() {
    const auto root =
        std::filesystem::temp_directory_path() / ("xsime-capture-" + rimes::runtime::RandomID());
    try {
        rimes::runtime::PrivateDirectory private_root(root);
        ContentStore library(root / "capsule");
        CaptureStore store(root / "capsule/captures");
        auto* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 64, 48);
        auto* cr = cairo_create(surface);
        cairo_set_source_rgb(cr, 0.1, 0.4, 0.8);
        cairo_paint(cr);
        cairo_surface_write_to_png(surface, (root / "source.png").c_str());
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        std::atomic<bool> cancelled{false};
        CaptureRecord value;
        value.source = "X11 fixture";
        value.title = "测试截图";
        value.created = 1000;
        value.width = 64;
        value.height = 48;
        auto saved = store.Import(root / "source.png", value, cancelled);
        Check(store.List().size() == 1, "capture manifest publication");
        Check(saved.bytes > 0 && saved.sha256.size() == 64 && saved.source == value.source,
              "capture metadata");
        struct stat info {};
        Check(!stat(store.File(saved).c_str(), &info) && (info.st_mode & 0777) == 0600,
              "capture media permissions");
        auto backup = root / "backup";
        std::filesystem::copy(root / "capsule", backup, std::filesystem::copy_options::recursive);
        CaptureStore restored(backup / "captures");
        auto recovered = restored.Get(saved.id, saved.revision);
        Check(recovered.has_value() && std::filesystem::is_regular_file(restored.File(*recovered)),
              "capture backup recovery");
        std::filesystem::remove(root / "source.png");
        Check(store.Get(saved.id).has_value(), "capture depended on source");
        Record media;
        media.kind = Kind::Image;
        media.content = store.File(saved).string();
        Check(!PreviewResource(media, library, cancelled).png.empty(), "capture PNG preview");
        Check(!store.Remove(saved.id, std::string(64, 'a')), "stale delete succeeded");
        auto favorite = store.Collect(saved.id, saved.revision);
        Check(favorite.collection_id == saved.id && favorite.revision != saved.revision,
              "collect identity/revision");
        Check(store.Collect(favorite.id, favorite.revision).collection_id == favorite.id,
              "collection not idempotent");
        Check(store.Prune(9999999, 0, 0) == 0 && std::filesystem::exists(store.File(saved)),
              "prune removed collection");
        Reject([&] { store.Remove(favorite.id, favorite.revision); },
               "history deleted permanent collection");
        Record entry;
        std::string error;
        Check(library.Get(saved.id, &entry, &error) == StoreStatus::Ok &&
                  entry.content == store.File(saved).string(),
              "favorite missing Markdown entry");
        const auto moved = root / "moved-library";
        std::filesystem::copy(root / "capsule", moved, std::filesystem::copy_options::recursive);
        CaptureStore moved_store(moved / "captures");
        Check(moved_store.Prune(9999999, 0, 0) == 0 &&
                  moved_store.Get(saved.id)->collection_id == saved.id,
              "backup move lost permanent collection identity");
        auto temporary = store.Import(store.File(saved), value, cancelled);
        auto lease = store.Acquire(temporary.id, temporary.revision);
        Check(lease.has_value(), "capture lease missing");
        Reject([&] { store.Remove(temporary.id, temporary.revision); },
               "delete ignored active lease");
        Check(store.Prune(1001, 100, 0) == 0, "prune ignored active lease");
        lease.reset();
        Check(store.Prune(1001, 100, 0) == 1 && !store.Get(temporary.id), "capacity prune");
        cancelled.store(true);
        Reject([&] { store.Import(store.File(saved), value, cancelled); },
               "cancelled import published");
        cancelled.store(false);
        Reject(
            [&] {
                store.Import(store.File(saved), value, cancelled, [&](const auto&) {
                    Check(store.List().size() == 1,
                          "staging held archive lock while authorizing publication");
                    return false;
                });
            },
            "commit gate ignored revocation");
        Check(store.List().size() == 1, "cancel left visible record");
        for (const auto& file : std::filesystem::directory_iterator(store.root()))
            Check(file.path().filename().string().rfind(".import-", 0) != 0,
                  "cancel leaked staging folder");
        std::filesystem::create_symlink(store.File(saved), root / "link.png");
        Reject([&] { store.Import(root / "link.png", value, cancelled); }, "symlink import");
        std::filesystem::create_directory_symlink(store.root(), root / "linked");
        Reject([&] { CaptureStore bad(root / "linked"); }, "symlink capture root");
        auto invalid = value;
        invalid.created = std::numeric_limits<double>::quiet_NaN();
        Reject([&] { store.Import(store.File(saved), invalid, cancelled); },
               "non-finite capture date");
        auto broken = store.Import(store.File(saved), value, cancelled);
        rimes::runtime::PrivateDirectory folder(store.root() / broken.id);
        folder.Write("record.json", "{broken");
        Check(store.List().size() == 1 && !store.Get(broken.id),
              "corrupt capture poisoned other records");
        auto card = CaptureCard(favorite);
        Check(card.favorite && card.payload.empty() && card.module == Module::Capture,
              "capture card leaked file payload");
        CapsuleModel model;
        model.EnableModule(Module::Capture, true);
        model.set_module(Module::Capture);
        model.LoadModuleCards(Module::Capture, {card});
        model.set_query("测试 X11");
        Check(model.cards().size() == 1, "capture source/title search");
        Check(CaptureDuration(125.9) == "02:05" && CaptureDuration(-1) == "--:--" &&
                  CaptureDuration(std::numeric_limits<double>::infinity()) == "--:--",
              "duration contract");
        std::ifstream fixture_input(NATIVE_CAPTURE_FIXTURES);
        const auto fixtures = nlohmann::json::parse(fixture_input);
        for (const auto& sample : fixtures.at("durations"))
            Check(CaptureDuration(std::stod(sample.at("input").get<std::string>())) ==
                      sample.at("label"),
                  "native duration drift");
        for (const auto& sample : fixtures.at("cards")) {
            CaptureRecord r;
            r.title = sample.at("title");
            r.kind = sample.at("kind");
            r.source = sample.at("source");
            r.duration = sample.at("duration");
            r.incomplete = sample.at("incomplete");
            if (sample.at("favorite"))
                r.collection_id = NewUuidV4();
            const auto card = CaptureCard(r);
            Check(CharacterPrefix(card.title, 25) == sample.at("labels").at(0) &&
                      card.preview == sample.at("labels").at(1),
                  "native capture card label drift");
        }
        CaptureControl gate;
        bool published = false;
        gate.Cancel();
        Check(!gate.Commit([&] { published = true; }) && !published, "cancel commit barrier");
        Check(library.Remove(entry.id, entry.revision, &error) == StoreStatus::Ok,
              "remove collection reference");
        Check(store.Prune(9999999, 0, 0) == 1 && !store.Get(saved.id),
              "deleted favorite retained forever");
        std::filesystem::remove_all(root);
        std::cout << "capture archive, permissions, source independence, backup, CAS, permanent "
                     "collection, pruning, cancellation and preview passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
