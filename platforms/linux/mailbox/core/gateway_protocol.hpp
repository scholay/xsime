#pragma once
#include "mailbox_store.hpp"
#include <chrono>
#include <map>
namespace rimes::mailbox {
struct GatewayRequest {
    std::string method, target, body;
    std::map<std::string, std::string> headers;
};
struct GatewayResponse {
    int status = 200;
    std::string body;
};
enum class ParseResult { Incomplete, Complete, Invalid, TooLarge };
ParseResult ParseGatewayRequest(std::string& incoming, GatewayRequest& request);
bool GatewayOrigin(const std::string& origin);
bool GatewayHost(const std::string& host, unsigned port);
const Json& GatewayContract();
struct GatewayContext {
    std::string client = "mcp", bearer;
    Json configuration;
};
// One worker owns this protocol instance. Streams are ephemeral, bounded and
// are discarded when the host's configuration/authorization epoch changes.
class GatewayProtocol {
  public:
    using CommitFactory = std::function<Commit(const GatewayContext&)>;
    using Now = std::function<std::chrono::steady_clock::time_point()>;
    GatewayProtocol(std::filesystem::path mailbox, CommitFactory commit, Now now = {});
    GatewayResponse Handle(const GatewayRequest&, GatewayContext&);
    void Reset();

  private:
    struct Stream {
        std::string owner, client, title, text, format;
        std::chrono::steady_clock::time_point created;
    };
    struct Cached {
        std::string digest;
        GatewayResponse response;
        std::chrono::steady_clock::time_point created;
    };
    GatewayResponse MCP(const GatewayRequest&, GatewayContext&, const Json&);
    GatewayResponse Tool(const GatewayRequest&, GatewayContext&, const Json&, const Json& id);
    Json Receive(const GatewayRequest&, GatewayContext&, const Json&, const std::string& kind);
    void ValidateText(const std::string&, bool allow_empty = false) const;
    void CheckSource(const GatewayContext&, const std::string& kind, const std::string& name) const;
    void Expire();
    std::filesystem::path mailbox_;
    CommitFactory commit_;
    Now now_;
    std::map<std::string, Stream> streams_;
    std::map<std::string, Cached> replay_;
};
} // namespace rimes::mailbox
