#include "authentication.hpp"
#include "signature_verifier.hpp"
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Action> void Reject(Action action, const char* message) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
struct Key { std::string id, public_key_hex; };
void ManifestContracts() {
    using namespace rimes::updates;
    int count = 0;
    auto require = [&](bool result) { ++count; if (!result) throw std::runtime_error("Update contract regression"); };
    auto rejects = [&](const std::function<void()>& call) { bool threw = false; try { call(); } catch (...) { threw = true; } require(threw); };
    nlohmann::json manifest = {{"schemaVersion", 1}, {"version", "1.2.10"}, {"platforms", {{"windows", {
        {"url", "https://github.com/scholay/xsime/releases/download/v1.2.10/RIMES-Windows-1.2.10-Setup.exe"},
        {"sha256", std::string(64, 'a')}, {"size", 100u}, {"architecture", "x64-x86"}}}}}};
    const auto asset = Parse(manifest.dump(), "windows");
    require(Version(asset.version) > Version("1.2.9"));
    require(Version("1.2.9") < Version("1.3.0"));
    for (const auto* version : {"1.2", "01.2.3", "1.2.3-preview.1", "1.2.3/evil", "9999999999999.0.0", "-1.0.0"})
        rejects([&] { (void)Version(version); });
    Verify(asset, 100, std::string(64, 'a'));
    rejects([&] { Verify(asset, 101, std::string(64, 'a')); });
    rejects([&] { Verify(asset, 100, std::string(64, 'b')); });
    for (const auto* url : {"http://github.com/scholay/xsime/releases/download/v1.2.10/RIMES-Windows-1.2.10-Setup.exe", "https://github.com/attacker/xsime/releases/download/v1.2.10/RIMES-Windows-1.2.10-Setup.exe", "https://github.com/scholay/xsime/releases/download/v1.2.9/RIMES-Windows-1.2.9-Setup.exe", "https://github.com/scholay/xsime/releases/download/v1.2.10/RIMES-Windows-1.2.10-Setup.exe?redirect=1"}) {
        auto changed = manifest; changed["platforms"]["windows"]["url"] = url;
        rejects([&] { (void)Parse(changed.dump(), "windows"); });
    }
    for (const auto size : {0ll, -1ll, 1024ll * 1024 * 1024}) {
        auto changed = manifest; changed["platforms"]["windows"]["size"] = size;
        rejects([&] { (void)Parse(changed.dump(), "windows"); });
    }
    manifest["schemaVersion"] = 2;
    rejects([&] { (void)Parse(manifest.dump(), "windows"); });
    std::cout << "Application update contract checks: " << count << '\n';
}

}
int main(int argc, char** argv) {
    try {
        ManifestContracts();
        if (argc != 2) throw std::runtime_error("Pass the public signed-manifest fixture");
        std::ifstream file(argv[1], std::ios::binary);
        const auto fixture = nlohmann::json::parse(file);
        const auto document = fixture.at("document");
        const auto now = fixture.at("now").get<std::uint64_t>();
        std::vector<Key> keys;
        for (const auto& item : fixture.at("trustedKeys").items()) keys.push_back({item.key(), item.value().get<std::string>()});
        const auto parse = [&](const nlohmann::json& value, std::uint64_t time) {
            return rimes::updates::ParseAuthenticatedWithKeys(value.dump(), "windows", time, rimes::updates::VerifyP256, keys);
        };
        const auto asset = parse(document, now);
        Check(asset.version == "9.8.7", "known signature rejected");
        Check(rimes::updates::ParseAuthenticatedWithKeys(document.dump(), "linux", now, rimes::updates::VerifyP256, keys).version == asset.version,
              "Linux platform selection rejected");
        Reject([&] { rimes::updates::ParseAuthenticated(document.dump(), "windows", now, rimes::updates::VerifyP256); }, "fixture key entered production trust");
        auto changed = document; changed.erase("authentication");
        Reject([&] { parse(changed, now); }, "unsigned legacy metadata accepted");
        changed = document; changed["platforms"]["windows"]["sha256"] = std::string(64, 'a');
        Reject([&] { parse(changed, now); }, "outer/payload disagreement accepted");
        changed = document; auto signature = changed["authentication"]["signatures"][0]["signature"].get<std::string>();
        signature[0] = signature[0] == 'A' ? 'B' : 'A'; changed["authentication"]["signatures"][0]["signature"] = signature;
        Reject([&] { parse(changed, now); }, "bad ECDSA signature accepted");
        changed = document; changed["authentication"]["signatures"][0]["keyId"] = "untrusted-key";
        Reject([&] { parse(changed, now); }, "unknown key accepted");
        changed["authentication"]["signatures"].push_back(document["authentication"]["signatures"][0]);
        Check(parse(changed, now).version == asset.version, "key rotation with one known signature rejected");
        changed = document; changed["authentication"]["signatures"].push_back(document["authentication"]["signatures"][0]);
        Reject([&] { parse(changed, now); }, "duplicate signer accepted");
        changed = document; changed["authentication"]["signatures"] = nlohmann::json::array();
        Reject([&] { parse(changed, now); }, "empty signer list accepted");
        for (int i = 0; i < 9; ++i) {
            auto signer = document["authentication"]["signatures"][0];
            signer["keyId"] = "unknown-" + std::to_string(i);
            changed["authentication"]["signatures"].push_back(signer);
        }
        Reject([&] { parse(changed, now); }, "unbounded signer list accepted");
        const auto payload = rimes::updates::DecodeBase64(document["authentication"]["payload"].get<std::string>());
        const auto original_signature = rimes::updates::DecodeBase64(document["authentication"]["signatures"][0]["signature"].get<std::string>());
        Check(!rimes::updates::VerifyP256(rimes::updates::DecodePublicKey(keys.front().public_key_hex),
              std::string(payload.begin(), payload.end()) + " ", original_signature), "signature does not bind exact payload bytes");
        changed = document; changed["authentication"]["algorithm"] = "none";
        Reject([&] { parse(changed, now); }, "wrong signing algorithm accepted");
        Reject([&] { parse(document, document.at("expiresAt").get<std::uint64_t>()); }, "expired metadata accepted");
        Reject([&] { parse(document, document.at("issuedAt").get<std::uint64_t>() - 301); }, "future metadata accepted");
        Check(parse(document, document.at("issuedAt").get<std::uint64_t>() - 300).version == asset.version, "clock-skew allowance lost");
        rimes::updates::CheckVersionFloor(asset, "9.8.6", "9.8.7");
        Reject([&] { rimes::updates::CheckVersionFloor(asset, "9.8.8", ""); }, "installed version downgrade accepted");
        Reject([&] { rimes::updates::CheckVersionFloor(asset, "9.8.6", "9.9.0"); }, "previously seen release rollback accepted");
        auto other = asset; other.sha256 = std::string(64, 'a');
        Check(rimes::updates::PackageName(asset, "windows") != rimes::updates::PackageName(other, "windows"), "different packages share a cache path");
        Reject([&] { rimes::updates::Verify(asset, asset.size + 1, asset.sha256); }, "incorrect length accepted");
        Reject([&] { rimes::updates::Verify(asset, asset.size, other.sha256); }, "incorrect digest accepted");
        for (const auto* invalid : {"A", "A===", "AA=A", "AB==", "AAB=", "AAAA\n"})
            Reject([&] { rimes::updates::DecodeBase64(invalid); }, "noncanonical base64 accepted");
        std::cout << "Authenticated updates: signatures, rotation, expiry, platform and rollback checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
