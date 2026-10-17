#include "buffer/buffer_service.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace rimes::buffer;
void Check(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
int main() {
    char pattern[] = "/tmp/xsime-derived-host-XXXXXX";
    const char* path = mkdtemp(pattern);
    if (!path) return 1;
    const auto root = std::filesystem::path(path);
    setenv("RIMES_BUFFER_SOCKET", (root / "buffer.sock").c_str(), 1);
    setenv("RIMES_BUFFER_HEADLESS", "1", 1);
    const auto dump_path = root / "snapshot.json";
    setenv("RIMES_BUFFER_DUMP", dump_path.c_str(), 1);
    std::vector<std::function<void()>> posted;
    auto drain = [&] { while (!posted.empty()) { auto action = std::move(posted.front()); posted.erase(posted.begin()); action(); } };
    fcitx::BufferService::ResultFn late;
    try {
        rimes::plugins::Registry plugins;
        plugins.Register({"builtin.fixture", "Fixture", "1", rimes::plugins::Kind::Buffer, {"ai.generate"}});
        plugins.ApplyState("builtin.fixture", true, true);
        rimes::runtime::ConfigurationStore config(root / "config");
        const auto revision = config.Load().at("revision").get<std::string>();
        {
            fcitx::BufferService host(nullptr, [&](auto action) { posted.push_back(std::move(action)); });
            bool unavailable_configuration = false;
            host.Configure(false, false, 760, revision, "night");
            host.BindPlugins(plugins, [&](const auto& lease) { return plugins.Accepts(lease); },
                [&](std::string_view id) {
                    if (unavailable_configuration) throw std::runtime_error("fixture configuration unavailable");
                    return config.Freeze(id);
                });
            std::shared_ptr<std::atomic<bool>> cancelled;
            DerivedRequest frozen;
            host.RegisterDerivedAdapter("builtin.fixture", "ai.generate", [&](auto request, auto settings, auto stop, auto receive) {
                Check(settings.revision == revision, "configuration was not frozen");
                frozen = std::move(request); cancelled = stop; late = std::move(receive);
            });
            auto prepare = [&] {
                plugins.ApplyState("builtin.fixture", true, true);
                Check(host.SelectPlugin("builtin.fixture"), "plugin selection");
                host.Configure(false, false, 760, revision, "night");
                host.model().set_password_field(false);
                host.model().activate_capture("fixture-field");
                host.model().set_visible(true);
                if (host.model().empty()) host.model().append("原文🙂", Origin::Rime);
                host.Publish();
                Check(host.Generate(), "generation unavailable");
            };
            prepare();
            Check(frozen.text == "原文🙂", "source not frozen");
            late({0, "Translated", false, true}); drain();
            Check(host.derived().Phase() == DerivedPhase::Running && host.derived().CopyText().empty(), "stream became sendable");
            late({1, "Alternative", false, true}); drain();
            late({0, "Translated🙂", true, true}); drain();
            Check(host.derived().CopyText() == "Translated🙂" && host.derived().Results().size() == 2, "results not retained");
            Check(host.model().staged_text() == "原文🙂", "generation consumed source");
            unavailable_configuration = true;
            Check(!host.Generate(), "invalid retry started");
            Check(host.derived().CopyText() == "Translated🙂", "failed retry erased ready results");
            unavailable_configuration = false;
            prepare();
            late({0, "private preview", false, true}); drain();
            plugins.ApplyState("builtin.fixture", true, false);
            // No intervening Publish: rejection itself must clear the UI.
            late({0, "revoked preview", true, true}); drain();
            std::ifstream snapshot_file(dump_path);
            const std::string published((std::istreambuf_iterator<char>(snapshot_file)), std::istreambuf_iterator<char>());
            Snapshot snapshot;
            Check(ParseSnapshot(published, &snapshot) && snapshot.results.empty() && snapshot.result_blocks.empty() &&
                  snapshot.phase != DerivedPhase::Running, "revoked AI preview remained in emitted UI snapshot");
            for (int scenario = 0; scenario < 7; ++scenario) {
                prepare();
                const auto previous = late;
                if (scenario == 0) host.CancelDerived();
                if (scenario == 1) host.CloseAndPause();
                if (scenario == 2) host.model().append("更改", Origin::Local);
                if (scenario == 3) host.Configure(false, false, 760, "replaced", "day");
                if (scenario == 4) plugins.ApplyState("builtin.fixture", true, false);
                if (scenario == 5) host.OnPasswordField(nullptr, true);
                if (scenario == 6) Check(host.SelectPlugin(""), "return default");
                host.Publish();
                if (scenario == 5) Check(host.model().visible(), "protected workbench lost its visible chrome");
                Check(cancelled->load(), "worker was not cancelled");
                previous({0, "late", true, true}); drain();
                Check(host.derived().Results().empty(), "late result regained authority");
            }
            prepare();
            late({0, "partial", false, true}); drain();
            late({0, "", true, false}); drain();
            Check(host.derived().Phase() == DerivedPhase::Failed && host.derived().Results().empty(), "failed worker result survived");
            prepare();
            late({0, "partial", false, true}); drain();
            late({0, "\xED\xA0\x80", true, true}); drain();
            Check(host.derived().Phase() == DerivedPhase::Failed, "malformed output published partial success");
            prepare();
            const auto before_lock = late;
            host.OnSessionProtection(true);
            Check(cancelled->load() && host.model().secure() && !host.model().visible(), "lock did not cancel/hide");
            Check(!host.model().staged_text().empty(), "lock erased source");
            host.OnPasswordField(nullptr, false);
            Check(host.model().secure(), "field update overrode session protection");
            before_lock({0, "late under lock", true, true}); drain();
            Check(host.derived().Results().empty(), "lock accepted late output");
            host.OnSessionProtection(false);
            Check(host.model().visible() && !host.model().capture_enabled() && !host.Generate(), "unlock reused processing/target");
            before_lock({0, "late after unlock", true, true}); drain();
            Check(host.derived().Results().empty(), "unlock accepted pre-lock output");
            host.ShowAndCapture(nullptr);
            Check(host.Generate(), "explicit fresh detached request was unavailable");
            // callback after host destruction must be harmless
        }
        late({0, "after shutdown", true, true}); drain();
        std::cout << "Derived host: frozen configuration/source, alternatives, revocation, failures and shutdown passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
    std::filesystem::remove_all(root);
}
