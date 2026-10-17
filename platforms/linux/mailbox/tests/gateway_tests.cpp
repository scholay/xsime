#include "configuration.hpp"
#include "gateway_access.hpp"
#include "gateway_server.hpp"
#include "gateway_test_client.hpp"
#include "official_catalog.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
using namespace rimes::mailbox;
namespace rt = rimes::runtime;
using Clock = std::chrono::steady_clock;
namespace {
void Check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
template <class F> void Eventually(F check, const char* why) {
    const auto end = Clock::now() + std::chrono::seconds(4);
    while (!check()) {
        Check(Clock::now() < end, why);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
GatewayRequest Request(const Json& value, std::string path = "/mcp") {
    return {"POST", std::move(path), value.dump(), {{"content-type", "application/json"}}};
}
Json Tool(const std::string& name, Json args = Json::object()) {
    return {{"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "tools/call"},
            {"params", {{"name", name}, {"arguments", args}}}};
}
bool ToolError(const GatewayResponse& result) {
    return Json::parse(result.body).at("result").at("isError");
}
void Protocol(const std::filesystem::path& root) {
    for (const auto& client : rt::GatewayClients()) {
        Check(rt::GatewayConfiguration(client.at("id"), 47700) == client.at("configuration"),
              "native client configuration differs");
        Check(rt::GatewayConfiguration(client.at("id"), 47700, "••••••••", true) ==
                  client.at("supplementary"),
              "native supplementary configuration differs");
    }
    for (const auto& test : GatewayContract().at("origins"))
        if (!test.at("origin").is_null())
            Check(GatewayOrigin(test.at("origin")) == test.at("allowed"), "native origin parity");
    Check(GatewayHost("127.0.0.1:47700", 47700) && !GatewayHost("evil.test:47700", 47700),
          "host origin binding");
    const std::string raw =
        "POST /v1/inbound HTTP/1.1\r\nHost: localhost:47700\r\nContent-Length: 2\r\n\r\n{}";
    for (std::size_t n = 0; n < raw.size(); ++n) {
        auto partial = raw.substr(0, n);
        GatewayRequest request;
        Check(ParseGatewayRequest(partial, request) == ParseResult::Incomplete,
              "fragment parsed early");
        partial += raw.substr(n) + raw;
        Check(ParseGatewayRequest(partial, request) == ParseResult::Complete &&
                  request.body == "{}" && partial == raw,
              "framing lost pipeline");
    }
    for (const auto& header : std::vector<std::string>{
             "Content-Length: 0\r\nContent-Length: 0", "Content-Length: -1",
             "Content-Length: 0\r\nTransfer-Encoding: chunked", "Content-Length: 0\nX-Hidden: true",
             "Content-Length: 0\r\nHost : evil", "Content-Length: 0\r\nX: a\rb"}) {
        auto frame = "POST /mcp HTTP/1.1\r\n" + header + "\r\n\r\n";
        GatewayRequest request;
        Check(ParseGatewayRequest(frame, request) == ParseResult::Invalid,
              "ambiguous HTTP framing accepted");
    }
    GatewayRequest parsed;
    std::string huge = "POST /mcp HTTP/1.1\r\nContent-Length: 262145\r\n\r\n";
    Check(ParseGatewayRequest(huge, parsed) == ParseResult::TooLarge, "body limit");
    auto now = Clock::now();
    GatewayProtocol protocol(root, [](const auto&) { return Commit{}; }, [&] { return now; });
    GatewayContext context{"public-fixture", std::string(32, 'a'),
                           rt::ConfigurationStore::Defaults()};
    auto tools = Json::parse(
        protocol.Handle(Request({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}}), context)
            .body);
    Check(tools.at("result").at("tools") == GatewayContract().at("mcp").at("tools"),
          "native tool schema differs");
    Check(
        protocol.Handle(Request({{"jsonrpc", "2.0"}, {"id", nullptr}, {"method", "ping"}}), context)
                .status == 400,
        "null MCP request identity accepted");
    Check(protocol.Handle(Request({{"jsonrpc", "2.0"}}), context).status == 400,
          "missing method treated as notification");
    auto notify = Tool("buffer_push", {{"text", "must not be executed"}});
    notify.erase("id");
    auto notification = protocol.Handle(Request(notify), context);
    Check(notification.status == 202 && notification.body.empty() &&
              Store(root).Read().document.at("threads").empty(),
          "notification executed or replied");
    auto inbound = Request({{"text", "公开正文\n第二段🙂"},
                            {"format", "markdown"},
                            {"source", "fixture"},
                            {"title", "Title"}},
                           "/v1/inbound");
    inbound.headers["idempotency-key"] = "retry-once";
    Check(protocol.Handle(inbound, context).status == 200, "HTTP inbound failed");
    Check(protocol.Handle(inbound, context).status == 200 &&
              Store(root).Read().document.at("threads").size() == 1,
          "durable retry duplicated");
    GatewayProtocol reopened(root, [](const auto&) { return Commit{}; });
    Check(reopened.Handle(inbound, context).status == 200 &&
              Store(root).Read().document.at("threads").size() == 1,
          "restart lost idempotency");
    auto conflict = inbound;
    conflict.body = Request({{"text", "different"}}, "/v1/inbound").body;
    Check(protocol.Handle(conflict, context).status == 409, "nonce collision accepted");
    for (const std::string& body : std::vector<std::string>{
             "{\"text\":\"a\",\"text\":\"b\"}", "[]", std::string(40, '[') + std::string(40, ']'),
             "{\"text\":\"\xff\"}"}) {
        auto invalid = inbound;
        invalid.body = body;
        Check(protocol.Handle(invalid, context).status == 400, "invalid JSON accepted");
    }
    auto longtext = Request({{"text", std::string(20001, 'x')}}, "/v1/inbound");
    Check(protocol.Handle(longtext, context).status == 413, "text count exceeded");
    context.configuration["gatewayBlockedSources"] = {"http:fixture"};
    Check(protocol.Handle(inbound, context).status == 403, "blocked source replayed");
    context.configuration["gatewayBlockedSources"] = Json::array();
    auto begin = Request(Tool("buffer_stream_begin", {{"title", "Stream"}, {"format", "json"}}));
    begin.headers["idempotency-key"] = "begin";
    const auto started = protocol.Handle(begin, context);
    Check(!ToolError(started) && protocol.Handle(begin, context).body == started.body,
          "stream begin replay duplicated");
    const auto id = Json::parse(started.body).at("result").at("structuredContent").at("stream_id");
    auto append =
        Request(Tool("buffer_stream_append", {{"stream_id", id}, {"delta", "{\"公开\":true}"}}));
    append.headers["idempotency-key"] = "append";
    Check(!ToolError(protocol.Handle(append, context)) &&
              !ToolError(protocol.Handle(append, context)),
          "stream append replay");
    auto other = context;
    other.bearer = std::string(32, 'b');
    Check(
        ToolError(protocol.Handle(Request(Tool("buffer_stream_end", {{"stream_id", id}})), other)),
        "cross token stream access");
    Check(!ToolError(
              protocol.Handle(Request(Tool("buffer_stream_end", {{"stream_id", id}})), context)),
          "stream end failed");
    const auto messages = Store(root).Read().document.at("threads");
    Check(messages.size() == 2 &&
              messages.back().at("messages")[0].at("body") == "{\"公开\":true}" &&
              messages.back().at("messages")[0].at("format") == "json",
          "stream duplicated/missing format");
    auto expiring = Json::parse(protocol.Handle(Request(Tool("buffer_stream_begin")), context).body)
                        .at("result")
                        .at("structuredContent")
                        .at("stream_id");
    now += std::chrono::minutes(6);
    Check(ToolError(protocol.Handle(
              Request(Tool("buffer_stream_append", {{"stream_id", expiring}, {"delta", "late"}})),
              context)),
          "expired stream revived");
    protocol.Reset();
    auto retry_begin = Request(Tool("buffer_stream_begin"));
    retry_begin.headers["idempotency-key"] = "retained-begin";
    const auto first_begin = protocol.Handle(retry_begin, context);
    const auto retry_id =
        Json::parse(first_begin.body).at("result").at("structuredContent").at("stream_id");
    for (int i = 0; i < 255; ++i) {
        auto operation =
            Request(Tool("buffer_stream_append", {{"stream_id", retry_id}, {"delta", ""}}));
        operation.headers["idempotency-key"] = "empty-" + std::to_string(i);
        Check(!ToolError(protocol.Handle(operation, context)), "retry window filled early");
    }
    auto overflow = Request(
        Tool("buffer_stream_append", {{"stream_id", retry_id}, {"delta", "must not append"}}));
    overflow.headers["idempotency-key"] = "overflow";
    Check(protocol.Handle(overflow, context).status == 429,
          "retry overflow evicted active identity");
    Check(protocol.Handle(retry_begin, context).body == first_begin.body,
          "active stream begin identity lost");
    now += std::chrono::minutes(6);
    Check(!ToolError(protocol.Handle(retry_begin, context)),
          "expired retry window never recovered");
    protocol.Reset();
    for (int i = 0; i < 50; ++i)
        Check(!ToolError(protocol.Handle(Request(Tool("buffer_stream_begin")), context)),
              "stream capacity too small");
    Check(ToolError(protocol.Handle(Request(Tool("buffer_stream_begin")), context)),
          "stream capacity unbounded");
    protocol.Reset();
    auto full = Request({{"text", std::string(20000, 'x')}}, "/v1/inbound");
    for (int i = 2; i < 50; ++i)
        Check(protocol.Handle(full, context).status == 200, "pending capacity too small");
    Check(protocol.Handle(full, context).status == 429, "pending review capacity unbounded");
    Check(protocol.Handle(inbound, context).status == 200, "full queue blocked idempotent retry");
}
using rimes::gateway_test::Client;
using rimes::gateway_test::Port;
void Transport(const std::filesystem::path& root, const std::filesystem::path& output) {
    rt::ConfigurationStore config(root / "configuration");
    auto next = config.Load();
    next["gatewayPort"] = Port();
    config.Save(next, next.at("revision"));
    next = config.Load();
    rimes::plugins::PackageStore packages(
        root / "plugins", rimes::plugins::OfficialCatalog(), rimes::plugins::BundledPackages(),
        rimes::plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
    rt::GatewayAccess access(root / "gateway");
    auto token = access.Current();
    Check(token.size() == 32 && access.Current() == token, "token not stable");
    struct stat mode {};
    Check(!stat((root / "gateway/token").c_str(), &mode) && (mode.st_mode & 0777) == 0600,
          "token file permissions");
    auto host = std::make_unique<GatewayServer>(root);
    host->Configure(next);
    host->SetModuleEnabled(true);
    Eventually([&] { return host->Status().at("state") == "protected"; },
               "initial session was not protected");
    host->SetSessionProtected(false);
    Eventually([&] { return host->Status().at("state") == "running"; }, "gateway failed to start");
    unsigned port = next.at("gatewayPort");
    auto client = std::make_unique<Client>(port, token);
    auto inbound =
        Request({{"text", "actual TCP public receipt"}, {"source", "fixture"}}, "/v1/inbound");
    {
        Client unauthorized(port, "");
        Check(unauthorized.Call(inbound).status == 401, "anonymous receipt accepted");
        Check(unauthorized.Call({"GET", "/v1/health", "", {}}).status == 200,
              "public inert health unavailable");
    }
    auto invalid = inbound;
    invalid.headers["origin"] = "https://evil.test";
    Check(client->Call(invalid).status == 403, "remote origin accepted");
    invalid = inbound;
    invalid.headers["host"] = "evil.test";
    Check(client->Call(invalid).status == 403, "DNS rebind host accepted");
    inbound.headers["idempotency-key"] = "transport-retry";
    const auto bytes = client->Wire(inbound);
    client->Send(bytes.substr(0, 5));
    client->Send(bytes.substr(5) + bytes);
    Check(client->Read().status == 200 && client->Read().status == 200,
          "fragmented pipeline failed");
    Check(Store(root / "mailbox").Read().document.at("threads").size() == 1,
          "pipeline duplicated receipt");
    const auto persisted = Store(root / "mailbox").Read().document.at("threads").front();
    Check(persisted.at("unread") == true && persisted.at("review").at("state") == "pending" &&
              persisted.at("workspace") == "inbox",
          "network bypassed review");
    Check(client->Call({"GET", "/mcp", "", {}}).status == 405, "stateless MCP GET should be 405");
    auto init = Request(
        {{"jsonrpc", "2.0"},
         {"id", 1},
         {"method", "initialize"},
         {"params",
          {{"protocolVersion", "future-version"}, {"clientInfo", {{"name", "test-client"}}}}}});
    Check(Json::parse(client->Call(init).body).at("result").at("protocolVersion") ==
              GatewayContract().at("mcp").at("latest"),
          "version negotiation");
    auto badversion = Request(Tool("buffer_push", {{"text", "bad-version"}}));
    badversion.headers["mcp-protocol-version"] = "future";
    Check(client->Call(badversion).status == 400, "unsupported protocol header");
    const auto stream = Json::parse(client->Call(Request(Tool("buffer_stream_begin"))).body)
                            .at("result")
                            .at("structuredContent")
                            .at("stream_id");
    Check(!ToolError(client->Call(Request(Tool(
              "buffer_stream_append", {{"stream_id", stream}, {"delta", "not yet committed"}})))),
          "TCP stream append");
    host->SetInputProtected(true);
    Eventually([&] { return host->Status().at("state") == "protected"; },
               "password context failed to stop listener");
    host->SetInputProtected(false);
    Eventually([&] { return host->Status().at("state") == "running"; },
               "password exit failed to resume");
    client = std::make_unique<Client>(port, token);
    Check(ToolError(client->Call(Request(Tool("buffer_stream_end", {{"stream_id", stream}})))),
          "protected stream revived");
    auto grant = packages.Inspect("builtin.mailbox.inbox");
    packages.Enable(grant.id, false, grant.grant);
    Check(client->Call(inbound).status == 503,
          "disk plugin revocation ignored before event delivery");
    grant = packages.Inspect(grant.id);
    packages.Enable(grant.id, true, grant.grant);
    {
        auto lock = rt::PrivateDirectory(root / "mailbox").Lock(false);
        const auto start = Clock::now();
        auto busy = inbound;
        busy.headers.erase("idempotency-key");
        Check(client->Call(busy).status == 503 && Clock::now() - start < std::chrono::seconds(1),
              "storage lock blocked worker");
    }
    next = config.Load();
    next["gatewayBlockedSources"] = {"http:fixture"};
    config.Save(next, next.at("revision"));
    Check(client->Call(inbound).status == 403, "fresh blocked source setting ignored");
    next = config.Load();
    next["gatewayBlockedSources"] = Json::array();
    config.Save(next, next.at("revision"));
    const auto changed = access.Rotate();
    Check(changed != token && client->Call(inbound).status == 401, "rotated token accepted");
    token = changed;
    client = std::make_unique<Client>(port, token);
    Check(client->Call(inbound).status == 200, "new token refused");
    next = config.Load();
    next["gatewayEnabled"] = false;
    config.Save(next, next.at("revision"));
    Check(client->Call(inbound).status == 503, "fresh disabled configuration ignored");
    host->Configure(config.Load());
    Eventually([&] { return host->Status().at("state") == "disabled"; },
               "disabled listener remained live");
    next = config.Load();
    next["gatewayEnabled"] = true;
    config.Save(next, next.at("revision"));
    host->Configure(config.Load());
    Eventually([&] { return host->Status().at("state") == "running"; }, "enabled listener failed");
    {
        GatewayServer duplicate(root);
        duplicate.Configure(config.Load());
        duplicate.SetModuleEnabled(true);
        duplicate.SetSessionProtected(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Check(Json::parse(*rt::PrivateDirectory(root / "gateway").Read("status.json", 4096))
                      .at("state") == "running",
              "second profile owner overwrote status");
    }
    host->SetModuleEnabled(false);
    Eventually([&] { return host->Status().at("state") == "module-disabled"; },
               "module hook did not stop listener");
    host->SetModuleEnabled(true);
    Eventually([&] { return host->Status().at("state") == "running"; },
               "module hook did not resume");
    host->SetSessionProtected(true);
    Eventually([&] { return host->Status().at("state") == "protected"; },
               "session lock did not close listener");
    host.reset();
    Check(Json::parse(*rt::PrivateDirectory(root / "gateway").Read("status.json", 4096))
                  .at("state") == "stopped",
          "shutdown status");
    auto reopened = std::make_unique<GatewayServer>(root);
    reopened->Configure(config.Load());
    reopened->SetModuleEnabled(true);
    reopened->SetSessionProtected(false);
    Eventually([&] { return reopened->Status().at("state") == "running"; }, "restart failed");
    client = std::make_unique<Client>(port, token);
    const auto before = Store(root / "mailbox").Read().document;
    Check(client->Call(inbound).status == 200 && Store(root / "mailbox").Read().document == before,
          "restart duplicated HTTP receipt");
    if (!output.empty()) {
        const auto document = Store(root / "mailbox").Read().document;
        rt::PrivateDirectory files(output);
        rt::PrivateDirectory(output / "mailbox").Write("mailbox.json", document.dump());
        auto threads = document.at("threads");
        for (auto& thread : threads)
            thread.erase("linuxGateway");
        files.Write("linux-public-fixture.json", Json{{"purpose", "public-parity-fixture"},
                                                      {"threads", threads},
                                                      {"images", Json::object()}}
                                                     .dump());
    }
    const auto status = *rt::PrivateDirectory(root / "gateway").Read("status.json", 4096);
    Check(status.find(token) == status.npos && status.find("actual TCP") == status.npos,
          "status exposed secrets/content");
    Check(rt::GatewayConfiguration("cursor", port).find(token) == std::string::npos &&
              rt::GatewayConfiguration("codex", port, token).find(token) == std::string::npos,
          "preview/Codex config leaked token");
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 1 && !(argc == 3 && std::string(argv[1]) == "--export"))
        return 2;
    const auto root =
        std::filesystem::temp_directory_path() / ("xsime-gateway-test-" + std::to_string(getpid()));
    try {
        Check(!std::filesystem::exists(root), "test directory exists");
        Protocol(root / "protocol");
        Transport(root / "transport",
                  argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
        std::filesystem::remove_all(root);
        std::cout << "Native gateway contract, framing, idempotency, bounded streams, actual TCP, "
                     "revocation, protected states and restart passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Gateway test: " << e.what() << '\n';
        return 1;
    }
}
