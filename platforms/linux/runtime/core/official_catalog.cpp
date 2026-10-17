#include "official_catalog.hpp"
#include "official_plugin_catalog.hpp"

namespace rimes::plugins {
std::map<std::string, Descriptor> CompiledAdapters() {
    // An entry is added only with its native implementation and acceptance
    // tests. Platform declarations are a separate required gate in PackageStore.
    return {{"builtin.capsule.notes", {"builtin.capsule.notes", "笔记", "1.1.0", Kind::Capsule, {"capsule.navigate"}}},
        {"builtin.capsule.temporary", {"builtin.capsule.temporary", "临时", "1.1.0", Kind::Capsule, {"capsule.navigate"}}},
        {"builtin.capsule.capture", {"builtin.capsule.capture", "捕获", "1.1.0", Kind::Capsule, {"capsule.navigate"}}},
        {"builtin.capsule.resources", {"builtin.capsule.resources", "资源", "1.1.0", Kind::Capsule, {"capsule.navigate"}}},
        {"builtin.capsule.passwords", {"builtin.capsule.passwords", "密码", "1.1.0", Kind::Capsule, {"capsule.navigate"}}},
        {"builtin.apple-translation", {"builtin.apple-translation", "实时翻译", "2.3.0",
        Kind::Buffer, {"buffer.read", "translation.generate"}}},
        {"builtin.openai-compatible", {"builtin.openai-compatible", "AI API", "1.1.0",
        Kind::Buffer, {"buffer.read", "ai.generate"}}}};
}
Json OfficialCatalog() { return Json::parse(linuxruntime::official::kCatalogJSON); }
std::map<std::string, std::string> BundledPackages() {
    std::map<std::string, std::string> result;
    for (const auto& entry : linuxruntime::official::kPackages) result.emplace(entry.id, entry.bytes);
    return result;
}
} // namespace rimes::plugins
