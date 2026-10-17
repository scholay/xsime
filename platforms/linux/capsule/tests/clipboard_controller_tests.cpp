#include "clipboard_backend.hpp"
#include "clipboard_controller.hpp"
#include "configuration.hpp"
#include "capsule_resources.hpp"
#include "capsule_capture.hpp"
#include <fstream>
#include "official_catalog.hpp"
#include <chrono>
#include <gtk/gtk.h>
#include <iostream>
#include <thread>
#include <xcb/xcb.h>
using namespace rimes::capsule;
using Json = nlohmann::json;
static void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("xsime-clipboard-controller-" + rimes::runtime::RandomID());
    try {
        setenv("RIMES_USER_DIR", root.c_str(), 1);
        gtk_init(&argc, &argv);
        rimes::runtime::ConfigurationStore config(root / "configuration");
        auto values = config.Load();
        values["plugins"]["builtin.capsule.temporary"] = {{"captureEnabled", true}};
        config.Save(values, values.at("revision"));
        rimes::plugins::PackageStore packages(
            root / "plugins", rimes::plugins::OfficialCatalog(), rimes::plugins::BundledPackages(),
            rimes::plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
        auto module = packages.Inspect("builtin.capsule.temporary");
        if (!module.enabled)
            packages.Enable(module.id, true, module.grant);
        ClipboardBackendState producer_state;
        producer_state.protected_input.store(false);
        producer_state.status = [](auto) {};
        producer_state.capture = [](auto, auto, auto) {};
        auto producer = X11Clipboard(producer_state);
        ClipboardArchive initial{{{"text/plain;charset=utf-8", "private baseline"}}};
        producer->Restore(initial);
        int refreshed = 0, closed = 0;
        std::string status;
        ClipboardController controller(
            [&](const std::string& raw) {
                const auto op = Json::parse(raw).at("op");
                if (op == "refresh")
                    ++refreshed;
                if (op == "close")
                    ++closed;
            },
            [&](auto text) { status = std::move(text); });
        Snapshot snapshot;
        snapshot.modules.push_back(Module::Temporary);
        snapshot.module = "temporary";
        snapshot.clipboard_capture = true;
        controller.Apply(snapshot);
        auto spin = [&](
                        int milliseconds,
                        const std::function<bool()>& done = [] { return false; }) {
            const auto end =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            do {
                producer->Tick();
                while (gtk_events_pending())
                    gtk_main_iteration();
                if (done())
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } while (std::chrono::steady_clock::now() < end);
            return done();
        };
        Check(spin(4000, [&] { return !status.empty(); }), "controller backend unavailable");
        spin(80);
        ClipboardHistoryStore history(ClipboardHistoryStore::DefaultRoot());
        Check(history.List().empty(), "controller captured enable baseline");
        ClipboardArchive copied{{{"text/plain;charset=utf-8", "中文原始文本"},
                                 {"text/html", "<b>中文原始文本</b>"},
                                 {"application/x-fixture", std::string("\0raw\xff", 5)}}};
        producer->Restore(copied);
        Check(spin(5000, [&] { return refreshed == 1; }),
              "background clipboard did not persist and notify");
        const auto item = history.List().at(0);
        Check(history.Read(item.id, item.revision)->representations == copied.representations,
              "controller lost MIME data");
        snapshot.password_field = true;
        controller.Apply(snapshot);
        producer->Restore(initial);
        spin(100);
        snapshot.password_field = false;
        controller.Apply(snapshot);
        spin(100);
        Check(history.List().size() == 1, "controller caught up protected copy");
        // Query selection ownership without asking the producer for its data.
        auto* xcb = xcb_connect(nullptr, nullptr);
        Check(!xcb_connection_has_error(xcb), "X11 test connection");
        auto owner = [&] {
            auto* atom =
                xcb_intern_atom_reply(xcb, xcb_intern_atom(xcb, 0, 9, "CLIPBOARD"), nullptr);
            Check(atom, "selection atom");
            const auto selection = atom->atom;
            free(atom);
            auto* reply = xcb_get_selection_owner_reply(
                xcb, xcb_get_selection_owner(xcb, selection), nullptr);
            Check(reply, "selection owner");
            const auto result = reply->owner;
            free(reply);
            return result;
        };
        const auto original_owner = owner();
        // Initial/reconnected state must consume restore sequence as a baseline.
        snapshot.password_field = true;
        controller.Apply(snapshot);
        snapshot.password_field = false;
        snapshot.visible = true;
        snapshot.restore_seq = 8;
        snapshot.restore_id = item.id;
        snapshot.restore_revision = item.revision;
        snapshot.selected_id = item.id;
        controller.Apply(snapshot);
        spin(150);
        Check(closed == 0 && owner() == original_owner,
              "reconnected snapshot replayed restoration");
        xcb_disconnect(xcb);
        auto read = [&](const std::string& type) {
            auto* data = gtk_clipboard_wait_for_contents(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
                                                         gdk_atom_intern(type.c_str(), FALSE));
            std::string bytes;
            if (data && gtk_selection_data_get_length(data) >= 0)
                bytes.assign(reinterpret_cast<const char*>(gtk_selection_data_get_data(data)),
                             gtk_selection_data_get_length(data));
            if (data)
                gtk_selection_data_free(data);
            return bytes;
        };
        // Reading the producer synchronously would block its event loop; restore
        // acknowledgments below come from the separate product clipboard worker.
        snapshot.restore_seq = 9;
        snapshot.restore_close = true;
        controller.Apply(snapshot);
        Check(spin(5000, [&] { return closed == 1; }), "restore was not acknowledged");
        for (const auto& data : copied.representations)
            Check(read(data.type) == data.bytes, "controller restoration changed bytes");
        spin(100);
        Check(refreshed == 1, "own restore entered history");
        snapshot.restore_seq = 10;
        snapshot.restore_revision = std::string(64, 'a');
        status.clear();
        controller.Apply(snapshot);
        Check(spin(3000, [&] { return status.find("未恢复") != status.npos; }),
              "stale history revision not rejected");
        Check(closed == 1, "failed restoration closed window");
        values = config.Load();
        values["plugins"]["builtin.capsule.temporary"]["captureEnabled"] = false;
        config.Save(values, values.at("revision"));
        producer->Restore(initial);
        spin(300);
        Check(history.List().size() == 1,
              "disabled persisted preference accepted stale observer capture");
        values = config.Load();
        values["plugins"]["builtin.capsule.temporary"]["captureEnabled"] = true;
        config.Save(values, values.at("revision"));
        module = packages.Inspect(module.id);
        packages.Enable(module.id, false, module.grant);
        // Even before the next host snapshot, the persisted package grant blocks capture.
        producer->Restore(initial);
        spin(300);
        Check(history.List().size() == 1, "disabled package accepted stale UI capture");
        snapshot.modules = {Module::Notes};
        snapshot.clipboard_capture = false;
        controller.Apply(snapshot);
        auto resource_grant = packages.Inspect("builtin.capsule.resources");
        if (!resource_grant.enabled) packages.Enable(resource_grant.id,true,resource_grant.grant);
        const auto resource_path=root/"file with 空格.txt"; std::ofstream(resource_path)<<"resource bytes";
        Record resource; resource.kind=Kind::Resource; resource.title="Resource"; resource.content=resource_path.string();
        ContentStore content(ContentStore::DefaultRoot()); std::string error;
        Check(content.Put(resource,"",&resource,&error)==StoreStatus::Ok,"clipboard resource fixture");
        snapshot.modules={Module::Resources}; snapshot.module="resources"; snapshot.selected_id=resource.id;
        snapshot.restore_id=resource.id; snapshot.restore_revision=resource.revision; snapshot.restore_close=false;
        snapshot.restore_seq=11; status.clear(); controller.Apply(snapshot);
        Check(spin(3000,[&] {return status.find("已复制")!=status.npos;}),"resource restore not acknowledged");
        Check(read("text/uri-list")==ResourceUri(resource_path)+"\r\n","resource file URI not exact");
        Check(read("x-special/gnome-copied-files")=="copy\n"+ResourceUri(resource_path),"resource file copy marker absent");
        snapshot.password_field=true; controller.Apply(snapshot); status.clear(); controller.CopyResource(resource); spin(100);
        Check(status.empty(),"protected explicit file copy started");
        snapshot.password_field=false; controller.Apply(snapshot);
        resource_grant=packages.Inspect(resource_grant.id); packages.Enable(resource_grant.id,false,resource_grant.grant);
        snapshot.restore_seq=12; controller.Apply(snapshot);
        Check(spin(3000,[&] {return status.find("未恢复")!=status.npos;}),"revoked resource module copied file");
        auto capture_grant = packages.Inspect("builtin.capsule.capture");
        if (!capture_grant.enabled) packages.Enable(capture_grant.id,true,capture_grant.grant);
        const auto image_path=root/"screen.png";
        auto* surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,32,24);
        cairo_surface_write_to_png(surface,image_path.c_str());cairo_surface_destroy(surface);
        CaptureStore captures(CaptureStore::DefaultRoot());CaptureRecord image;image.width=32;image.height=24;image.source="X11 fixture";
        std::atomic<bool> cancelled{false};image=captures.Import(image_path,image,cancelled);
        snapshot.modules={Module::Capture};snapshot.module="capture";snapshot.selected_id=image.id;
        snapshot.restore_id=image.id;snapshot.restore_revision=image.revision;snapshot.restore_seq=13;
        status.clear();controller.Apply(snapshot);
        Check(spin(3000,[&] {return status.find("已复制")!=status.npos;}),"capture restore not acknowledged");
        Check(read("image/png")==ReadResource(image_path,1024*1024,cancelled),"capture PNG bytes changed");
        Check(read("text/uri-list")==ResourceUri(captures.File(image))+"\r\n","capture file URI mismatch");
        Check(captures.Prune(99999999999.0,0,0)==0,"clipboard offer lost capture file lease");
        producer->Restore(initial);spin(150);
        Check(captures.Prune(99999999999.0,0,0)==1,"clipboard selection loss retained capture lease");
        std::cout
            << "GTK clipboard controller: opt-in background capture, MIME persistence, privacy "
               "baseline, reconnect/no replay, revision and package revocation passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
    std::filesystem::remove_all(root);
    return 0;
}
