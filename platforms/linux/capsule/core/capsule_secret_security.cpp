#include "capsule_password.hpp"
#include "capsule_rich_text.hpp"
#include "capsule_text.hpp"
#include "private_files.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
namespace rimes::capsule {
namespace {
std::optional<std::string> Base32(std::string_view input) {
    if (input.empty() || input.size() > 4096)
        return {};
    std::string output;
    std::uint32_t bits = 0;
    int count = 0;
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    for (unsigned char c : input) {
        if (c == ' ' || c == '-' || c == '=')
            continue;
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        const auto index = alphabet.find(c);
        if (index == alphabet.npos)
            return {};
        bits = (bits << 5) | index;
        count += 5;
        if (count >= 8) {
            count -= 8;
            output += static_cast<char>((bits >> count) & 255);
        }
    }
    return output.empty() ? std::nullopt : std::make_optional(std::move(output));
}
std::optional<std::string> DecodeURL(std::string_view text) {
    std::string result;
    auto hex = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%')
            result += text[i];
        else {
            if (i + 2 >= text.size())
                return {};
            int a = hex(text[i + 1]), b = hex(text[i + 2]);
            if (a < 0 || b < 0)
                return {};
            result += static_cast<char>(a * 16 + b);
            i += 2;
        }
    }
    return result;
}
int Number(const std::string& value, int fallback) {
    int number = 0;
    auto* start = value.data();
    if (!value.empty() && *start == '+')
        ++start;
    const auto result = std::from_chars(start, value.data() + value.size(), number);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() ? number
                                                                                 : fallback;
}
std::optional<std::string> Canonical(const RevealCode& code) {
    std::string result;
    for (const auto& keys : code) {
        const auto chord = CanonicalChord(keys);
        if (!chord)
            return {};
        if (!result.empty())
            result += '\x1f';
        result += *chord;
    }
    return result;
}
std::string Digest(const std::string& salt, const RevealCode& code) {
    auto canonical = Canonical(code);
    if (!canonical)
        return {};
    std::string bytes = "RIMES.CapsuleRevealPasscode.v1";
    bytes += '\0';
    bytes += salt;
    bytes += *canonical;
    std::array<unsigned char, 32> digest{};
    unsigned int count = 0;
    const bool ok =
        EVP_Digest(bytes.data(), bytes.size(), digest.data(), &count, EVP_sha256(), nullptr) == 1 &&
        count == 32;
    EraseSecret(bytes);
    EraseSecret(*canonical);
    if (!ok)
        throw std::runtime_error("口令摘要不可用");
    return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}
constexpr auto credential_name = "reveal-credential-v1";
std::string RevisionOf(const std::optional<std::string>& data) {
    return data ? runtime::Sha256(*data) : "default-v1";
}
bool MatchesData(const std::optional<std::string>& encoded, const RevealCode& code) {
    if (!Canonical(code))
        return false;
    if (!encoded)
        return Canonical(code) == Canonical(DefaultRevealCode());
    const auto bytes = Base64Decode(*encoded);
    if (!bytes || bytes->size() != 57 || (*bytes)[0] != 1)
        return false;
    auto digest = Digest(bytes->substr(1, 24), code);
    const bool match =
        digest.size() == 32 && CRYPTO_memcmp(digest.data(), bytes->data() + 25, 32) == 0;
    EraseSecret(digest);
    return match;
}
} // namespace
std::optional<TotpParameters> ParseTotp(std::string_view label, std::string_view value) {
    if (value.size() > 16384)
        return {};
    const auto text = TrimText(value);
    const auto lower = LowerText(text);
    if (lower.rfind("otpauth://totp/", 0) == 0) {
        const auto at = text.find('?');
        if (at == text.npos)
            return {};
        std::map<std::string, std::string> query;
        std::string_view rest(text.data() + at + 1, text.size() - at - 1);
        if (const auto fragment = rest.find('#'); fragment != rest.npos)
            rest = rest.substr(0, fragment);
        while (!rest.empty()) {
            const auto amp = rest.find('&');
            const auto field = rest.substr(0, amp);
            const auto eq = field.find('=');
            const auto name = DecodeURL(field.substr(0, eq));
            const auto content =
                DecodeURL(eq == field.npos ? std::string_view{} : field.substr(eq + 1));
            if (!name || !content)
                return {};
            query.emplace(LowerText(*name), *content);
            if (amp == rest.npos)
                break;
            rest.remove_prefix(amp + 1);
        }
        if (query.count("algorithm") && LowerText(query.at("algorithm")) != "sha1")
            return {};
        const auto secret = Base32(query["secret"]);
        if (!secret)
            return {};
        const int digits = Number(query["digits"], 6), period = Number(query["period"], 30);
        if (digits < 6 || digits > 8 || period < 10 || period > 120)
            return {};
        return TotpParameters{*secret, digits, period};
    }
    const auto name = LowerText(label);
    for (const auto* term :
         {"totp", "2fa", "otp", "mfa", "两步验证", "二次验证", "动态码", "验证器"})
        if (name.find(term) != name.npos) {
            const auto seed = Base32(text);
            if (!seed || seed->size() < 10)
                return {};
            return TotpParameters{*seed, 6, 30};
        }
    return {};
}
std::pair<std::string, int> TotpCode(const TotpParameters& p, std::int64_t seconds) {
    if (seconds < 0 || p.digits < 6 || p.digits > 8 || p.period < 10 || p.period > 120 ||
        p.secret.empty() || p.secret.size() > 4096)
        throw std::runtime_error("动态码参数无效");
    std::array<unsigned char, 8> counter{};
    auto value = static_cast<std::uint64_t>(seconds / p.period);
    for (int i = 7; i >= 0; --i) {
        counter[i] = value & 255;
        value >>= 8;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int length = 0;
    if (!HMAC(EVP_sha1(), p.secret.data(), p.secret.size(), counter.data(), counter.size(),
              mac.data(), &length) ||
        length != 20)
        throw std::runtime_error("动态码生成失败");
    const auto offset = mac[19] & 15;
    const std::uint32_t binary = (std::uint32_t(mac[offset] & 127) << 24) |
                                 (std::uint32_t(mac[offset + 1]) << 16) |
                                 (std::uint32_t(mac[offset + 2]) << 8) | mac[offset + 3];
    std::uint32_t modulus = 1;
    for (int i = 0; i < p.digits; ++i)
        modulus *= 10;
    const auto code = std::to_string(binary % modulus);
    OPENSSL_cleanse(mac.data(), mac.size());
    return {std::string(p.digits - code.size(), '0') + code, p.period - int(seconds % p.period)};
}
RevealCode DefaultRevealCode() { return {"rh", "wo", "cvn", "qu"}; }
std::optional<std::string> CanonicalChord(std::string_view keys) {
    constexpr std::string_view order = "qwertyuiopasdfghjklzxcvbnm";
    std::string lower;
    for (unsigned char c : keys) {
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (order.find(c) == order.npos)
            return {};
        lower += c;
    }
    std::string result;
    for (char c : order)
        if (lower.find(c) != lower.npos)
            result += c;
    if (result.empty() || result.size() > 8)
        return {};
    return result;
}
char RevealPhysicalLetter(unsigned key) {
    if (key >= 24 && key <= 33)
        return "qwertyuiop"[key - 24];
    if (key >= 38 && key <= 46)
        return "asdfghjkl"[key - 38];
    if (key >= 52 && key <= 58)
        return "zxcvbnm"[key - 52];
    return 0;
}
RevealCredential::RevealCredential(std::filesystem::path root) : root_(std::move(root)) {
    runtime::PrivateDirectory directory(root_);
}
std::string RevealCredential::Revision() const {
    return RevisionOf(runtime::PrivateDirectory(root_).Read(credential_name, 1024));
}
bool RevealCredential::Matches(const RevealCode& code) const {
    try {
        return MatchesData(runtime::PrivateDirectory(root_).Read(credential_name, 1024), code);
    } catch (const std::exception&) {
        return false;
    }
}
void RevealCredential::Change(const RevealCode& current, const std::optional<RevealCode>& next,
                              const std::string& expected, const SecretCommit& gate) {
    runtime::PrivateDirectory directory(root_);
    auto lock = directory.Lock(false);
    const auto data = directory.Read(credential_name, 1024);
    if (RevisionOf(data) != expected || !MatchesData(data, current))
        throw std::runtime_error("当前口令不匹配或已变化");
    if (!next) {
        CommitSecret(gate, [&] { directory.Remove(credential_name); });
        return;
    }
    if (!Canonical(*next))
        throw std::runtime_error("口令需要四组有效物理字母键");
    std::string salt(24, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), salt.size()) != 1)
        throw std::runtime_error("随机源不可用");
    const auto encoded = Base64Encode(std::string(1, '\1') + salt + Digest(salt, *next));
    CommitSecret(gate, [&] { directory.Write(credential_name, encoded); });
}
void SecretAccess::Authorize(std::string id, std::string record, std::string credential, Time now) {
    Conceal();
    id_ = std::move(id);
    record_revision_ = std::move(record);
    credential_revision_ = std::move(credential);
    until_ = now + std::chrono::seconds(60);
}
bool SecretAccess::Valid(const std::string& id, const std::string& record,
                         const std::string& credential, Time now) const {
    return !id_.empty() && id == id_ && record == record_revision_ &&
           credential == credential_revision_ && now < until_;
}
void SecretAccess::Reveal(std::size_t row, Time now) {
    if (id_.empty() || now >= until_) {
        Conceal();
        return;
    }
    row_ = row;
    reveal_until_ = std::min(until_, now + std::chrono::seconds(15));
}
bool SecretAccess::Revealed(std::size_t row, Time now) const {
    return row_ == row && now < until_ && now < reveal_until_;
}
void SecretAccess::Conceal() {
    id_.clear();
    record_revision_.clear();
    credential_revision_.clear();
    row_.reset();
    until_ = {};
    reveal_until_ = {};
}
} // namespace rimes::capsule
