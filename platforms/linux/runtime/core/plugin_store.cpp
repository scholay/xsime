#include "plugin_store.hpp"
#include <array>
#include <charconv>
#include <regex>
#include <stdexcept>

namespace rimes::plugins {
namespace {
std::array<unsigned, 3> Version(const std::string& text) {
    static const std::regex pattern("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)");
    std::smatch match;
    if (!std::regex_match(text, match, pattern)) throw std::runtime_error("Invalid plugin version");
    std::array<unsigned, 3> version{};
    for (std::size_t i = 0; i < version.size(); ++i) {
        const auto part = match[i + 1].str();
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), version[i]);
        if (parsed.ec != std::errc{}) throw std::runtime_error("Invalid plugin version");
    }
    return version;
}
bool Hex(const std::string& value, std::size_t size) {
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool Id(const std::string& id) {
    static const std::regex pattern("builtin\\.[a-z0-9]+([.-][a-z0-9]+)*");
    return id.size() <= 128 && std::regex_match(id, pattern);
}
std::string Filename(const std::string& id, const Json& state) {
    const auto hash = state.at("sha256").get<std::string>();
    if (!Id(id) || !Hex(hash, 64)) throw std::runtime_error("Invalid package identity");
    return id + "." + hash + ".json";
}
void ValidateContribution(const Json& value) {
    const auto& content = value.at("contribution");
    const auto type = content.at("type").get<std::string>();
    const auto kind = value.at("kind").get<std::string>();
    static const std::map<std::string, std::pair<std::string, std::set<std::string>>> types{
        {"translation.v1", {"buffer", {"buffer.read", "translation.generate"}}},
        {"ai.channel.v1", {"buffer", {"buffer.read", "ai.generate"}}},
        {"ai.prompt.v1", {"buffer", {"buffer.read", "ai.generate"}}},
        {"stream.pinyin.v1", {"buffer", {"buffer.read", "ai.generate"}}},
        {"input.chord.v1", {"extension", {"input.chord"}}},
        {"metrics.v1", {"extension", {"metrics.aggregate"}}},
        {"typing.practice.v1", {"extension", {"metrics.aggregate"}}}
    };
    const auto caps = value.at("capabilities").get<std::set<std::string>>();
    if (type == "host.module.v1") {
        const auto& options = content.at("options");
        const auto module = options.at("module").get<std::string>();
        const std::set<std::string> capsules{"temporary", "capture", "notes", "resources", "passwords"};
        const std::set<std::string> mailboxes{"terminal", "chat", "inbox"};
        if (options.at("host") != kind || caps != std::set<std::string>{kind + ".navigate"} ||
            !((kind == "capsule" && capsules.count(module)) || (kind == "mailbox" && mailboxes.count(module))))
            throw std::runtime_error("Unsupported host contribution");
    } else {
        const auto expected = types.find(type);
        if (expected == types.end() || expected->second.first != kind || expected->second.second != caps)
            throw std::runtime_error("Unsupported plugin contribution");
    }
}
}

PackageStore::PackageStore(std::filesystem::path root, Json catalog,
    std::map<std::string, std::string> bundled, std::map<std::string, Descriptor> adapters,
    std::string host_version)
    : files_(root), catalog_(std::move(catalog)), bundled_(std::move(bundled)),
      adapters_(std::move(adapters)), host_version_(std::move(host_version)) {
    (void)Version(host_version_);
    if (catalog_.at("schemaVersion") != 1 || !catalog_.at("plugins").is_array())
        throw std::runtime_error("Unsupported plugin catalog");
    std::set<std::string> ids;
    for (const auto& entry : catalog_.at("plugins")) {
        const auto id = entry.at("id").get<std::string>();
        if (!Id(id) || !ids.insert(id).second || !Hex(entry.at("sha256"), 64))
            throw std::runtime_error("Invalid plugin catalog identity");
        (void)Version(entry.at("version"));
    }
    Seed();
}

const Json& PackageStore::Entry(const std::string& id) const {
    for (const auto& entry : catalog_.at("plugins")) if (entry.at("id") == id) return entry;
    throw std::runtime_error("Unknown official plugin");
}

bool PackageStore::Available(const std::string& id) const {
    const auto& entry = Entry(id);
    return adapters_.count(id) && entry.at("platforms").contains("linux");
}

void PackageStore::Seed() {
    auto lock = files_.Lock();
    for (const auto& entry : catalog_.at("plugins")) {
        const auto id = entry.at("id").get<std::string>();
        // Unsupported entries have no local receipt. A later native port can
        // then apply its bundled defaults without mistaking a placeholder for
        // a deliberate user uninstall.
        if (!Available(id)) continue;
        if (files_.Read(id + ".state.json", 8192)) continue;
        const bool bundled = Available(id) && entry.at("platforms").at("linux").at("distribution") == "bundled";
        if (bundled) (void)Verified(id, bundled_.at(id));
        WriteState(id, {{"installed", bundled}, {"enabled", bundled && id != "builtin.fly-chord-learning"},
                       {"bundled", bundled}, {"sha256", entry.at("sha256")}});
    }
}

Json PackageStore::State(const std::string& id) const {
    (void)Entry(id);
    const auto bytes = files_.Read(id + ".state.json", 8192);
    if (!bytes) throw std::runtime_error("Plugin state unavailable");
    auto state = Json::parse(*bytes);
    if (!state.at("installed").is_boolean() || !state.at("enabled").is_boolean() || !state.at("bundled").is_boolean() ||
        !Hex(state.at("grant"), 32) || !Hex(state.at("sha256"), 64) ||
        (state.at("enabled").get<bool>() && !state.at("installed").get<bool>()))
        throw std::runtime_error("Invalid plugin state");
    return state;
}

Json PackageStore::Verified(const std::string& id, std::string_view bytes) const {
    const auto& entry = Entry(id);
    if (!Available(id)) throw std::runtime_error("This plugin has no compiled Linux adapter");
    if (bytes.size() > kPackageLimit || runtime::Sha256(bytes) != entry.at("sha256"))
        throw std::runtime_error("Plugin checksum mismatch");
    auto value = Json::parse(bytes);
    if (value.at("schemaVersion") != 2 || value.at("sdkVersion") != 1 || value.at("runtime") != "host-interpreted" ||
        value.at("id") != id || value.at("version") != entry.at("version") ||
        value.at("capabilities") != entry.at("capabilities") || !value.at("platforms").contains("linux") ||
        Version(host_version_) < Version(value.at("minimumHostVersion")))
        throw std::runtime_error("Incompatible plugin package");
    const auto& adapter = adapters_.at(id);
    if (value.at("kind") != KindName(adapter.kind) ||
        value.at("capabilities").get<std::set<std::string>>() != adapter.capabilities)
        throw std::runtime_error("Unsupported plugin capabilities");
    ValidateContribution(value);
    return value;
}

std::string PackageStore::Bytes(const std::string& id, const Json& state) const {
    if (!state.at("installed").get<bool>()) throw std::runtime_error("Plugin is not installed");
    if (state.at("sha256") != Entry(id).at("sha256")) throw std::runtime_error("Plugin update requires confirmation");
    if (state.at("bundled").get<bool>()) return bundled_.at(id);
    auto bytes = files_.Read(Filename(id, state), kPackageLimit);
    if (!bytes) throw std::runtime_error("Installed plugin package is missing");
    return *bytes;
}

PackageView PackageStore::Inspect(const std::string& id) const {
    const auto& entry = Entry(id);
    PackageView view;
    view.id = id; view.name = entry.at("nameZH"); view.version = entry.at("version"); view.kind = entry.at("kind");
    view.available = Available(id);
    if (!view.available) { view.error = "此插件尚无 Linux 适配"; return view; }
    try {
        const auto state = State(id);
        view.grant = state.at("grant"); view.installed = state.at("installed"); view.bundled = state.at("bundled");
        if (view.installed) (void)Verified(id, Bytes(id, state));
        view.enabled = view.installed && state.at("enabled").get<bool>();
        if (!view.available) view.error = "此插件尚无 Linux 适配";
    } catch (const std::exception&) { view.enabled = false; view.error = "插件不可用，请重新安装或更新"; }
    return view;
}

std::vector<PackageView> PackageStore::Entries() const {
    std::vector<PackageView> result;
    for (const auto& entry : catalog_.at("plugins")) result.push_back(Inspect(entry.at("id")));
    return result;
}

Json PackageStore::Package(const std::string& id) const {
    const auto state = State(id);
    if (!state.at("enabled").get<bool>()) throw std::runtime_error("Plugin is disabled");
    return Verified(id, Bytes(id, state));
}

void PackageStore::WithEnabledGrant(const std::string& id, const std::string& grant,
                                    const std::function<void()>& action) const {
    auto lock = files_.Lock();
    const auto state = CheckedState(id, grant);
    if (!state.at("enabled").get<bool>()) throw std::runtime_error("Plugin is disabled");
    (void)Verified(id, Bytes(id, state));
    action();
}

Json PackageStore::CheckedState(const std::string& id, const std::string& expected_grant) const {
    auto state = State(id);
    if (state.at("grant") != expected_grant) throw std::runtime_error("Plugin changed; refresh and retry");
    return state;
}

void PackageStore::WriteState(const std::string& id, Json state) const {
    state["grant"] = runtime::RandomID();
    files_.Write(id + ".state.json", state.dump());
}

void PackageStore::Install(const std::string& id, std::string_view bytes, const std::string& expected_grant) {
    (void)Verified(id, bytes); // no mutation on validation/download failure
    auto lock = files_.Lock();
    (void)CheckedState(id, expected_grant);
    Json next{{"installed", true}, {"enabled", false}, {"bundled", false}, {"sha256", Entry(id).at("sha256")}};
    files_.Write(Filename(id, next), bytes);
    WriteState(id, next); // single publication point; old content remains recoverable
}

void PackageStore::Enable(const std::string& id, bool enabled, const std::string& expected_grant) {
    auto lock = files_.Lock();
    auto state = CheckedState(id, expected_grant);
    if (enabled) (void)Verified(id, Bytes(id, state));
    if (!state.at("installed").get<bool>()) throw std::runtime_error("Install this plugin first");
    if (state.at("enabled") == enabled) return;
    state["enabled"] = enabled;
    WriteState(id, state);
}

void PackageStore::RestoreBundled(const std::string& id, const std::string& expected_grant) {
    if (!Available(id) || Entry(id).at("platforms").at("linux").at("distribution") != "bundled")
        throw std::runtime_error("Plugin is not bundled");
    (void)Verified(id, bundled_.at(id));
    auto lock = files_.Lock();
    (void)CheckedState(id, expected_grant);
    WriteState(id, {{"installed", true}, {"enabled", false}, {"bundled", true}, {"sha256", Entry(id).at("sha256")}});
}

void PackageStore::Uninstall(const std::string& id, const std::string& expected_grant) {
    auto lock = files_.Lock();
    auto state = CheckedState(id, expected_grant);
    const bool downloaded = state.at("installed").get<bool>() && !state.at("bundled").get<bool>();
    const auto previous = state;
    state["installed"] = false; state["enabled"] = false;
    WriteState(id, state);
    if (downloaded) files_.Remove(Filename(id, previous));
}

std::string PackageStore::DownloadURL(const std::string& id) const {
    if (!Available(id)) throw std::runtime_error("This plugin has no compiled Linux adapter");
    const auto url = Entry(id).at("downloadURL").get<std::string>();
    constexpr std::string_view prefix = "https://github.com/scholay/rimes-plugins/releases/download/";
    if (url.compare(0, prefix.size(), prefix) != 0 || url.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("Invalid official plugin download URL");
    return url;
}
} // namespace rimes::plugins
