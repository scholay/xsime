#include "configuration.hpp"
#include "gateway_access.hpp"
#include "gateway_test_client.hpp"
#include "official_catalog.hpp"
#include "session_fixture.hpp"
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/testing.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/instance.h>
#include <fstream>
#include <future>
#include <iostream>
#include <testfrontend_public.h>
#include <thread>
using namespace rimes::mailbox;
using namespace rimes::gateway_test;
namespace rt = rimes::runtime;
namespace {
void Key(fcitx::AddonInstance* frontend, const fcitx::ICUUID& uuid, const char* name) {
    frontend->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(name), false);
    frontend->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(name), true);
}
Json Status(const std::filesystem::path& root) {
    auto bytes = rt::PrivateDirectory(root / "gateway").Read("status.json", 4096);
    return bytes ? Json::parse(*bytes) : Json{{"state", "waiting"}};
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: gateway-fcitx-e2e BUILD ADDONS DATA\n";
        return 2;
    }
    char folder[] = "/tmp/xsime-gateway-fcitx-XXXXXX";
    if (!mkdtemp(folder))
        return 1;
    const std::filesystem::path root(folder);
    int result = 0;
    try {
        for (const auto* key : {"RIMES_USER_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                                "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"})
            setenv(key, folder, 1);
        setenv("RIMES_LOG_DIR", (root / "log").c_str(), 1);
        setenv("RIMES_BUFFER_SOCKET", (root / "buffer.sock").c_str(), 1);
        setenv("RIMES_BUFFER_DUMP", (root / "buffer.json").c_str(), 1);
        setenv("RIMES_CAPSULE_SOCKET", (root / "capsule.sock").c_str(), 1);
        setenv("RIMES_BUFFER_HEADLESS", "1", 1);
        setenv("RIMES_CAPSULE_HEADLESS", "1", 1);
        setenv("RIMES_BUFFER_AUTO_CAPTURE", "0", 1);
        rt::ConfigurationStore configuration(root / "configuration");
        auto config = configuration.Load();
        config["gatewayPort"] = Port();
        configuration.Save(config, config.at("revision"));
        config = configuration.Load();
        fcitx::setupTestingEnvironment(argv[1], {argv[2]}, {argv[3]});
        unsetenv("SKIP_FCITX_USER_PATH");
        setenv("FCITX_CONFIG_HOME", (root / "fcitx").c_str(), 1);
        setenv("FCITX_DATA_HOME", (root / "data").c_str(), 1);
        char name[] = "gateway-fcitx-e2e", disabled[] = "--disable=all",
             enabled[] = "--enable=testfrontend,testui,rimes";
        char* args[] = {name, disabled, enabled};
        fcitx::Instance instance(3, args);
        instance.addonManager().registerDefaultLoader(nullptr);
        SessionFixture session(instance.eventLoop(), true);
        fcitx::EventDispatcher dispatcher;
        dispatcher.attach(&instance.eventLoop());
        std::thread driver;
        std::unique_ptr<fcitx::EventSourceTime> timer;
        auto main = [&](std::function<void()> action) {
            auto promise = std::make_shared<std::promise<void>>();
            auto done = promise->get_future();
            dispatcher.schedule([action, promise] {
                try {
                    action();
                    promise->set_value();
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
            Check(done.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                  "Fcitx event loop blocked");
            done.get();
        };
        dispatcher.schedule([&] {
            auto& manager = instance.inputMethodManager();
            Check(manager.entry("rimes"), "rimes addon missing");
            if (manager.groupCount() == 0) {
                manager.addEmptyGroup("Default");
                manager.setGroupOrder({"Default"});
            }
            fcitx::InputMethodGroup group("Default");
            group.setDefaultLayout("us");
            group.inputMethodList().emplace_back(fcitx::InputMethodGroupItem("rimes"));
            group.setDefaultInputMethod("rimes");
            manager.setGroup(group);
            manager.setCurrentGroup("Default");
            auto* frontend = instance.addonManager().addon("testfrontend", true);
            Check(frontend, "frontend missing");
            auto uuid = frontend->call<fcitx::ITestFrontend::createInputContext>("gateway-fixture");
            auto* ic = instance.inputContextManager().findByUUID(uuid);
            Check(ic, "input context missing");
            instance.setCurrentInputMethod(ic, "rimes", true);
            ic->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
            ic->focusIn();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
            timer = instance.eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 50000, 0,
                [&, frontend, uuid, ic, deadline](fcitx::EventSourceTime* event, uint64_t) {
                    try {
                        auto* engine = instance.inputMethodEngine(ic);
                        Check(std::chrono::steady_clock::now() < deadline,
                              "addon deployment/start deadline");
                        if (!engine ||
                            engine->subMode(*manager.entry("rimes"), *ic) == "Deploying" ||
                            Status(root).at("state") != "running") {
                            event->setTime(fcitx::now(CLOCK_MONOTONIC) + 50000);
                            event->setOneShot();
                            return true;
                        }
                        driver = std::thread([&, frontend, uuid, ic] {
                            try {
                                auto wait = [&](const char* expected) {
                                    const auto until =
                                        std::chrono::steady_clock::now() + std::chrono::seconds(5);
                                    while (Status(root).at("state") != expected) {
                                        Check(std::chrono::steady_clock::now() < until,
                                              "gateway addon lifecycle deadline");
                                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                    }
                                };
                                const unsigned port = config.at("gatewayPort");
                                const auto token = rt::GatewayAccess(root / "gateway").Current();
                                Client client(port, token), slow(port, token);
                                slow.Send("POST /v1/inbound HTTP/1.1\r\nContent-Length: 100\r\n");
                                // The same loaded addon must still deliver normal Chinese while a
                                // request stalls.
                                main([&] {
                                    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>(
                                        "你好");
                                    for (const auto* key : {"n", "i", "h", "a", "o", "space"})
                                        Key(frontend, uuid, key);
                                });
                                GatewayRequest push{"POST",
                                                    "/v1/inbound",
                                                    Json{{"source", "public-addon-fixture"},
                                                         {"text", "仅待审核，不自动上屏"},
                                                         {"format", "markdown"}}
                                                        .dump(),
                                                    {{"content-type", "application/json"}}};
                                Check(client.Call(push).status == 200,
                                      "actual addon refused receipt");
                                const auto received = Store(root / "mailbox").Read().document;
                                Check(received.at("threads").size() == 1 &&
                                          received.at("threads")[0].at("review").at("state") ==
                                              "pending",
                                      "actual addon skipped review");
                                if (std::filesystem::exists(root / "buffer.json")) {
                                    auto buffer = Json::parse(std::ifstream(root / "buffer.json"));
                                    Check(!buffer.at("visible").get<bool>() &&
                                              !buffer.at("capturing").get<bool>(),
                                          "receipt opened/captured Buffer");
                                }
                                main([&] {
                                    ic->setCapabilityFlags(
                                        fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit} |
                                        fcitx::CapabilityFlag::Password);
                                });
                                wait("protected");
                                Check(Store(root / "mailbox").Read().document == received,
                                      "password state changed receipt");
                                main([&] {
                                    ic->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
                                });
                                wait("running");
                                main([&] { session.Saver(true); });
                                wait("protected");
                                main([&] { session.Saver(false); });
                                wait("running");
                                auto next = configuration.Load();
                                next["gatewayEnabled"] = false;
                                configuration.Save(next, next.at("revision"));
                                wait("disabled");
                                next = configuration.Load();
                                next["gatewayEnabled"] = true;
                                configuration.Save(next, next.at("revision"));
                                wait("running");
                                rimes::plugins::PackageStore packages(
                                    root / "plugins", rimes::plugins::OfficialCatalog(),
                                    rimes::plugins::BundledPackages(),
                                    rimes::plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
                                auto grant = packages.Inspect("builtin.mailbox.inbox");
                                packages.Enable(grant.id, false, grant.grant);
                                wait("module-disabled");
                                main([&] {
                                    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>(
                                        "你好");
                                    for (const auto* key : {"n", "i", "h", "a", "o", "space"})
                                        Key(frontend, uuid, key);
                                });
                                grant = packages.Inspect(grant.id);
                                packages.Enable(grant.id, true, grant.grant);
                                wait("running");
                                Check(Store(root / "mailbox").Read().document == received,
                                      "revocation or reopen mutated receipt");
                                std::cout << "Actual Fcitx addon: pending receipt with no Mailbox "
                                             "window, normal commitString while slow client, "
                                             "password/session/config/plugin revocation passed\n";
                            } catch (const std::exception& e) {
                                std::cerr << e.what() << '\n';
                                result = 1;
                            }
                            dispatcher.schedule([&] { instance.exit(); });
                        });
                        return true;
                    } catch (const std::exception& e) {
                        std::cerr << e.what() << '\n';
                        result = 1;
                        instance.exit();
                        return true;
                    }
                });
        });
        try {
            instance.exec();
        } catch (const fcitx::InstanceQuietQuit&) {
        }
        if (driver.joinable())
            driver.join();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    if (!result)
        std::filesystem::remove_all(root);
    return result;
}
