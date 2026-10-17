#include "capsule_model.hpp"
#include "capsule_resources.hpp"
#include "capsule_rich_text.hpp"
#include "private_files.hpp"
#include <cairo-pdf.h>
#include <fstream>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>
using namespace rimes::capsule;
using Json = nlohmann::json;
static void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
static std::string File(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), {}};
}
int main() {
    const auto root =
        std::filesystem::temp_directory_path() / ("xsime-resource-" + rimes::runtime::RandomID());
    try {
        std::filesystem::create_directories(root / "source");
        const auto fixtures = Json::parse(File(NATIVE_REFERENCE_FIXTURES));
        for (const auto& item : fixtures.at("cases")) {
            std::vector<std::string> lines;
            for (const auto& [key, value] : item.at("headers").items())
                lines.push_back(key + ": " + value.dump());
            const auto reference = ReadReference(*ParseFrontMatter(lines));
            Check(reference.kind == item.at("kind"), "reference kind drift");
            Check(Json(reference.Missing()) == item.at("missing"),
                  "missing fields differ from native");
            const auto citation =
                FormatReference(item.at("title"), reference, item.at("style").get<std::string>());
            Check(citation ? Json(*citation) == item.at("citation") : item.at("citation").is_null(),
                  "citation differs from actual macOS formatter");
        }
        const auto pdf = root / "source" / "研究 #1.pdf";
        auto* surface = cairo_pdf_surface_create(pdf.c_str(), 400, 300);
        auto* cr = cairo_create(surface);
        cairo_set_source_rgb(cr, .2, .4, .8);
        cairo_paint(cr);
        cairo_show_page(cr);
        cairo_set_source_rgb(cr, .9, .2, .2);
        cairo_paint(cr);
        cairo_show_page(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        ContentStore store(root / "capsule");
        std::string error;
        Record value, saved;
        value.kind = Kind::Pdf;
        value.title = "Sample reference";
        value.content = pdf.string();
        value.reference.kind = "journalArticle";
        value.reference.fields = {{"authors", "张三; Alice"},
                                  {"year", "2024"},
                                  {"container", "Journal"},
                                  {"doi", "10.1000/fixture"}};
        value.extra_header = {"tags: [research, local]", "custom: untouched"};
        Check(ValidateResource(value, &error), "valid PDF rejected");
        Check(store.Put(value, "", &saved, &error) == StoreStatus::Ok, "save reference");
        Check(saved.reference.Get("authors") == "张三; Alice" &&
                  saved.extra_header == value.extra_header,
              "reference roundtrip lost metadata");
        auto changed = saved;
        changed.reference.fields["year"] = "2025";
        Check(store.Put(changed, saved.revision, &saved, &error) == StoreStatus::Ok,
              "edit reference");
        Check(saved.reference.Get("year") == "2025" && saved.extra_header == value.extra_header,
              "edit retained old reference or lost unknown header");
        Check(store.Put(changed, changed.revision, nullptr, &error) ==
                  StoreStatus::RevisionConflict,
              "stale reference overwrote edit");
        CapsuleModel model;
        model.EnableModule(Module::Resources, true);
        model.set_module(Module::Resources);
        model.LoadCards({saved});
        model.set_query("张三 10.1000");
        Check(model.cards().size() == 1, "reference metadata not searchable");
        Check(model.cards()[0].payload.empty(),
              "resource search metadata leaked into wire payload");
        std::atomic<bool> cancelled{false};
        auto preview = PreviewResource(saved, store, cancelled);
        Check(preview.pages == 2 && !preview.png.empty(), "real first-page PDF preview failed");
        auto* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
        Check(gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(preview.png.data()),
                                      preview.png.size(), nullptr) &&
                  gdk_pixbuf_loader_close(loader, nullptr),
              "preview is not PNG");
        auto* image = gdk_pixbuf_loader_get_pixbuf(loader);
        Check(gdk_pixbuf_get_width(image) == 1600 && gdk_pixbuf_get_height(image) == 1200,
              "PDF preview dimensions drift");
        auto* pixel = gdk_pixbuf_get_pixels(image);
        Check(pixel[2] > pixel[0], "preview rendered the wrong PDF page");
        g_object_unref(loader);
        const auto png = root / "source" / "image.png";
        std::ofstream(png, std::ios::binary) << preview.png;
        auto document = value;
        document.kind = Kind::Resource;
        document.content = png.string();
        Check(!PreviewResource(document, store, cancelled).png.empty(), "image preview failed");
        const auto text = root / "source" / "notes.md";
        std::ofstream(text) << std::string(17000, 'a');
        document.content = text.string();
        preview = PreviewResource(document, store, cancelled);
        Check(preview.text.size() == 16000 && preview.message.find("16000") != preview.message.npos,
              "text preview not bounded");
        std::ofstream(root / "source" / "binary.bin", std::ios::binary).write("a\0b", 3);
        document.content = (root / "source" / "binary.bin").string();
        Check(PreviewResource(document, store, cancelled).text.empty(),
              "binary file exposed as text");
        const auto huge = root / "source" / "too-big.pdf";
        std::ofstream(huge) << "%PDF-1.4\n";
        std::filesystem::resize_file(huge, 256ULL * 1024 * 1024 + 1);
        document.kind = Kind::Pdf;
        document.content = huge.string();
        Check(PreviewResource(document, store, cancelled).png.empty(), "oversize PDF decoded");
        std::filesystem::create_symlink(pdf, root / "link.pdf");
        document.content = (root / "link.pdf").string();
        Check(!ValidateResource(document, &error), "symlink accepted");
        Check(store.Put(document, "", nullptr, &error) == StoreStatus::Invalid,
              "writer accepted linked PDF");
        std::filesystem::create_directory_symlink(root / "source", root / "parent-link");
        document.content = (root / "parent-link" / pdf.filename()).string();
        Check(!ValidateResource(document, &error), "symlink ancestor accepted");
        Check(OpenResource(root / "source" / ".." / "source" / pdf.filename()) < 0,
              "parent traversal accepted");
        mkfifo((root / "fifo.pdf").c_str(), 0600);
        document.content = (root / "fifo.pdf").string();
        Check(!ValidateResource(document, &error), "FIFO accepted");
        auto project = value;
        project.kind = Kind::Resource;
        project.resource_type = "project";
        project.content = (root / "source").string();
        Check(ValidateResource(project, &error) &&
                  PreviewResource(project, store, cancelled).png.empty(),
              "directory bookmark failed");
        Check(store.ImportAttachment(root / "source", &document.attachment, &error) ==
                  StoreStatus::Invalid,
              "directory recursively imported");
        std::ofstream(root / "source" / "SKILL.md") << "# Skill\nLocal instructions";
        project.kind = Kind::Skill;
        Check(PreviewResource(project, store, cancelled).text == "# Skill\nLocal instructions",
              "legacy Skill folder preview failed");
        // Explicit copy survives source removal, entry deletion and a backup moved to another root.
        Check(store.ImportAttachment(pdf, &saved.attachment, &error) == StoreStatus::Ok,
              "attachment import failed");
        Check(store.Put(saved, saved.revision, &saved, &error) == StoreStatus::Ok,
              "attach reference failed");
        const auto original = File(pdf);
        std::filesystem::remove(pdf);
        Check(!ValidateResource(value, &error), "missing path accepted");
        Check(!PreviewResource(value, store, cancelled).message.empty(),
              "missing path not explained");
        const auto materialized = ResolveResource(saved, store);
        Check(File(materialized) == original && materialized.filename() == "研究 #1.pdf",
              "attachment copy corrupted filename or bytes");
        struct stat mode {};
        stat(materialized.c_str(), &mode);
        Check((mode.st_mode & 0777) == 0600, "materialized file not private");
        Check(ResourceUri(materialized).find("%23") != std::string::npos &&
                  ResourceUri(materialized).find(' ') == std::string::npos,
              "file URL not encoded");
        std::filesystem::copy(store.root(), root / "restored",
                              std::filesystem::copy_options::recursive);
        ContentStore restored(root / "restored");
        Record recovered;
        Check(restored.Get(saved.id, &recovered, &error) == StoreStatus::Ok,
              "restore reference failed");
        Check(File(ResolveResource(recovered, restored)) == original,
              "restored attachment still depends on source root");
        Check(store.Remove(saved.id, saved.revision, &error) == StoreStatus::Ok,
              "remove reference failed");
        std::string bytes;
        Check(store.ReadAttachment(saved.attachment, &bytes, &error) == StoreStatus::Ok &&
                  bytes == original,
              "deleting entry deleted attachment");
        cancelled.store(true);
        Check(PreviewResource(recovered, restored, cancelled).png.empty(),
              "cancelled preview decoded");
        // Existing PDF Markdown without metadata is still a reference, with no invented facts.
        Record legacy;
        Check(ParseMarkdown("---\ncapsule: pdf\nversion: 1\nid: "
                            "\"11111111-1111-4111-8111-111111111111\"\ntitle: \"Legacy "
                            "PDF\"\n---\n\n/missing/old.pdf",
                            &legacy, &error),
              "legacy PDF parser failed");
        Check(legacy.kind == Kind::Pdf && legacy.reference.Get("authors").empty() &&
                  legacy.reference.Missing().size() == 3,
              "legacy PDF invented metadata");
        std::cout << fixtures.at("cases").size()
                  << " native citations; bounded PDF/image/text, path safety, CAS, search, "
                     "attachments and backup restore passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
    std::filesystem::remove_all(root);
    return 0;
}
