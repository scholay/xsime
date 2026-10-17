#include "mailbox_session.hpp"
#include "mailbox_workspace.hpp"
#include "session_fixture.hpp"
#include <fcitx-utils/event.h>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace rimes::mailbox;
namespace runtime = rimes::runtime;
int main(int argc, char** argv) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("xsime-mailbox-session-" + std::to_string(getpid()));
    int result = 1, step = 0;
    try {
        if (std::filesystem::exists(root))
            throw std::runtime_error("fixture already exists");
        setenv("RIMES_USER_DIR", root.c_str(), 1);
        gtk_init(&argc, &argv);
        const auto fixture = Json::parse(std::ifstream(RIMES_MAILBOX_FIXTURE));
        runtime::PrivateDirectory(root / "mailbox")
            .Write("mailbox.json", fixture.at("document").dump());
        fcitx::EventLoop loop;
        SessionFixture session(loop, true);
        session.locked = session.saverActive = true;
        Workspace host;
        DesktopSession monitor([&](bool value) { host.Protect(value); });
        std::string persisted;
        auto check = [](bool ok, const char* reason) {
            if (!ok)
                throw std::runtime_error(reason);
        };
        const auto started = fcitx::now(CLOCK_MONOTONIC);
        uint64_t checkpoint = started;
        auto timer = loop.addTimeEvent(
            CLOCK_MONOTONIC, started + 10000, 0, [&](fcitx::EventSourceTime* source, uint64_t) {
                try {
                    while (gtk_events_pending())
                        gtk_main_iteration();
                    const auto now = fcitx::now(CLOCK_MONOTONIC);
                    check(now - started < 10000000, "session deadline");
                    const bool visible = gtk_widget_get_visible(host.Window());
                    if (now - checkpoint >= 100000) {
                        if (step == 0 && session.reads >= 4 && host.Inspect().at("ready") == true) {
                            check(host.Protected() &&
                                      !host.Open("inbox", fixture.at("selectedInbox")) && !visible,
                                  "initial lock exposed workspace");
                            session.Saver(false);
                            ++step;
                        } else if (step == 1) {
                            check(host.Protected() && !visible, "overlapping lock cleared early");
                            session.Property("LockedHint", false);
                            ++step;
                        } else if (step == 2 && !host.Protected()) {
                            check(!visible && host.Open("inbox", fixture.at("selectedInbox")),
                                  "unlock auto-opened or explicit open failed");
                            ++step;
                        } else if (step == 3 && Store(root / "mailbox")
                                                        .Get(fixture.at("selectedInbox"))
                                                        .at("unread") == false) {
                            check(visible, "explicit open stayed hidden");
                            persisted = *runtime::PrivateDirectory(root / "mailbox")
                                             .Read("mailbox.json", 16 * 1024 * 1024);
                            session.Saver(true);
                            session.Sleep(true);
                            ++step;
                        } else if (step == 4 && host.Protected()) {
                            check(!visible && !host.Open("chat"),
                                  "live lock did not hide/reject open");
                            session.Saver(false);
                            ++step;
                        } else if (step == 5) {
                            check(host.Protected() && !visible,
                                  "screen unlock released sleep protection");
                            session.Sleep(false);
                            ++step;
                        } else if (step == 6 && !host.Protected()) {
                            check(!visible, "wake silently reopened private window");
                            session.bus.releaseName(SessionFixture::saver);
                            ++step;
                        } else if (step == 7 && host.Protected()) {
                            check(!visible, "provider loss exposed workspace");
                            session.bus.requestName(SessionFixture::saver,
                                                    fcitx::dbus::RequestNameFlag::None);
                            ++step;
                        } else if (step == 8 && !host.Protected()) {
                            check(!visible && host.Open("inbox"),
                                  "provider restart auto-opened or failed recovery");
                            check(*runtime::PrivateDirectory(root / "mailbox")
                                          .Read("mailbox.json", 16 * 1024 * 1024) == persisted,
                                  "lock/wake changed messages");
                            std::cout << "Mailbox GTK session: initial/overlapping lock, "
                                         "sleep/wake, provider loss/restart, explicit reopen and "
                                         "data retention passed\n";
                            result = 0;
                            loop.exit();
                            return true;
                        }
                        checkpoint = now;
                    }
                    source->setTime(now + 10000);
                    source->setOneShot();
                } catch (const std::exception& error) {
                    std::cerr << "Mailbox session step " << step << ": " << error.what() << '\n';
                    loop.exit();
                }
                return true;
            });
        loop.exec();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    std::filesystem::remove_all(root);
    return result;
}
