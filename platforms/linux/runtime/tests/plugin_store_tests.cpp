#include "plugin_store.hpp"
#include "package_binding.hpp"
#include "http_download.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace rimes::plugins;
using namespace rimes::runtime;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void Reject(F operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, "unsafe operation was accepted");
}
Json Manifest(std::string version = "1.0.0") {
    return {{"id", "builtin.openai-compatible"}, {"nameZH", "AI API"}, {"version", version},
        {"kind", "buffer"}, {"schemaVersion", 2}, {"sdkVersion", 1}, {"minimumHostVersion", "1.1.0"},
        {"runtime", "host-interpreted"}, {"platforms", {{"linux", {{"distribution", "download"}}}}},
        {"capabilities", {"buffer.read", "ai.generate"}},
        {"contribution", {{"type", "ai.channel.v1"}, {"options", {{"channel", "openai-compatible"}}}}}};
}
Json Catalog(Json manifest) {
    auto entry = manifest;
    entry["sha256"] = Sha256(manifest.dump());
    entry["downloadURL"] = "https://github.com/scholay/rimes-plugins/releases/download/v1.1.0/test.json";
    return {{"schemaVersion", 1}, {"plugins", Json::array({entry})}};
}

int main() {
    char name[] = "/tmp/xsime-plugin-store-XXXXXX";
    const char* created = mkdtemp(name);
    Check(created, "cannot create fixture");
    const std::filesystem::path root(created);
    const std::string id = "builtin.openai-compatible";
    const std::map<std::string, Descriptor> adapters{{id, {id, "AI API", "1.0.0", Kind::Buffer, {"buffer.read", "ai.generate"}}}};
    try {
        auto manifest = Manifest();
        auto catalog = Catalog(manifest);
        PackageStore store(root / "plugins", catalog, {}, adapters, "1.1.5");
        auto view = store.Inspect(id);
        Check(view.available && !view.installed && !view.enabled, "optional plugin installed itself");
        const auto initial_grant = view.grant;
        Reject([&] { store.Install(id, "corrupt", initial_grant); });
        Check(store.Inspect(id).grant == initial_grant, "failed download revoked old state");
        Reject([&] { store.Enable(id, true, initial_grant); });
        store.Install(id, manifest.dump(), initial_grant);
        view = store.Inspect(id);
        Check(view.installed && !view.enabled && view.grant != initial_grant, "installation inherited permission");
        Reject([&] { store.Enable(id, true, initial_grant); });
        Reject([&] { (void)store.Package(id); });
        store.Enable(id, true, view.grant);
        const auto enabled_grant = store.Inspect(id).grant;
        int writes=0;
        store.WithEnabledGrants({{id,enabled_grant},{id,enabled_grant}},[&]{++writes;});
        Check(writes==1,"multiple grants did not publish once");
        Reject([&]{store.WithEnabledGrants({{id,enabled_grant},{id,"stale"}},[&]{++writes;});});
        Check(writes==1,"stale secondary grant published");
        {auto lock=PrivateDirectory(root/"plugins").Lock();Reject([&]{store.WithEnabledGrant(id,enabled_grant,[]{},false);});}
        Check(store.Package(id).at("contribution").at("options").at("channel") == "openai-compatible", "installed contribution missing");
        store.Enable(id, true, enabled_grant);
        Check(store.Inspect(id).grant == enabled_grant, "unchanged setting revoked jobs");
        {
            PackageStore restarted(root / "plugins", catalog, {}, adapters, "1.1.5");
            Check(restarted.Inspect(id).enabled && restarted.Inspect(id).grant == enabled_grant, "restart lost state");
        }
        // A stale settings window cannot overwrite an intervening uninstall.
        store.Uninstall(id, enabled_grant);
        Reject([&] { store.Enable(id, true, enabled_grant); });
        view = store.Inspect(id);
        Check(!view.installed && !view.enabled && view.grant != enabled_grant, "uninstall did not revoke authorization");
        store.Install(id, manifest.dump(), view.grant);
        Check(!store.Inspect(id).enabled, "reinstall silently re-enabled plugin");
        store.Enable(id, true, store.Inspect(id).grant);
        const auto original = store.Inspect(id);
        auto next = Manifest("1.0.1");
        PackageStore update(root / "plugins", Catalog(next), {}, adapters, "1.1.5");
        Check(!update.Inspect(id).enabled, "unreviewed updated catalog kept execution rights");
        Reject([&] { update.Install(id, "bad", original.grant); });
        Check(store.Inspect(id).enabled, "failed update destroyed previous version");
        update.Install(id, next.dump(), original.grant);
        Check(!update.Inspect(id).enabled && !store.Inspect(id).enabled, "update inherited old authorization");
        update.Enable(id, true, update.Inspect(id).grant);
        Check(update.Package(id).at("version") == "1.0.1", "upgrade did not publish new content");

        // Persistent user content lives outside the package directory.
        PrivateDirectory content(root / "content"); content.Write("learning.json", "keep");
        update.Uninstall(id, update.Inspect(id).grant);
        Check(content.Read("learning.json", 100) == "keep", "uninstall deleted user content");
        for (auto bad : {"../../escape", "/tmp/escape", "builtin.unknown"})
            Reject([&] { update.Install(bad, manifest.dump(), ""); });

        auto bundled = manifest;
        bundled["platforms"]["linux"]["distribution"] = "bundled";
        PackageStore preinstalled(root / "bundled", Catalog(bundled), {{id, bundled.dump()}}, adapters, "1.1.5");
        Check(preinstalled.Inspect(id).enabled, "bundled defaults missing");
        Registry live;
        int starts = 0, stops = 0;
        live.Register(adapters.at(id), {[&] { ++starts; }, [&] { ++stops; }});
        PackageBinding binding(preinstalled, live);
        Check(live.SelectBuffer(id), "bundled adapter not enabled in host");
        const auto old_job = live.Authorize(id, "ai.generate");
        Check(old_job && binding.Accepts(*old_job), "valid bundled job rejected");
        // Deliberately do not dispatch an inotify refresh before the result.
        preinstalled.Enable(id, false, preinstalled.Inspect(id).grant);
        Check(!binding.Accepts(*old_job) && live.SelectedBuffer().empty() && stops == 1,
              "late result bypassed persisted disable or did not stop the adapter");
        preinstalled.Enable(id, true, preinstalled.Inspect(id).grant);
        binding.Refresh();
        Check(starts == 2 && live.SelectBuffer(id), "explicit re-enable failed");
        const auto before_update = *live.Authorize(id, "ai.generate");
        preinstalled.Install(id, bundled.dump(), preinstalled.Inspect(id).grant);
        preinstalled.Enable(id, true, preinstalled.Inspect(id).grant);
        binding.Refresh();
        Check(starts == 3 && stops == 2 && !binding.Accepts(before_update), "same-state update kept old adapter alive");
        preinstalled.Uninstall(id, preinstalled.Inspect(id).grant);
        binding.Refresh();
        Check(!live.Find(id)->enabled && stops == 3, "uninstall kept live adapter enabled");
        PackageStore reboot(root / "bundled", Catalog(bundled), {{id, bundled.dump()}}, adapters, "1.1.5");
        Check(!reboot.Inspect(id).installed, "restart restored uninstalled built-in");
        reboot.RestoreBundled(id, reboot.Inspect(id).grant);
        Check(reboot.Inspect(id).installed && !reboot.Inspect(id).enabled && reboot.Inspect(id).bundled,
              "offline bundled restore inherited authorization");

        PackageStore unsupported(root / "unsupported", Catalog(manifest), {}, {}, "1.1.5");
        Check(!unsupported.Inspect(id).available, "catalog advertised uncompiled code");
        Reject([&] { unsupported.Install(id, manifest.dump(), unsupported.Inspect(id).grant); });
        auto future = manifest; future["sdkVersion"] = 99;
        PackageStore incompatible(root / "future", Catalog(future), {}, adapters, "1.1.5");
        Reject([&] { incompatible.Install(id, future.dump(), incompatible.Inspect(id).grant); });
        auto executable = manifest; executable["contribution"]["type"] = "shell.exec.v1";
        PackageStore arbitrary(root / "arbitrary", Catalog(executable), {}, adapters, "1.1.5");
        Reject([&] { arbitrary.Install(id, executable.dump(), arbitrary.Inspect(id).grant); });
        auto escalation = manifest; escalation["capabilities"].push_back("secret.read");
        PackageStore permissions(root / "permissions", Catalog(escalation), {}, adapters, "1.1.5");
        Reject([&] { permissions.Install(id, escalation.dump(), permissions.Inspect(id).grant); });

        PrivateDirectory files(root / "private");
        files.Write("test", "exact bytes");
        Check(files.Read("test", 32) == "exact bytes", "private file round-trip failed");
        Reject([&] { files.Read("test", 2); });
        struct stat status{};
        Check(!stat((root / "private/test").c_str(), &status) && (status.st_mode & 0777) == 0600, "private mode lost");
        std::filesystem::create_symlink(root / "content/learning.json", root / "private/link");
        Reject([&] { files.Read("link", 100); });
        Reject([&] { files.Write("link", "replace"); });
        Reject([&] { files.Remove("link"); });
        std::filesystem::create_directory_symlink(root / "private", root / "linked");
        Reject([&] { PrivateDirectory unsafe(root / "linked/child"); });
        Check(!std::filesystem::exists(root / "private/child"), "unsafe parent created child before validation");
        std::filesystem::create_hard_link(root / "private/test", root / "private/hard");
        Reject([&] { files.Read("hard", 100); });
        Reject([&] { files.Write("hard", "changed"); });
        Check(content.Read("learning.json", 100) == "keep", "unsafe path modified other content");
        chmod((root / "private").c_str(), 0755);
        Reject([&] { files.Write("exposed", "secret"); });
        Reject([&] { files.Read("test", 100); });
        chmod((root / "private").c_str(), 0700);
        Check(OfficialDownloadURL("https://github.com/scholay/rimes-plugins/releases/download/v1/a.json"), "official download rejected");
        for (auto bad : {"http://github.com/a", "https://github.com.evil.example/a", "file:///tmp/secret",
                         "https://user:secret@github.com/a", "https://github.com:8080/a", "https://example.org/a"}) {
            Check(!OfficialDownloadURL(bad), "unsafe download recipient accepted");
            Reject([&] { DownloadOfficialPackage(bad); });
        }
        // Fail locally at connection time without depending on internet state.
        setenv("https_proxy", "http://127.0.0.1:1", 1);
        setenv("no_proxy", "", 1);
        Reject([&] { DownloadOfficialPackage("https://github.com/scholay/rimes-plugins/releases/download/v1/missing.json"); });
        std::cout << "Package lifecycle, grants, atomic updates, compatibility and private paths passed\n";
    } catch (...) { std::filesystem::remove_all(root); throw; }
    std::filesystem::remove_all(root);
}
