#pragma once
#include "manifest.hpp"
#include "trusted_keys.generated.hpp"
#include <algorithm>
#include <string_view>
#include <set>
#include <vector>

namespace rimes::updates {
inline std::vector<unsigned char> DecodeBase64(const std::string& input) {
    if (input.empty() || input.size() % 4 != 0 || input.size() > 2 * 1024 * 1024)
        throw std::runtime_error("Invalid update authentication encoding");
    const auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return c == '=' ? -2 : -1;
    };
    std::vector<unsigned char> result;
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const int a = digit(input[i]), b = digit(input[i + 1]), c = digit(input[i + 2]), d = digit(input[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1 || (c == -2 && d != -2) ||
            ((c == -2 || d == -2) && i + 4 != input.size()) ||
            (c == -2 && (b & 15)) || (d == -2 && c >= 0 && (c & 3)))
            throw std::runtime_error("Invalid update authentication encoding");
        result.push_back(static_cast<unsigned char>((a << 2) | (b >> 4)));
        if (c >= 0) result.push_back(static_cast<unsigned char>((b << 4) | (c >> 2)));
        if (d >= 0) result.push_back(static_cast<unsigned char>((c << 6) | d));
    }
    return result;
}
inline std::vector<unsigned char> DecodePublicKey(std::string_view hex) {
    if (hex.size() != 130) throw std::runtime_error("Invalid trusted update key");
    std::vector<unsigned char> result;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        auto digit = [](char c) -> unsigned {
            if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
            throw std::runtime_error("Invalid trusted update key");
        };
        result.push_back(static_cast<unsigned char>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    }
    if (result.front() != 4) throw std::runtime_error("Invalid trusted update key");
    return result;
}

template<class VerifySignature, class TrustedKeys>
Asset ParseAuthenticatedWithKeys(const std::string& bytes, const std::string& platform,
                                std::uint64_t now, VerifySignature verify, const TrustedKeys& trusted_keys) {
    if (bytes.size() > 1024 * 1024) throw std::runtime_error("Update metadata too large");
    auto outer = nlohmann::json::parse(bytes);
    const auto& authentication = outer.at("authentication");
    if (authentication.at("format") != 1 || authentication.at("algorithm") != "ecdsa-p256-sha256")
        throw std::runtime_error("Unsupported update authentication");
    const auto payload = DecodeBase64(authentication.at("payload").get<std::string>());
    const std::string signed_bytes(payload.begin(), payload.end());
    const auto& signatures = authentication.at("signatures");
    if (!signatures.is_array() || signatures.empty() || signatures.size() > 8)
        throw std::runtime_error("Invalid update signature list");
    std::set<std::string> identifiers;
    bool authenticated = false;
    for (const auto& item : signatures) {
        if (!item.is_object() || item.size() != 2) throw std::runtime_error("Invalid update signature record");
        const auto id = item.at("keyId").get<std::string>();
        if (!std::regex_match(id, std::regex("[a-zA-Z0-9._-]{1,80}")) || !identifiers.insert(id).second)
            throw std::runtime_error("Invalid or duplicate update signing key");
        const auto signature = DecodeBase64(item.at("signature").get<std::string>());
        if (signature.size() != 64) throw std::runtime_error("Invalid update signature size");
        const auto key = std::find_if(trusted_keys.begin(), trusted_keys.end(),
                                     [&](const auto& trusted) { return trusted.id == id; });
        if (key != trusted_keys.end() && verify(DecodePublicKey(key->public_key_hex), signed_bytes, signature))
            authenticated = true;
    }
    if (!authenticated)
        throw std::runtime_error("Update signature verification failed");
    const auto signed_json = nlohmann::json::parse(signed_bytes);
    outer.erase("authentication");
    if (signed_json != outer) throw std::runtime_error("Update metadata does not match its signed payload");
    const auto& issued = signed_json.at("issuedAt");
    const auto& expires = signed_json.at("expiresAt");
    if (!issued.is_number_unsigned() || !expires.is_number_unsigned())
        throw std::runtime_error("Invalid update metadata validity");
    const auto start = issued.get<std::uint64_t>(), end = expires.get<std::uint64_t>();
    if (end > 253402300799ull || now > 253402300499ull || start > now + 300 ||
        end <= now || end <= start || end - start > 90ull * 24 * 60 * 60)
        throw std::runtime_error("Update metadata is expired or not yet valid");
    return Parse(signed_bytes, platform);
}
template<class VerifySignature>
Asset ParseAuthenticated(const std::string& bytes, const std::string& platform,
                         std::uint64_t now, VerifySignature verify) {
    return ParseAuthenticatedWithKeys(bytes, platform, now, verify, kTrustedUpdateKeys);
}
} // namespace rimes::updates
