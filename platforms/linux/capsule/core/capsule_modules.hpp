#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
namespace rimes::capsule {
enum class Module { Temporary, Capture, Notes, Resources, Passwords };
inline constexpr Module kModules[] = {Module::Temporary, Module::Capture, Module::Notes, Module::Resources, Module::Passwords};
const char* ModuleRaw(Module module);
std::optional<Module> ModuleFromRaw(std::string_view raw);
const nlohmann::json& ModuleContract();
const nlohmann::json& ModuleDefinition(Module module);
bool ValidFilter(Module module, std::string_view filter);
std::string ModulePlugin(Module module);
}
