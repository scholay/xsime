#pragma once
#include "private_files.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>
namespace rimes::runtime {
inline std::vector<std::string> TranslationCommand() {
    auto resources = std::filesystem::path(RIMES_INSTALLED_TRANSLATION_RESOURCES);
    if (!std::filesystem::is_regular_file(resources / "runtime.py")) resources = RIMES_BUILD_TRANSLATION_RESOURCES;
    const auto python = access("/usr/bin/python3.12", X_OK) == 0 ? "/usr/bin/python3.12" : RIMES_TRANSLATION_PYTHON;
    PrivateDirectory root(UserRoot() / "translation");
    return {python, "-I", (resources / "runtime.py").string(), "--root", root.Path().string()};
}
}
