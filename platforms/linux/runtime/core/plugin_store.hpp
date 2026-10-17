#pragma once
#include "plugin_registry.hpp"
#include "private_files.hpp"
#include <nlohmann/json.hpp>
#include <functional>

namespace rimes::plugins {
using Json = nlohmann::json;

struct PackageView {
    std::string id, name, version, kind, grant, error;
    bool available = false, installed = false, enabled = false, bundled = false;
};

// Catalog and bundled bytes come from the pinned build dependency. A JSON
// package can only configure an adapter already compiled into this host.
class PackageStore {
public:
    static constexpr std::size_t kPackageLimit = 256 * 1024;
    PackageStore(std::filesystem::path root, Json catalog,
                 std::map<std::string, std::string> bundled,
                 std::map<std::string, Descriptor> adapters,
                 std::string host_version);
    std::vector<PackageView> Entries() const;
    PackageView Inspect(const std::string& id) const;
    Json Package(const std::string& id) const;
    // Linearize a local host operation with enable/disable/update. The action
    // must not call back into this store or acquire the package lock again.
    void WithEnabledGrant(const std::string& id, const std::string& grant,
                          const std::function<void()>& action) const;
    void Install(const std::string& id, std::string_view bytes, const std::string& expected_grant);
    void RestoreBundled(const std::string& id, const std::string& expected_grant);
    void Enable(const std::string& id, bool enabled, const std::string& expected_grant);
    void Uninstall(const std::string& id, const std::string& expected_grant);
    std::string DownloadURL(const std::string& id) const;
    const std::filesystem::path& Path() const { return files_.Path(); }

private:
    const Json& Entry(const std::string& id) const;
    Json State(const std::string& id) const;
    Json Verified(const std::string& id, std::string_view bytes) const;
    std::string Bytes(const std::string& id, const Json& state) const;
    Json CheckedState(const std::string& id, const std::string& expected_grant) const;
    void WriteState(const std::string& id, Json state) const;
    bool Available(const std::string& id) const;
    void Seed();

    runtime::PrivateDirectory files_;
    Json catalog_;
    std::map<std::string, std::string> bundled_;
    std::map<std::string, Descriptor> adapters_;
    std::string host_version_;
};
} // namespace rimes::plugins
