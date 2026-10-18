#include "configuration.hpp"
#include "gateway_access.hpp"
#include "gateway_server.hpp"
#include "official_catalog.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
namespace rimes::mailbox {
namespace {
using Clock = std::chrono::steady_clock;
std::string Reason(int code) {
    switch (code) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 409:
        return "Conflict";
    case 413:
        return "Content Too Large";
    case 415:
        return "Unsupported Media Type";
    case 429:
        return "Too Many Requests";
    default:
        return "Service Unavailable";
    }
}
std::string Wire(const GatewayResponse& response, bool close) {
    std::string result =
        "HTTP/1.1 " + std::to_string(response.status) + " " + Reason(response.status) +
        "\r\nContent-Length: " + std::to_string(response.body.size()) +
        "\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n";
    if (response.status == 401)
        result += "WWW-Authenticate: Bearer\r\n";
    if (response.status == 405)
        result += "Allow: POST\r\n";
    if (response.status == 429)
        result += "Retry-After: 1\r\n";
    return result + (close ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n") +
           response.body;
}
GatewayResponse Failure(int code, const char* message) {
    return {code, Json{{"error", message}}.dump()};
}
std::string Header(const GatewayRequest& request, const char* name) {
    const auto found = request.headers.find(name);
    return found == request.headers.end() ? "" : found->second;
}
} // namespace
struct GatewayServer::Impl {
    struct Control {
        bool enabled = false, module = false, session = true, input = false;
        unsigned port = 47700;
        std::uint64_t epoch = 0;
        std::string revision;
        bool Active() const { return enabled && module && !session && !input; }
    };
    struct Client {
        int fd;
        GatewayContext context;
        std::string input, output;
        std::size_t sent = 0;
        unsigned requests = 0;
        bool closing = false;
        Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
    };
    std::filesystem::path root;
    mutable std::mutex mutex;
    std::shared_ptr<const Control> control = std::make_shared<Control>();
    Json status{{"state", "starting"}};
    std::atomic<bool> alive{true};
    std::thread worker;
    explicit Impl(std::filesystem::path path) : root(std::move(path)), worker([this] { Run(); }) {}
    ~Impl() {
        alive.store(false);
        if (worker.joinable())
            worker.join();
    }
    template <class F> void Update(F change) {
        std::lock_guard lock(mutex);
        auto next = std::make_shared<Control>(*std::atomic_load(&control));
        if (!change(*next))
            return;
        ++next->epoch;
        std::atomic_store(&control, std::shared_ptr<const Control>(std::move(next)));
    }
    bool Current(std::uint64_t epoch) const {
        const auto state = std::atomic_load(&control);
        return alive.load() && state->Active() && state->epoch == epoch;
    }
    void SetStatus(const char* state, unsigned port) {
        Json next{{"state", state}, {"port", port}, {"pid", getpid()}};
        {
            std::lock_guard lock(mutex);
            if (status == next)
                return;
            status = next;
        }
        try {
            runtime::PrivateDirectory(root / "gateway").Write("status.json", next.dump());
        } catch (...) {
        } // Status reporting cannot grant authority or crash the IME.
    }
    Commit Guard(const GatewayContext& context, std::uint64_t epoch) {
        return [this, context, epoch](const auto& publish) {
            if (!Current(epoch))
                return;
            runtime::GatewayAccess access(root / "gateway");
            runtime::ConfigurationStore configuration(root / "configuration", false);
            plugins::PackageStore packages(root / "plugins", plugins::OfficialCatalog(),
                                           plugins::BundledPackages(), plugins::CompiledAdapters(),
                                           RIMES_PRODUCT_VERSION, false);
            const auto grant = packages.Inspect("builtin.mailbox.inbox");
            access.WithBearer(context.bearer, [&] {
                configuration.WithGateway(context.configuration.at("revision"), [&] {
                    packages.WithEnabledGrant(
                        grant.id, grant.grant,
                        [&] {
                            if (Current(epoch))
                                publish();
                        },
                        false);
                });
            });
        };
    }
    GatewayResponse Handle(const GatewayRequest& request, Client& client, GatewayProtocol& protocol,
                           const Control& state) {
        if (!GatewayHost(Header(request, "host"), state.port))
            return Failure(403, "invalid loopback Host");
        if (request.headers.count("origin") && !GatewayOrigin(Header(request, "origin")))
            return Failure(403, "origin is not allowed");
        if (!Current(state.epoch))
            return Failure(503, "gateway is unavailable");
        // Health contains no profile, module, content or token details.
        if (request.method == "GET" && request.target == "/v1/health")
            return {200, "{\"ok\":true}"};
        const auto authorization = Header(request, "authorization");
        if (authorization.rfind("Bearer ", 0) != 0)
            return Failure(401, "bearer token required");
        try {
            runtime::GatewayAccess access(root / "gateway");
            const auto bearer = authorization.substr(7);
            if (!access.Matches(bearer))
                return Failure(401, "invalid bearer token");
            client.context.bearer = bearer;
            runtime::ConfigurationStore configuration(root / "configuration", false);
            client.context.configuration = configuration.Load();
            if (client.context.configuration.at("gatewayEnabled") != true ||
                client.context.configuration.at("gatewayPort") != state.port)
                return Failure(503, "gateway configuration changed");
            plugins::PackageStore packages(root / "plugins", plugins::OfficialCatalog(),
                                           plugins::BundledPackages(), plugins::CompiledAdapters(),
                                           RIMES_PRODUCT_VERSION, false);
            const auto inbox = packages.Inspect("builtin.mailbox.inbox");
            if (!inbox.available || !inbox.installed || !inbox.enabled)
                return Failure(503, "Mailbox inbox is unavailable");
            return protocol.Handle(request, client.context);
        } catch (const std::exception&) {
            return Failure(503, "gateway storage is unavailable");
        }
    }
    void Run() noexcept {
        int listener = -1;
        std::vector<Client> clients;
        std::unique_ptr<runtime::FileLock> owner;
        auto close_all = [&] {
            for (auto& client : clients)
                close(client.fd);
            clients.clear();
            if (listener >= 0) {
                close(listener);
                listener = -1;
            }
        };
        try {
            std::uint64_t epoch = 0;
            GatewayProtocol protocol(root / "mailbox", [this, &epoch](const auto& context) {
                return Guard(context, epoch);
            });
            auto retry = Clock::now();
            auto refill = Clock::now();
            double tokens = 120;
            while (alive.load()) {
                const auto state = std::atomic_load(&control);
                if (state->epoch != epoch) {
                    epoch = state->epoch;
                    close_all();
                    protocol.Reset();
                    retry = Clock::now();
                }
                if (!owner) {
                    try {
                        auto lock = runtime::PrivateDirectory(root / "gateway/host").Lock(false);
                        owner = std::make_unique<runtime::FileLock>(std::move(lock));
                    } catch (const std::exception&) {
                        // Another owner controls this profile; do not overwrite its status.
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue;
                    }
                }
                if (!state->Active()) {
                    SetStatus(!state->enabled  ? "disabled"
                              : !state->module ? "module-disabled"
                                               : "protected",
                              state->port);
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    continue;
                }
                if (listener < 0 && Clock::now() >= retry) {
                    retry = Clock::now() + std::chrono::seconds(1);
                    try {
                        (void)runtime::GatewayAccess(root / "gateway").Current();
                        listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
                        if (listener < 0)
                            throw std::runtime_error("socket");
                        sockaddr_in address{};
                        address.sin_family = AF_INET;
                        address.sin_port = htons(state->port);
                        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                        int reuse = 1;
                        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
                        if (bind(listener, reinterpret_cast<sockaddr*>(&address),
                                 sizeof(address)) ||
                            listen(listener, 16))
                            throw std::runtime_error("bind");
                        SetStatus("running", state->port);
                    } catch (const std::exception&) {
                        close_all();
                        SetStatus("unavailable", state->port);
                    }
                }
                if (listener < 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    continue;
                }
                std::vector<pollfd> watched{{listener, POLLIN, 0}};
                for (const auto& client : clients)
                    watched.push_back(
                        {client.fd, short(client.output.empty() ? POLLIN : POLLOUT), 0});
                const auto ready = poll(watched.data(), watched.size(), 25);
                if (ready < 0 && errno == EINTR)
                    continue;
                if (ready < 0) {
                    close_all();
                    SetStatus("unavailable", state->port);
                    continue;
                }
                if (!Current(epoch))
                    continue;
                const auto now = Clock::now();
                tokens = std::min(
                    120.0, tokens + std::chrono::duration<double>(now - refill).count() * 120.0);
                refill = now;
                for (std::size_t i = clients.size(); i-- > 0;) {
                    auto& client = clients[i];
                    const auto events = watched[i + 1].revents;
                    bool lost = (events & (POLLERR | POLLNVAL)) != 0;
                    if (client.output.empty() && (events & (POLLIN | POLLHUP))) {
                        char bytes[16384];
                        const auto size = recv(client.fd, bytes, sizeof(bytes), 0);
                        if (size > 0) {
                            if (client.input.empty())
                                client.deadline = now + std::chrono::seconds(5);
                            client.input.append(bytes, size);
                            if (client.input.size() > 32768 + 262144 + sizeof(bytes))
                                lost = true;
                        } else if (size == 0 || (errno != EAGAIN && errno != EINTR))
                            lost = true;
                    }
                    if (!lost && client.output.empty() && !client.input.empty()) {
                        GatewayRequest request;
                        const auto parsed = ParseGatewayRequest(client.input, request);
                        if (parsed == ParseResult::Complete) {
                            client.closing = Header(request, "connection") == "close" ||
                                             ++client.requests >= 100;
                            const auto response = tokens < 1
                                                      ? Failure(429, "request rate limit exceeded")
                                                      : Handle(request, client, protocol, *state);
                            tokens = std::max(0.0, tokens - 1);
                            client.output = Wire(response, client.closing);
                            client.sent = 0;
                            client.deadline = now + std::chrono::seconds(5);
                        } else if (parsed != ParseResult::Incomplete) {
                            client.output =
                                Wire(Failure(parsed == ParseResult::TooLarge ? 413 : 400,
                                             "invalid HTTP request"),
                                     true);
                            client.closing = true;
                        }
                    }
                    if (!lost && !client.output.empty()) {
                        const auto size = send(client.fd, client.output.data() + client.sent,
                                               client.output.size() - client.sent, MSG_NOSIGNAL);
                        if (size > 0)
                            client.sent += size;
                        else if (size < 0 && errno != EAGAIN && errno != EINTR)
                            lost = true;
                        if (client.sent == client.output.size()) {
                            lost = lost || client.closing;
                            client.output.clear();
                            client.sent = 0;
                            client.deadline = now + std::chrono::seconds(30);
                        }
                    }
                    if (now > client.deadline)
                        lost = true;
                    if (lost) {
                        close(client.fd);
                        clients.erase(clients.begin() + i);
                    }
                }
                if (watched[0].revents & POLLIN) {
                    for (unsigned n = 0; n < 16; ++n) {
                        const int fd =
                            accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                        if (fd < 0)
                            break;
                        if (clients.size() >= 16)
                            close(fd);
                        else
                            clients.push_back(Client{fd});
                    }
                }
            }
        } catch (const std::exception&) {
            if (owner)
                SetStatus("unavailable", 0);
        }
        close_all();
        if (owner)
            SetStatus("stopped", 0);
    }
};
GatewayServer::GatewayServer(std::filesystem::path user)
    : impl_(std::make_unique<Impl>(std::move(user))) {}
GatewayServer::~GatewayServer() = default;
void GatewayServer::Configure(const Json& configuration) {
    impl_->Update([&](auto& state) {
        const auto revision = configuration.at("revision").template get<std::string>();
        const bool enabled = configuration.at("gatewayEnabled");
        const unsigned port = configuration.at("gatewayPort");
        if (port < 1024 || port > 65535)
            throw std::runtime_error("invalid gateway port");
        if (state.revision == revision && state.enabled == enabled && state.port == port)
            return false;
        state.enabled = enabled;
        state.port = port;
        state.revision = revision;
        return true;
    });
}
void GatewayServer::SetModuleEnabled(bool value) {
    impl_->Update([&](auto& state) {
        if (state.module == value)
            return false;
        state.module = value;
        return true;
    });
}
void GatewayServer::SetSessionProtected(bool value) {
    impl_->Update([&](auto& state) {
        if (state.session == value)
            return false;
        state.session = value;
        return true;
    });
}
void GatewayServer::SetInputProtected(bool value) {
    impl_->Update([&](auto& state) {
        if (state.input == value)
            return false;
        state.input = value;
        return true;
    });
}
Json GatewayServer::Status() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->status;
}
} // namespace rimes::mailbox
