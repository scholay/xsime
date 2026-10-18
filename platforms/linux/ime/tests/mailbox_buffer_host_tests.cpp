#include "buffer/buffer_service.hpp"
#include "mailbox_buffer_client.hpp"
#include "official_catalog.hpp"
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextmanager.h>
#include <future>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace rimes::mailbox;
namespace {
void Check(bool value, const char* text) {
    if (!value)
        throw std::runtime_error(text);
}
class Client final : public fcitx::InputContext {
  public:
    explicit Client(fcitx::InputContextManager& manager) : InputContext(manager, "receipt-target") {
        created();
    }
    ~Client() override { destroy(); }
    const char* frontend() const override { return "receipt-target"; }
    std::vector<std::string> commits;

  private:
    void commitStringImpl(const std::string& text) override { commits.push_back(text); }
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent&) override {}
    void updatePreeditImpl() override {}
};
} // namespace
int main() {
    char pattern[] = "/tmp/xsime-receipt-host-XXXXXX";
    const auto* folder = mkdtemp(pattern);
    if (!folder)
        return 1;
    const std::filesystem::path root(folder);
    for (const auto* key : {"RIMES_USER_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
                            "XDG_RUNTIME_DIR"})
        setenv(key, folder, 1);
    setenv("RIMES_BUFFER_SOCKET", (root / "buffer.sock").c_str(), 1);
    setenv("RIMES_BUFFER_HEADLESS", "1", 1);
    int status = 0;
    try {
        char name[] = "xsime-mailbox-host", disabled[] = "--disable=all";
        char* args[] = {name, disabled};
        fcitx::Instance instance(2, args);
        instance.addonManager().registerDefaultLoader(nullptr);
        fcitx::EventDispatcher dispatcher;
        dispatcher.attach(&instance.eventLoop());
        std::unique_ptr<Client> client;
        std::unique_ptr<fcitx::BufferService> host;
        rimes::plugins::Registry registry;
        registry.Register(rimes::plugins::CompiledAdapters().at("builtin.mailbox.inbox"));
        registry.ApplyState("builtin.mailbox.inbox", true, true);
        std::mutex pending_mutex;
        std::vector<std::function<void()>> pending;
        auto post = [&](auto fn) {
            std::lock_guard lock(pending_mutex);
            pending.push_back(std::move(fn));
        };
        auto main = [&](std::function<void()> fn) {
            auto promise = std::make_shared<std::promise<void>>();
            auto done = promise->get_future();
            dispatcher.schedule([fn, promise] {
                try {
                    fn();
                    promise->set_value();
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
            Check(done.wait_for(std::chrono::seconds(4)) == std::future_status::ready,
                  "event loop stopped responding");
            done.get();
        };
        auto pump = [&] {
            main([&] {
                std::function<void()> fn;
                {
                    std::lock_guard lock(pending_mutex);
                    if (!pending.empty()) {
                        fn = std::move(pending.front());
                        pending.erase(pending.begin());
                    }
                }
                if (fn)
                    fn();
            });
        };
        std::thread driver;
        std::string interrupted_id;
        dispatcher.schedule([&] {
            client = std::make_unique<Client>(instance.inputContextManager());
            client->focusIn();
            client->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
            Store startup_store(root / "mailbox");
            auto interrupted = startup_store.Create({{"kind", "http"},
                                                     {"displayName", "公开中断来源"},
                                                     {"replyCapability", "localNotesOnly"}},
                                                    "中断接收", "重启后应恢复待处理");
            interrupted_id = interrupted.at("id");
            startup_store.BeginBufferTransfer(interrupted_id, Revision(interrupted),
                                              interrupted.at("review").at("messageID"),
                                              "aabbccdd-1111-2222-3333-444455556666");
            host = std::make_unique<fcitx::BufferService>(&instance, post);
            host->BindPlugins(registry, [&](const auto& lease) { return registry.Accepts(lease); },
                              {});
            driver = std::thread([&] {
                try {
                    rimes::plugins::PackageStore packages(
                        root / "plugins", rimes::plugins::OfficialCatalog(),
                        rimes::plugins::BundledPackages(), rimes::plugins::CompiledAdapters(),
                        RIMES_PRODUCT_VERSION);
                    Store store(root / "mailbox");
                    auto create = [&] {
                        return store.Create({{"kind", "http"},
                                             {"displayName", "公开收件测试"},
                                             {"replyCapability", "localNotesOnly"}},
                                            "收件", "公开外部消息。下一句🙂");
                    };
                    auto request = [&](const Json& row,
                                       std::shared_ptr<std::atomic<bool>> keep =
                                           std::make_shared<std::atomic<bool>>(true)) {
                        return std::async(std::launch::async, [row, keep] {
                            try {
                                ReceiveInBuffer(row, row.at("review").at("messageID"),
                                                [keep] { return keep->load(); });
                                return true;
                            } catch (const std::exception&) {
                                return false;
                            }
                        });
                    };
                    auto finish = [&](std::future<bool>& result) {
                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(8);
                        while (result.wait_for(std::chrono::milliseconds(0)) !=
                               std::future_status::ready) {
                            Check(std::chrono::steady_clock::now() < deadline, "receipt timed out");
                            pump();
                            std::this_thread::sleep_for(std::chrono::milliseconds(3));
                        }
                        for (int i = 0; i < 6; ++i)
                            pump();
                        return result.get();
                    };
                    auto wait_journal = [&](const std::string& id) {
                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(5);
                        while (!store.Get(id).contains("linuxBufferTransfer")) {
                            Check(std::chrono::steady_clock::now() < deadline,
                                  "transfer never reached durable review");
                            pump();
                            std::this_thread::sleep_for(std::chrono::milliseconds(3));
                        }
                    };
                    const auto recovery_deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(4);
                    while (store.Get(interrupted_id).at("review").at("state") != "pending") {
                        Check(std::chrono::steady_clock::now() < recovery_deadline,
                              "startup did not restore unfinished receipt without a user action");
                        pump();
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    auto row = create();
                    main([&] {
                        host->model().append("已有原文");
                        host->model().select_all();
                    });
                    auto success = request(row);
                    Check(finish(success), "valid receipt failed");
                    row = store.Get(row.at("id"));
                    Check(row.at("review").at("state") == "accepted" &&
                              !row.contains("linuxBufferTransfer"),
                          "receipt not finalized");
                    main([&] {
                        Check(host->model().staged_text() == "已有原文公开外部消息。下一句🙂" &&
                                  !host->model().capture_enabled() &&
                                  !host->model().processing_active() && client->commits.empty(),
                              "receipt lost text or automatically delivered");
                    });
                    auto repeat = request(row);
                    Check(!finish(repeat), "repeated receipt duplicated content");
                    main([&] {
                        host->ShowAndCapture(client.get());
                        Check(host->SendAll() && host->model().empty() &&
                                  client->commits.size() == 3,
                              "explicit native delivery failed");
                    });
                    // A stale displayed revision cannot resolve a newer local note.
                    auto stale = create();
                    store.AddNote(stale.at("id"), Revision(stale), "保留最新备注");
                    auto stale_request = request(stale);
                    Check(!finish(stale_request), "stale source accepted");
                    // Focus changes after the durable review but before in-memory staging
                    // roll back only that review and never call commitString.
                    auto switched = create();
                    auto late = request(switched);
                    wait_journal(switched.at("id"));
                    main([&] { host->OnDeactivate(client.get(), false); });
                    Check(!finish(late), "late focus receipt accepted");
                    Check(store.Get(switched.at("id")).at("review").at("state") == "pending",
                          "late focus review not rolled back");
                    auto revoked = create();
                    auto revoked_request = request(revoked);
                    wait_journal(revoked.at("id"));
                    main([&] {
                        registry.ApplyState("builtin.mailbox.inbox", true, false);
                        host->Publish();
                    });
                    Check(!finish(revoked_request), "revoked module accepted receipt");
                    Check(store.Get(revoked.at("id")).at("review").at("state") == "pending",
                          "revoked receipt not rolled back");
                    main([&] { registry.ApplyState("builtin.mailbox.inbox", true, true); });
                    auto protected_row = create();
                    main([&] {
                        client->setCapabilityFlags(fcitx::CapabilityFlag::Password);
                        host->OnActivate(client.get());
                    });
                    auto protected_request = request(protected_row);
                    Check(!finish(protected_request), "password field accepted receipt");
                    main([&] {
                        client->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
                        host->OnActivate(client.get());
                        for (int i = 0; i < rimes::buffer::BufferModel::kMaxBlocks; ++i)
                            host->model().append("已有");
                    });
                    auto overflow = create();
                    auto full = request(overflow);
                    Check(!finish(full), "full Buffer partially accepted receipt");
                    Check(store.Get(overflow.at("id")).at("review").at("state") == "pending",
                          "capacity failure resolved receipt");
                    main([&] { host->model().discard_for_privacy(); });
                    auto timeout_row = create();
                    auto timeout_request = request(timeout_row);
                    wait_journal(timeout_row.at("id"));
                    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
                    Check(!finish(timeout_request), "expired event-loop callback staged receipt");
                    Check(store.Get(timeout_row.at("id")).at("review").at("state") == "pending",
                          "expired callback did not restore pending review");
                    auto cancel_row = create();
                    auto keep = std::make_shared<std::atomic<bool>>(true);
                    auto cancelled = request(cancel_row, keep);
                    wait_journal(cancel_row.at("id"));
                    keep->store(false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    // A queued final callback must observe the disconnected socket
                    // even if its disconnect notification is later in the queue.
                    Check(!finish(cancelled), "cancelled client accepted");
                    for (int i = 0;
                         i < 100 &&
                         store.Get(cancel_row.at("id")).at("review").at("state") != "pending";
                         ++i) {
                        pump();
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    Check(store.Get(cancel_row.at("id")).at("review").at("state") == "pending",
                          "cancelled review not rolled back");
                    main([&] {
                        Check(host->model().empty() && client->commits.size() == 3,
                              "cancelled receipt changed Buffer or target");
                    });
                    std::cout
                        << "Mailbox -> production Fcitx Buffer: source CAS, duplicate/capacity "
                           "rejection, password protection, late focus/grant/cancel rollback and "
                           "explicit native delivery passed\n";
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                    status = 1;
                }
                dispatcher.schedule([&] { instance.exit(); });
            });
        });
        instance.exec();
        if (driver.joinable())
            driver.join();
        host.reset();
        client.reset();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    std::filesystem::remove_all(root);
    return status;
}
