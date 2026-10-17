#include "buffer/buffer_service.hpp"
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextmanager.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace rimes::buffer;
void Check(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }

void BeginDrag(const fcitx::BufferService& host) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    Check(fd >= 0, "drag socket");
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    const auto path = host.SocketPath();
    if (path.size() >= sizeof(address.sun_path)) { close(fd); Check(false, "drag socket path"); }
    std::copy(path.begin(), path.end(), address.sun_path);
    std::string frame;
    const bool sent = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        EncodeFrame(R"({"v":1,"op":"drag_begin"})", &frame) &&
        send(fd, frame.data(), frame.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(frame.size());
    close(fd); Check(sent, "drag command");
}

// Exercise the production Buffer service and native commit boundary without a
// translation model, HTTP server, window server or the user's configuration.
class Client final : public fcitx::InputContext {
public:
    explicit Client(fcitx::InputContextManager& manager) : InputContext(manager, "delivery-fixture") { created(); }
    ~Client() override { destroy(); }
    const char* frontend() const override { return "delivery-fixture"; }
    std::vector<std::string> commits;
    bool reject_commit = false;
private:
    void commitStringImpl(const std::string& text) override {
        if (reject_commit) throw std::runtime_error("fixture rejected commit");
        commits.push_back(text);
    }
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent&) override {}
    void updatePreeditImpl() override {}
};

int main() {
    char pattern[] = "/tmp/xsime-delivery-host-XXXXXX";
    const auto* directory = mkdtemp(pattern);
    if (!directory) return 1;
    const std::filesystem::path root(directory);
    for (const auto* variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"})
        setenv(variable, directory, 1);
    setenv("RIMES_BUFFER_SOCKET", (root / "buffer.sock").c_str(), 1);
    setenv("RIMES_BUFFER_DUMP", (root / "buffer.json").c_str(), 1);
    setenv("RIMES_BUFFER_HEADLESS", "1", 1);
    int status = 0;
    try {
        char name[] = "xsime-delivery-fixture", disabled[] = "--disable=all";
        char* args[] = {name, disabled};
        fcitx::Instance instance(2, args);
        instance.addonManager().registerDefaultLoader(nullptr);
        fcitx::EventDispatcher startup;
        startup.attach(&instance.eventLoop());
        std::unique_ptr<Client> client;
        std::unique_ptr<fcitx::BufferService> host;
        std::vector<std::unique_ptr<fcitx::EventSourceTime>> timers;
        auto guarded = [&](const auto& action) {
            try { action(); }
            catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; instance.exit(); }
        };
        auto after = [&](std::uint64_t milliseconds, std::function<void()> action) {
            timers.push_back(instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + milliseconds * 1000, 0,
                [&, action](fcitx::EventSourceTime* timer, std::uint64_t) {
                    timer->setEnabled(false); guarded(action); return true;
                }));
        };
        std::vector<std::function<void()>> posted;
        std::mutex posted_mutex;
        auto post = [&](auto fn) { std::lock_guard lock(posted_mutex); posted.push_back(std::move(fn)); };
        auto drain = [&] {
            for (;;) {
                std::function<void()> fn;
                { std::lock_guard lock(posted_mutex); if (posted.empty()) break; fn = std::move(posted.front()); posted.erase(posted.begin()); }
                fn();
            }
        };
        auto key = [&](bool release) {
            fcitx::KeyEvent event(client.get(), fcitx::Key("Return"), release);
            Check(host->HandleEarlyKey(event, false) && event.accepted(), "Return escaped Buffer");
        };
        rimes::plugins::Registry registry;
        constexpr auto ai = "builtin.fixture", translation = "builtin.apple-translation";
        registry.Register({ai, "AI fixture", "1", rimes::plugins::Kind::Buffer, {"ai.generate"}});
        registry.Register({translation, "Translation", "1", rimes::plugins::Kind::Buffer, {"translation.generate"}});
        registry.ApplyState(ai, true, true); registry.ApplyState(translation, true, true);
        rimes::runtime::ConfigurationStore config(root / "config");
        const auto revision = config.Load().at("revision").get<std::string>();
        auto prepare = [&] {
            host.reset();
            host = std::make_unique<fcitx::BufferService>(&instance, post);
            host->Configure(false, false, 760, revision, "night");
            host->BindPlugins(registry, [&](const auto& lease) { return registry.Accepts(lease); },
                              [&](auto id) { return config.Freeze(id); });
            host->ShowAndCapture(client.get());
        };
        std::vector<fcitx::BufferService::ResultFn> replies;
        std::vector<std::shared_ptr<std::atomic<bool>>> cancelled;
        fcitx::BufferService::ResultFn drag_reply;
        std::shared_ptr<std::atomic<bool>> drag_cancelled;
        std::string prefix, pending_id;
        startup.schedule([&] { guarded([&] {
            client = std::make_unique<Client>(instance.inputContextManager());
            client->focusIn(); client->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
            prepare();
            int requests = 0;
            fcitx::BufferService::ResultFn reply;
            host->RegisterDerivedAdapter(ai, "ai.generate", [&](auto, auto, auto, auto receive) { ++requests; reply = receive; });
            Check(host->SelectPlugin(ai), "AI selection");
            host->model().append("原文", Origin::Rime);
            Check(host->Generate(), "AI generation");
            reply({0, "Ready result", true, true}); drain();
            const auto result_id = host->derived().SelectedResult()->id;
            client->focusOut();
            key(false); key(true);
            Check(requests == 1 && host->derived().CopyText() == "Ready result" &&
                  host->derived().SelectedResult()->id == result_id, "no-target Return replaced ready output");
            client->focusIn(); client->reject_commit = true;
            Check(!host->SendNext(), "rejected commit was reported as delivery");
            key(false); key(true);
            Check(requests == 1 && host->derived().CopyText() == "Ready result" &&
                  host->model().staged_text() == "原文", "conflicted Return replaced result/source");
            client->reject_commit = false;

            prepare();
            host->RegisterDerivedAdapter(translation, "translation.generate", [&](auto, auto, auto stop, auto receive) {
                replies.push_back(receive); cancelled.push_back(stop);
            });
            Check(host->SelectPlugin(translation), "translation selection");
            host->model().append("第一句。第二句。", Origin::Rime);
            Check(host->Generate(), "first sentence generation");
            replies[0]({0, "First translated sentence has several separate words and multiple delivery blocks.", true, true}); drain();
            prefix = host->translation().Preview();
            Check(host->translation().All().front().children.size() > 2, "fixture needs multiple translated children");
            Check(host->Generate() && replies.size() == 2, "second sentence generation");
            pending_id = host->translation().All().back().id;
            key(false);
            Check(client->commits.size() == 1, "derived press did not send exactly one child");
            after(1350, [&] {
                key(true);
                std::string sent; for (const auto& text : client->commits) sent += text;
                Check(sent == prefix && client->commits.size() > 2, "hold did not drain the ready prefix before the pending tail");
                Check(host->model().staged_text() == "第二句。" && host->translation().Retired().empty() &&
                      host->translation().Running() && host->translation().All().front().id == pending_id && !cancelled[1]->load(),
                      "prefix delivery erased or restarted the in-flight source unit");
                const auto commit_count = client->commits.size();
                Check(!host->SendAll() && client->commits.size() == commit_count, "pending tail was delivered");
                replies[1]({0, "Second.", true, true}); drain();
                Check(host->SendAll() && host->model().empty() && host->translation().All().empty(), "late pending result lost delivery ownership");
                prepare(); Check(host->SelectPlugin(""), "default selection");
                host->model().append("保留", Origin::Rime);
                key(false); host->model().set_preedit("ni");
                after(150, [&] {
                    const auto modified = std::filesystem::last_write_time(root / "buffer.json");
                    after(150, [&, modified] {
                        Check(std::filesystem::last_write_time(root / "buffer.json") == modified, "aborted hold kept publishing");
                        key(true);
                        Check(host->model().staged_text() == "保留", "aborted hold delivered source");
                        prepare();
                        host->RegisterDerivedAdapter(ai, "ai.generate", [&](auto, auto, auto stop, auto receive) {
                            drag_reply = receive; drag_cancelled = stop;
                        });
                        Check(host->SelectPlugin(ai), "drag AI selection");
                        host->model().append("拖动", Origin::Rime); Check(host->Generate(), "drag generation");
                        BeginDrag(*host);
                        after(150, [&] {
                            drain();
                            client->focusOut(); host->OnDeactivate(client.get(), false);
                            Check(!drag_cancelled->load(), "toolbar drag cancelled the in-flight request");
                            drag_reply({0, "Drag result", true, true}); drain();
                            Check(host->derived().CopyText() == "Drag result" && !host->SendNext(), "drag lost output or delivered without focus");
                            client->focusIn(); host->OnActivate(client.get());
                            Check(host->model().capture_enabled() && host->derived().CopyText() == "Drag result", "same-IC drag return replaced output");
                            client->focusOut(); host->OnDeactivate(client.get(), false);
                            Check(host->derived().Results().empty(), "later non-drag focus loss retained authority");
                            std::cout << "Buffer delivery host: ready-result Return, commit conflict, held ready prefix, late tail, stopped hold and drag ownership passed\n";
                            instance.exit();
                        });
                    });
                });
            });
        }); });
        instance.exec();
        host.reset(); client.reset(); startup.detach();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; }
    std::filesystem::remove_all(root);
    return status;
}
