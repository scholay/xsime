#include "gateway_access.hpp"
#include "native_gateway_contract.hpp"
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
namespace rimes::runtime {
namespace {
bool Valid(const std::string& value) {
    return value.size() == 32 &&
           value.find_first_not_of(
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == value.npos;
}
std::string Generate() {
    unsigned char bytes[24], encoded[33];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        throw std::runtime_error("无法生成网关令牌");
    EVP_EncodeBlock(encoded, bytes, sizeof(bytes));
    std::string token(reinterpret_cast<char*>(encoded), 32);
    for (auto& c : token) {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
    }
    return token;
}
bool Equal(const std::string& expected, const std::string& candidate) {
    return Valid(expected) && candidate.size() == expected.size() &&
           CRYPTO_memcmp(expected.data(), candidate.data(), expected.size()) == 0;
}
} // namespace
const nlohmann::json& GatewayClients() {
    static const auto clients =
        nlohmann::json::parse(rimes::mailbox::kNativeGateway).at("settings").at("clients");
    return clients;
}
GatewayAccess::GatewayAccess(std::filesystem::path root) : files_(root) {}
std::string GatewayAccess::Current() {
    auto lock = files_.Lock(false);
    if (auto saved = files_.Read("token", 128)) {
        if (!Valid(*saved))
            throw std::runtime_error("网关令牌文件无效，请重新生成授权");
        return *saved;
    }
    const auto token = Generate();
    files_.Write("token", token);
    return token;
}
std::string GatewayAccess::Rotate() {
    auto lock = files_.Lock(false);
    const auto token = Generate();
    files_.Write("token", token);
    return token;
}
bool GatewayAccess::Matches(const std::string& candidate) const {
    const auto token = files_.Read("token", 128);
    return token && Equal(*token, candidate);
}
void GatewayAccess::WithBearer(const std::string& candidate,
                               const std::function<void()>& action) const {
    auto lock = files_.Lock(false);
    if (!Matches(candidate))
        throw std::runtime_error("网关授权已撤销");
    action();
}
std::string GatewayConfiguration(const std::string& client, unsigned port, const std::string& token,
                                 bool supplementary) {
    if (port < 1024 || port > 65535 || (!Valid(token) && token != "••••••••"))
        throw std::runtime_error("无效的网关配置");
    if (client != "codex" && client != "cursor" && client != "claudeCode" && client != "genericMCP")
        throw std::runtime_error("无效的 MCP 客户端");
    const auto url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    if (supplementary) {
        if (client == "codex")
            return "export RIMES_MCP_TOKEN='" + token + "'";
        if (client == "claudeCode")
            return "claude mcp add --transport http rimes " + url +
                   " --header \"Authorization: Bearer " + token + "\"";
        return "";
    }
    if (client == "codex")
        return "[mcp_servers.rimes]\nurl = \"" + url +
               "\"\nbearer_token_env_var = \"RIMES_MCP_TOKEN\"\ndefault_tools_approval_mode = "
               "\"prompt\"";
    return nlohmann::ordered_json{
        {"mcpServers",
         {{"rimes",
           {{"type", "http"}, {"url", url}, {"headers", {{"Authorization", "Bearer " + token}}}}}}}}
        .dump(2);
}
} // namespace rimes::runtime
