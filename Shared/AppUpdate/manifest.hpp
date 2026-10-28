#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <regex>
#include <stdexcept>
#include <string>

namespace rimes::updates {
inline constexpr const char* kFeed = "https://github.com/scholay/xsime/releases/latest/download/update-manifest.json";
inline constexpr std::uint64_t kMaximumPackage = 512ull * 1024 * 1024;
inline std::array<unsigned, 3> Version(const std::string& value) {
    static const std::regex valid("(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})");
    if (!std::regex_match(value, valid)) throw std::runtime_error("Invalid release version");
    const auto a = value.find('.'), b = value.find('.', a + 1);
    return {static_cast<unsigned>(std::stoul(value.substr(0, a))), static_cast<unsigned>(std::stoul(value.substr(a + 1, b - a - 1))), static_cast<unsigned>(std::stoul(value.substr(b + 1)))};
}
struct Asset { std::string version, url, sha256; std::uint64_t size = 0; };
inline Asset Parse(const std::string& bytes, const std::string& platform) {
    if (bytes.size() > 1024 * 1024) throw std::runtime_error("Update metadata too large");
    const auto json = nlohmann::json::parse(bytes);
    if (json.at("schemaVersion") != 1) throw std::runtime_error("Unsupported update format");
    Asset result; result.version = json.at("version").get<std::string>();
    (void)Version(result.version);
    const auto& asset = json.at("platforms").at(platform);
    result.url = asset.at("url").get<std::string>(); result.sha256 = asset.at("sha256").get<std::string>();
    if (!asset.at("size").is_number_unsigned()) throw std::runtime_error("Invalid package size");
    result.size = asset.at("size").get<std::uint64_t>();
    const auto name = platform == "windows" ? "RIMES-Windows-" + result.version + "-Setup.exe"
        : platform == "linux" ? "fcitx5-rimes_" + result.version + "_amd64.deb" : "";
    if (name.empty() || result.url != "https://github.com/scholay/xsime/releases/download/v" + result.version + "/" + name ||
        result.size == 0 || result.size > kMaximumPackage || !std::regex_match(result.sha256, std::regex("[a-f0-9]{64}")))
        throw std::runtime_error("Untrusted update package metadata");
    if (platform == "linux" && (asset.at("architecture") != "amd64" || asset.at("distribution") != "ubuntu-24.04"))
        throw std::runtime_error("Unsupported Linux update target");
    if (platform == "windows" && asset.at("architecture") != "x64-x86") throw std::runtime_error("Unsupported Windows update target");
    return result;
}
inline void Verify(const Asset& asset, std::uint64_t size, const std::string& sha256) {
    if (asset.size != size || asset.sha256 != sha256) throw std::runtime_error("Package size/SHA-256 mismatch; download again");
}
inline std::string PackageName(const Asset& asset, const std::string& platform) {
    (void)Version(asset.version);
    if (!std::regex_match(asset.sha256, std::regex("[a-f0-9]{64}")))
        throw std::runtime_error("Invalid package identity");
    if (platform != "windows" && platform != "linux") throw std::runtime_error("Unsupported package platform");
    return "update-" + asset.version + "-" + asset.sha256 + (platform == "windows" ? ".exe" : ".deb");
}
inline void CheckVersionFloor(const Asset& asset, const std::string& installed, const std::string& highest) {
    const auto candidate = Version(asset.version);
    if (candidate < Version(installed) || (!highest.empty() && candidate < Version(highest)))
        throw std::runtime_error("Update metadata is older than an installed or previously verified release");
}
} // namespace rimes::updates
