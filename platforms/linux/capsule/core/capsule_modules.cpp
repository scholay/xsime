#include "capsule_modules.hpp"
#include "native_capsule_contract.hpp"
namespace rimes::capsule {
const char* ModuleRaw(Module module) {
    switch (module) {
    case Module::Temporary: return "temporary";
    case Module::Capture: return "capture";
    case Module::Notes: return "notes";
    case Module::Resources: return "resources";
    case Module::Passwords: return "passwords";
    }
    return "notes";
}
std::optional<Module> ModuleFromRaw(std::string_view raw) {
    for (auto module : kModules) if (raw == ModuleRaw(module)) return module;
    return std::nullopt;
}
const nlohmann::json& ModuleContract() {
    static const auto value = nlohmann::json::parse(native::kContract);
    return value;
}
const nlohmann::json& ModuleDefinition(Module module) {
    return ModuleContract().at("modules").at(static_cast<std::size_t>(module));
}
bool ValidFilter(Module module, std::string_view filter) {
    for (const auto& item : ModuleDefinition(module).at("filters")) if (item.at("id") == filter) return true;
    return false;
}
std::string ModulePlugin(Module module) { return "builtin.capsule." + std::string(ModuleRaw(module)); }
}
