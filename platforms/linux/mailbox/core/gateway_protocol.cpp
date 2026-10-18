#include "capsule_text.hpp"
#include "gateway_protocol.hpp"
#include "native_gateway_contract.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <set>
#include <sstream>
namespace rimes::mailbox {
namespace {
struct Rejection : std::runtime_error {
    int status;
    Rejection(int code, const char* message) : std::runtime_error(message), status(code) {}
};
void Require(bool ok, int status, const char* message) {
    if (!ok)
        throw Rejection(status, message);
}
std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == value.npos)
        return "";
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string Lower(std::string value) {
    for (auto& c : value)
        c = std::tolower(static_cast<unsigned char>(c));
    return value;
}
bool Plain(const std::string& value, std::size_t limit) {
    return value.size() <= limit && std::none_of(value.begin(), value.end(), [](unsigned char c) {
               return c < 32 || c == 127;
           });
}
std::string Field(const Json& value, const char* name, const std::string& fallback = "",
                  std::size_t limit = 128) {
    if (!value.contains(name))
        return fallback;
    Require(value.at(name).is_string(), 400, "invalid field type");
    const auto result = value.at(name).get<std::string>();
    Require(Plain(result, limit), 400, "invalid field value");
    return result;
}
std::string Format(const Json& args) {
    const auto format = Field(args, "format", "plain");
    Require(format == "plain" || format == "markdown" || format == "json", 400,
            "format must be plain, markdown, or json");
    return format;
}
Json ParseJSON(const std::string& body) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(body, [&](int depth, Json::parse_event_t event, Json& value) {
        Require(depth <= 32, 400, "JSON nesting limit exceeded");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        if (event == Json::parse_event_t::key)
            Require(!keys.empty() && keys.back().insert(value.get<std::string>()).second, 400,
                    "duplicate JSON field");
        if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    });
}
GatewayResponse JSON(const Json& value, int code = 200) { return {code, value.dump()}; }
GatewayResponse Error(const Json& id, int code, const std::string& text, int http = 200) {
    return JSON({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", text}}}},
                http);
}
GatewayResponse Result(const Json& id, const Json& value) {
    return JSON({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}});
}
GatewayResponse ToolResult(const Json& id, const std::string& text, bool error = false,
                           const Json& structured = nullptr) {
    Json result{{"content", Json::array({{{"type", "text"}, {"text", text}}})}, {"isError", error}};
    if (!structured.is_null())
        result["structuredContent"] = structured;
    return Result(id, result);
}
std::string Header(const GatewayRequest& request, const std::string& name) {
    const auto item = request.headers.find(name);
    return item == request.headers.end() ? "" : item->second;
}
std::string ReplayKey(const GatewayRequest& request, const GatewayContext& context) {
    const auto key = Header(request, "idempotency-key");
    if (key.empty())
        return "";
    Require(Plain(key, 128), 400, "invalid idempotency key");
    return runtime::Sha256(context.bearer + ":" + request.target + ":" + key);
}
} // namespace
const Json& GatewayContract() {
    static const auto contract = Json::parse(kNativeGateway);
    return contract;
}
bool GatewayOrigin(const std::string& raw) {
    const auto value = Lower(Trim(raw));
    std::string authority;
    if (value.rfind("http://", 0) == 0)
        authority = value.substr(7);
    else if (value.rfind("https://", 0) == 0)
        authority = value.substr(8);
    else
        return false;
    for (const auto* host : {"localhost", "127.0.0.1", "[::1]"}) {
        const std::string name = host;
        if (authority == name)
            return true;
        if (authority.rfind(name + ":", 0) != 0)
            continue;
        const auto port = authority.substr(name.size() + 1);
        unsigned number = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
        return !port.empty() && parsed.ec == std::errc() &&
               parsed.ptr == port.data() + port.size() && number <= 65535 &&
               std::to_string(number) == port;
    }
    return false;
}
bool GatewayHost(const std::string& raw, unsigned port) {
    const auto host = Lower(raw);
    return host == "127.0.0.1:" + std::to_string(port) ||
           host == "localhost:" + std::to_string(port);
}
ParseResult ParseGatewayRequest(std::string& bytes, GatewayRequest& request) {
    const auto maximum = GatewayContract().at("maxHeaderBytes").get<std::size_t>();
    const auto split = bytes.find("\r\n\r\n");
    if (split == bytes.npos)
        return bytes.size() > maximum ? ParseResult::TooLarge : ParseResult::Incomplete;
    if (split + 4 > maximum)
        return ParseResult::TooLarge;
    for (std::size_t i = 0; i < split; ++i) {
        if ((bytes[i] == '\n' && (!i || bytes[i - 1] != '\r')) ||
            (bytes[i] == '\r' && (i + 1 >= split || bytes[i + 1] != '\n')))
            return ParseResult::Invalid;
    }
    std::istringstream header(bytes.substr(0, split));
    std::string line, version, extra;
    if (!std::getline(header, line) || line.size() > 8192)
        return ParseResult::Invalid;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    std::istringstream first(line);
    GatewayRequest parsed;
    if (!(first >> parsed.method >> parsed.target >> version) || (first >> extra) ||
        !Plain(line, 8192) || parsed.target.empty() || parsed.target.front() != '/' ||
        (version != "HTTP/1.1" && version != "HTTP/1.0"))
        return ParseResult::Invalid;
    while (std::getline(header, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto colon = line.find(':');
        if (colon == line.npos || !colon)
            return ParseResult::Invalid;
        const auto name = Lower(line.substr(0, colon)), value = Trim(line.substr(colon + 1));
        if (name.size() > 128 ||
            std::any_of(name.begin(), name.end(),
                        [](unsigned char c) { return !std::isalnum(c) && c != '-'; }) ||
            !Plain(value, maximum) || !parsed.headers.emplace(name, value).second)
            return ParseResult::Invalid;
    }
    if (parsed.headers.count("transfer-encoding") || parsed.headers.count("expect"))
        return ParseResult::Invalid;
    std::size_t length = 0;
    if (parsed.headers.count("content-length")) {
        const auto& text = parsed.headers.at("content-length");
        const auto end = std::from_chars(text.data(), text.data() + text.size(), length);
        if (text.empty() || end.ec != std::errc() || end.ptr != text.data() + text.size())
            return ParseResult::Invalid;
    } else if (parsed.method == "POST")
        return ParseResult::Invalid;
    if (length > GatewayContract().at("maxBodyBytes").get<std::size_t>())
        return ParseResult::TooLarge;
    if (bytes.size() - split - 4 < length)
        return ParseResult::Incomplete;
    parsed.body = bytes.substr(split + 4, length);
    bytes.erase(0, split + 4 + length);
    request = std::move(parsed);
    return ParseResult::Complete;
}
GatewayProtocol::GatewayProtocol(std::filesystem::path mailbox, CommitFactory commit, Now now)
    : mailbox_(std::move(mailbox)), commit_(std::move(commit)),
      now_(now ? std::move(now) : [] { return std::chrono::steady_clock::now(); }) {}
void GatewayProtocol::Reset() {
    streams_.clear();
    replay_.clear();
}
void GatewayProtocol::Expire() {
    const auto cutoff = now_() - std::chrono::minutes(5);
    for (auto it = replay_.begin(); it != replay_.end();)
        if (it->second.created < cutoff)
            it = replay_.erase(it);
        else
            ++it;
    for (auto it = streams_.begin(); it != streams_.end();)
        if (it->second.created < cutoff)
            it = streams_.erase(it);
        else
            ++it;
}
void GatewayProtocol::ValidateText(const std::string& text, bool allow_empty) const {
    Require((allow_empty || !capsule::TrimText(text).empty()) && text.find('\0') == text.npos, 400,
            "text is empty or contains NUL");
    Require(text.size() <= GatewayContract().at("maxBodyBytes") &&
                capsule::CharacterCount(text) <= GatewayContract().at("maxTextCount"),
            413, "text is too large");
}
void GatewayProtocol::CheckSource(const GatewayContext& context, const std::string& kind,
                                  const std::string& name) const {
    Require(!name.empty() && Plain(name, 128), 400, "invalid source name");
    const auto& blocked = context.configuration.at("gatewayBlockedSources");
    Require(std::find(blocked.begin(), blocked.end(), kind + ":" + name) == blocked.end(), 403,
            "source is blocked");
}
Json GatewayProtocol::Receive(const GatewayRequest& request, GatewayContext& context,
                              const Json& args, const std::string& kind) {
    Require(args.contains("text") && args.at("text").is_string(), 400, "text is required");
    const auto text = args.at("text").get<std::string>();
    ValidateText(text);
    const auto name = kind == "http" ? Field(args, "source", "http") : context.client;
    CheckSource(context, kind, name);
    const auto nonce = ReplayKey(request, context);
    const auto title = Field(args, "title", Field(args, "kind"), 240);
    const auto format = Format(args);
    const Json source{{"kind", kind}, {"displayName", name}, {"replyCapability", "localNotesOnly"}};
    const auto digest =
        nonce.empty() ? "" : runtime::Sha256(Json{source, title, text, format}.dump());
    try {
        return Store(mailbox_, {}, commit_(context))
            .ReceiveGateway(source, title, text, format, nonce, digest);
    } catch (const runtime::StorageBusy&) {
        throw Rejection(503, "Mailbox storage is busy");
    } catch (const std::exception& e) {
        const std::string message = e.what();
        if (message == "请求标识已用于不同内容")
            throw Rejection(409, "idempotency key already used for different content");
        if (message == "待审核收件已达到上限")
            throw Rejection(429, "Mailbox review queue is full");
        throw Rejection(503, "Mailbox publication was cancelled or storage is unavailable");
    }
}
GatewayResponse GatewayProtocol::Handle(const GatewayRequest& request, GatewayContext& context) {
    Expire();
    try {
        if (request.method == "GET" && request.target == "/v1/health")
            return JSON({{"ok", true}});
        if (request.target == "/mcp" && (request.method == "GET" || request.method == "DELETE"))
            return JSON({{"error", "stateless MCP uses POST JSON responses"}}, 405);
        Require(request.method == "POST", 405, "method not allowed");
        Require(request.target == "/mcp" || request.target == "/v1/inbound", 404, "not found");
        const auto content_type = Lower(Header(request, "content-type"));
        Require(content_type == "application/json" ||
                    content_type == "application/json; charset=utf-8" ||
                    content_type == "application/json;charset=utf-8",
                415, "application/json required");
        Json value;
        try {
            value = ParseJSON(request.body);
        } catch (const std::exception&) {
            return request.target == "/mcp" ? Error(nullptr, -32700, "parse error", 400)
                                            : JSON({{"error", "invalid JSON"}}, 400);
        }
        Require(value.is_object(), 400, "a JSON object is required");
        if (request.target == "/mcp")
            return MCP(request, context, value);
        Receive(request, context, value, "http");
        return JSON({{"accepted", true}, {"disposition", "mailbox"}});
    } catch (const Rejection& e) {
        return JSON({{"error", e.what()}}, e.status);
    } catch (const std::exception&) {
        return JSON({{"error", "invalid request"}}, 400);
    }
}
GatewayResponse GatewayProtocol::MCP(const GatewayRequest& request, GatewayContext& context,
                                     const Json& value) {
    const auto id = value.value("id", Json(nullptr));
    if (value.value("jsonrpc", Json()) != "2.0" ||
        (value.contains("id") && !id.is_string() && !id.is_number_integer()) ||
        (id.is_string() && id.get<std::string>().size() > 128))
        return Error(nullptr, -32600, "invalid request", 400);
    const auto method = Field(value, "method");
    if (method.empty())
        return Error(nullptr, -32600, "missing method", 400);
    const auto version = Header(request, "mcp-protocol-version");
    const auto& contract = GatewayContract().at("mcp");
    const auto& versions = contract.at("versions");
    if (method != "initialize" && !version.empty() &&
        std::find(versions.begin(), versions.end(), version) == versions.end())
        return Error(id, -32600, "unsupported MCP-Protocol-Version", 400);
    // Notifications never execute give-only tools or produce a JSON-RPC response.
    if (!value.contains("id"))
        return {202, ""};
    if (method == "initialize") {
        const auto params = value.value("params", Json::object());
        Require(params.is_object(), 400, "invalid initialize params");
        if (params.contains("clientInfo")) {
            Require(params.at("clientInfo").is_object(), 400, "invalid clientInfo");
            context.client = Field(params.at("clientInfo"), "name", "mcp");
        }
        const auto requested = Field(params, "protocolVersion");
        const auto selected =
            std::find(versions.begin(), versions.end(), requested) != versions.end()
                ? Json(requested)
                : contract.at("latest");
        return Result(
            id, {{"protocolVersion", selected},
                 {"capabilities", {{"tools", {{"listChanged", false}}}}},
                 {"serverInfo", {{"name", "XSIME"}, {"title", "XSIME 缓冲区"}, {"version", "1"}}},
                 {"instructions", "把文字送进用户输入法的缓冲区收件箱，等用户确认后由用户上屏。只进"
                                  "不出：无法读取缓冲区，也不会自动上屏。"}});
    }
    if (method == "ping")
        return Result(id, Json::object());
    if (method == "tools/list")
        return Result(id, {{"tools", contract.at("tools")}});
    if (method == "tools/call") {
        const auto key = ReplayKey(request, context);
        const auto digest = runtime::Sha256(request.body);
        if (!key.empty()) {
            const auto found = replay_.find(key);
            if (found != replay_.end()) {
                Require(found->second.digest == digest, 409,
                        "idempotency key already used for different content");
                return found->second.response;
            }
        }
        // Never evict an active stream's retry identity to accept another mutation.
        // Both the stream and its earliest cached operation expire after five minutes.
        Require(key.empty() || replay_.size() < 256, 429, "idempotency window is full");
        auto result = Tool(request, context, value, id);
        if (!key.empty() && result.status == 200 &&
            !Json::parse(result.body).value("result", Json::object()).value("isError", true)) {
            replay_[key] = {digest, result, now_()};
        }
        return result;
    }
    return Error(id, -32601, "method not found");
}
GatewayResponse GatewayProtocol::Tool(const GatewayRequest& request, GatewayContext& context,
                                      const Json& value, const Json& id) {
    try {
        const auto params = value.value("params", Json::object());
        Require(params.is_object(), 400, "invalid tool params");
        const auto name = Field(params, "name"), owner = runtime::Sha256(context.bearer);
        const auto args = params.value("arguments", Json::object());
        Require(args.is_object(), 400, "invalid tool arguments");
        if (name == "buffer_push") {
            Receive(request, context, args, "mcp");
            return ToolResult(id, "accepted into Mailbox for review");
        }
        CheckSource(context, "mcp", context.client);
        if (name == "buffer_stream_begin") {
            Require(streams_.size() < GatewayContract().at("maxPending"), 429,
                    "stream queue is full");
            const auto key = "st-" + runtime::RandomID();
            streams_[key] = {owner, context.client, Field(args, "title", "", 240),
                             "",    Format(args),   now_()};
            return ToolResult(id, "stream " + key + " open", false, {{"stream_id", key}});
        }
        if (name != "buffer_stream_append" && name != "buffer_stream_end")
            return Error(id, -32602, "unknown tool");
        const auto key = Field(args, "stream_id");
        const auto found = streams_.find(key);
        Require(found != streams_.end() && found->second.owner == owner, 404,
                "stream was not found");
        auto& stream = found->second;
        CheckSource(context, "mcp", stream.client);
        if (name == "buffer_stream_append") {
            Require(args.contains("delta") && args.at("delta").is_string(), 400,
                    "delta is required");
            const auto next = stream.text + args.at("delta").get<std::string>();
            ValidateText(next, true);
            std::size_t total = next.size();
            for (const auto& [stream_id, pending] : streams_)
                if (stream_id != key)
                    total += pending.text.size();
            Require(total <= 1048576, 429, "stream memory limit exceeded");
            stream.text = next;
            return ToolResult(id, "appended");
        }
        auto frozen = context;
        frozen.client = stream.client;
        Receive(request, frozen,
                {{"text", stream.text}, {"title", stream.title}, {"format", stream.format}}, "mcp");
        streams_.erase(found);
        return ToolResult(id, "stream committed to Mailbox for review");
    } catch (const std::exception& e) {
        return ToolResult(id, e.what(), true);
    }
}
} // namespace rimes::mailbox
