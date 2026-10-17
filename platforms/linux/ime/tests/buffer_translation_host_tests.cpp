#include "buffer/buffer_service.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace rimes::buffer;
void Check(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
int main() {
    char pattern[] = "/tmp/xsime-translation-host-XXXXXX";
    const auto* root = mkdtemp(pattern);
    if (!root) return 1;
    setenv("RIMES_BUFFER_SOCKET", (std::filesystem::path(root) / "buffer.sock").c_str(), 1);
    setenv("RIMES_BUFFER_HEADLESS", "1", 1);
    const auto dump_path = std::filesystem::path(root) / "snapshot.json";
    setenv("RIMES_BUFFER_DUMP", dump_path.c_str(), 1);
    std::vector<std::function<void()>> posted;
    auto drain = [&] { while (!posted.empty()) { auto fn = std::move(posted.front()); posted.erase(posted.begin()); fn(); } };
    try {
        rimes::plugins::Registry registry;
        constexpr auto plugin = "builtin.apple-translation";
        registry.Register({plugin, "Translation", "2.3.0", rimes::plugins::Kind::Buffer, {"translation.generate"}});
        registry.ApplyState(plugin, true, true);
        rimes::runtime::ConfigurationStore config(std::filesystem::path(root) / "config");
        auto revision = config.Load().at("revision").get<std::string>();
        fcitx::BufferService host(nullptr, [&](auto fn) { posted.push_back(std::move(fn)); });
        bool invalid_grant = false;
        host.BindPlugins(registry, [&](const auto& lease) { if (invalid_grant) throw std::runtime_error("Unavailable permission"); return registry.Accepts(lease); },
                         [&](auto id) { return config.Freeze(id); });
        std::vector<DerivedRequest> requests;
        std::vector<fcitx::BufferService::ResultFn> replies;
        std::vector<std::shared_ptr<std::atomic<bool>>> cancellations;
        host.RegisterDerivedAdapter(plugin, "translation.generate", [&](auto request, auto frozen, auto cancelled, auto reply) {
            Check(frozen.plugin.at("sourceLanguage") == "zh-Hans", "frozen source language");
            requests.push_back(request); replies.push_back(reply); cancellations.push_back(cancelled);
        });
        host.Configure(false, false, 760, revision, "night");
        host.model().activate_capture("first-field"); host.model().set_visible(true);
        host.model().append("第一句。第二", Origin::Rime);
        Check(host.SelectPlugin(plugin) && host.Generate(), "translation selection and generation");
        Check(requests[0].text == "第一句。", "first sentence owns exact source");
        replies[0]({0, "First.", true, true}); drain();
        Check(host.translation().Preview() == "First. ", "ready prefix visible");
        Check(host.Generate() && requests[1].text == "第二", "pending tail generation");
        host.model().append("句。", Origin::Rime); host.Publish();
        Check(cancellations[1]->load(), "editing the running tail cancels it");
        Check(host.translation().Preview() == "First. ", "stable ready prefix survives tail edit");
        replies[1]({0, "stale tail", true, true}); drain();
        Check(host.translation().Preview() == "First. ", "stale tail callback rejected");
        Check(host.Generate() && requests[2].text == "第二句。", "only changed sentence is regenerated");
        replies[2]({0, "Second.", true, true}); drain();
        Check(host.translation().Complete() && host.translation().Preview() == "First. Second. ", "whole translation completes");
        Check(host.model().staged_text() == "第一句。第二句。", "generation never consumes source");
        host.model().append("第三句", Origin::Rime); host.Publish(); host.Generate();
        const auto old = replies.back(); const auto old_cancel = cancellations.back();
        host.CancelDerived();
        Check(old_cancel->load(), "explicit cancellation stops worker");
        Check(host.Generate(), "cancelled unit can be retried");
        const auto running_count = replies.size();
        Check(!host.Generate() && !host.TranslationTick(), "running unit was replaced by generate/debounce");
        Check(replies.size() == running_count, "duplicate worker escaped ownership");
        old({0, "late after retry", true, true}); drain();
        Check(host.translation().Running(), "old request cannot finish replacement");
        replies.back()({0, "", true, false}); drain();
        Check(host.translation().Failed() && host.translation().Preview() == "First. Second. ", "failure keeps successful prefix");
        auto settings = config.Load(); settings["plugins"][plugin] = {{"continuous", false}};
        config.Save(settings, revision); revision = config.Load().at("revision").get<std::string>();
        host.Configure(false, false, 760, revision, "night");
        Check(host.translation().Preview().empty(), "configuration change revokes old output");
        Check(!host.TranslationTick(), "continuous off prevents automatic requests");
        Check(host.Generate(), "manual requests remain available");
        const auto private_reply = replies.back();
        host.OnPasswordField(nullptr, true);
        private_reply({0, "must be hidden", true, true}); drain();
        Check(host.translation().All().empty(), "protection scrubs plaintext and old results");
        host.model().set_password_field(false); host.model().activate_capture("new-field"); host.model().set_visible(true);
        host.model().append("新原文。后半句", Origin::Rime); host.Publish(); host.Generate();
        replies.back()({0, "Visible prefix.", true, true}); drain();
        Check(host.Generate(), "revocation fixture tail did not start");
        invalid_grant = true;
        replies.back()({0, "invalid permission", true, true}); drain();
        Check(host.translation().All().empty(), "permission error closes authorization without escaping event loop");
        std::ifstream file(dump_path);
        const std::string published((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        Snapshot snapshot;
        Check(ParseSnapshot(published, &snapshot) && snapshot.results.empty() && snapshot.result_blocks.empty() &&
              snapshot.phase != DerivedPhase::Running, "revoked translation remained in published UI snapshot");
        std::cout << "Translation host: incremental reuse, exact source, cancel/retry, configuration and protection passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::filesystem::remove_all(root); return 1; }
    std::filesystem::remove_all(root);
}
