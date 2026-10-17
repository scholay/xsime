#include "clipboard_backend.hpp"
#include "process.hpp"
#include <chrono>
#include <cstdlib>
#include <gtk/gtk.h>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace rimes::capsule;
using Clock = std::chrono::steady_clock;
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        const bool wayland = std::getenv("WAYLAND_DISPLAY") != nullptr;
        if (!wayland)
            gtk_init(&argc, &argv);
        ClipboardBackendState state;
        state.protected_input.store(false);
        state.capture = [](auto, auto, auto) {};
        state.status = [](auto) {};
        auto producer = wayland ? WaylandClipboard(state) : X11Clipboard(state);
        std::mutex mutex;
        std::vector<ClipboardArchive> captured;
        std::string status;
        DesktopClipboard consumer(
            [&](auto archive, auto source, auto) {
                Check(source.backend == (wayland ? "wayland" : "x11"), "source backend");
                std::lock_guard<std::mutex> lock(mutex);
                captured.push_back(std::move(archive));
            },
            [&](auto value) {
                std::lock_guard<std::mutex> lock(mutex);
                status = std::move(value);
            });
        auto count = [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return captured.size();
        };
        auto spin = [&](
                        int milliseconds,
                        const std::function<bool()>& done = [] { return false; }) {
            const auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
            do {
                producer->Tick();
                if (!wayland)
                    while (gtk_events_pending())
                        gtk_main_iteration();
                if (done())
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } while (Clock::now() < end);
            return done();
        };
        Check(spin(3000,
                   [&] {
                       std::lock_guard<std::mutex> lock(mutex);
                       return !status.empty();
                   }),
              "observer initialization timeout");
        ClipboardArchive first{{{"text/plain;charset=utf-8", "initial private clipboard"}}};
        producer->Restore(first);
        spin(150);
        consumer.Configure(true, false);
        spin(150);
        Check(count() == 0, "initial clipboard was captured during enable baseline");
        ClipboardArchive text{{{"text/plain;charset=utf-8", "中文 example\nsecond line"},
                               {"text/html", "<b>中文</b>"},
                               {"application/x-fixture", std::string("\0raw\xff", 5)}}};
        const auto old_epoch = state.epoch.load(), old_write = state.write_epoch.load();
        state.protected_input.store(true);
        Check(!producer->Restore(text, old_epoch, old_write),
              "protected in-flight restore reached ownership write");
        state.protected_input.store(false);
        state.write_epoch.fetch_add(1);
        Check(!producer->Restore(text, old_epoch, old_write),
              "revoked in-flight restore reached ownership write");
        spin(100);
        Check(count() == 0, "rejected restore changed desktop clipboard");
        producer->Restore(text);
        Check(spin(5000, [&] { return count() == 1; }), "multi-MIME capture timeout");
        {
            std::lock_guard<std::mutex> lock(mutex);
            Check(captured.back().representations == text.representations,
                  "multi-MIME bytes changed");
        }
        auto secret = text;
        secret.representations.push_back({"x-kde-passwordManagerHint", "secret"});
        producer->Restore(secret);
        spin(250);
        Check(count() == 1, "password-manager marker ignored");
        consumer.Configure(true, true);
        spin(100);
        producer->Restore(first);
        spin(150);
        consumer.Configure(true, false);
        spin(150);
        Check(count() == 1, "protected copy caught up on resume");
        ClipboardArchive large{{{"text/plain;charset=utf-8", std::string(3 * 1024 * 1024, 'x')},
                                {"application/x-fixture", std::string("\0raw\xff", 5)}}};
        producer->Restore(large);
        Check(spin(8000, [&] { return count() == 2; }), "large nonblocking transfer timeout");
        {
            std::lock_guard<std::mutex> lock(mutex);
            Check(captured.back().representations == large.representations,
                  "large MIME truncation");
        }
        consumer.Restore(large);
        spin(250);
        Check(count() == 2, "own restore entered history");
        for (const auto& value : large.representations) {
            if (wayland) {
                std::atomic<bool> cancel{false};
                const auto result = rimes::runtime::RunProcess(
                    {"/usr/bin/wl-paste", "--no-newline", "--type", value.type}, "", cancel,
                    std::chrono::seconds(8), 4 * 1024 * 1024);
                Check(result.success && result.output == value.bytes,
                      "Wayland restoration was not byte-exact");
            } else {
                auto* data =
                    gtk_clipboard_wait_for_contents(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
                                                    gdk_atom_intern(value.type.c_str(), FALSE));
                Check(data &&
                          gtk_selection_data_get_length(data) ==
                              static_cast<int>(value.bytes.size()) &&
                          std::string(
                              reinterpret_cast<const char*>(gtk_selection_data_get_data(data)),
                              value.bytes.size()) == value.bytes,
                      "X11 restoration was not byte-exact");
                gtk_selection_data_free(data);
            }
        }
        ClipboardArchive partial{{{"text/plain;charset=utf-8", "valid before failure"},
                                  {"application/x-unavailable", ""},
                                  {"text/html", "<b>valid after failure</b>"}}};
        if (wayland)
            producer->Restore(partial);
        else {
            GtkTargetEntry targets[] = {{const_cast<char*>("text/plain;charset=utf-8"), 0, 0},
                                        {const_cast<char*>("application/x-unavailable"), 0, 1},
                                        {const_cast<char*>("text/html"), 0, 2}};
            Check(gtk_clipboard_set_with_data(
                      gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), targets, 3,
                      +[](GtkClipboard*, GtkSelectionData* selection, guint index, gpointer raw) {
                          if (index == 1)
                              return; // Advertised conversion refuses its request.
                          auto* archive = static_cast<ClipboardArchive*>(raw);
                          const auto& value = archive->representations.at(index);
                          gtk_selection_data_set(
                              selection, gtk_selection_data_get_target(selection), 8,
                              reinterpret_cast<const guchar*>(value.bytes.data()),
                              value.bytes.size());
                      },
                      +[](GtkClipboard*, gpointer) {}, &partial),
                  "partial producer unavailable");
        }
        Check(spin(8000, [&] { return count() == 3; }),
              "one failed MIME discarded successful formats");
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto& values = captured.back().representations;
            Check(values.size() == 2 && values[0] == partial.representations[0] &&
                      values[1] == partial.representations[2],
                  "partial MIME failure mixed or truncated remaining representations");
        }
        consumer.Configure(false, false);
        spin(100);
        producer->Restore(first);
        spin(150);
        Check(count() == 3, "disabled observer read clipboard");
        std::cout << (wayland ? "Wayland" : "X11")
                  << ": real multi-MIME, binary and 3 MiB transfers, protection baseline, "
                     "exclusions and own-copy suppression passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
