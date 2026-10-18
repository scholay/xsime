#pragma once
#include "gateway_protocol.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
namespace rimes::gateway_test {
using namespace rimes::mailbox;
inline void Check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
inline unsigned Port() {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    Check(fd >= 0, "port socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(!bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), "port bind");
    socklen_t size = sizeof(address);
    Check(!getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size), "port address");
    close(fd);
    return ntohs(address.sin_port);
}
struct Client {
    int fd;
    unsigned port;
    std::string bearer, buffer;
    Client(unsigned p, std::string token) : port(p), bearer(std::move(token)) {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        Check(fd >= 0, "client socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(p);
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
            close(fd);
            throw std::runtime_error("gateway connect");
        }
        timeval timeout{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }
    ~Client() { close(fd); }
    void Send(const std::string& bytes) {
        for (std::size_t n = 0; n < bytes.size();) {
            const auto count = send(fd, bytes.data() + n, bytes.size() - n, MSG_NOSIGNAL);
            Check(count > 0, "send failed");
            n += count;
        }
    }
    std::string Wire(GatewayRequest r) {
        r.headers.emplace("host", "127.0.0.1:" + std::to_string(port));
        if (!bearer.empty())
            r.headers.emplace("authorization", "Bearer " + bearer);
        r.headers["content-length"] = std::to_string(r.body.size());
        auto bytes = r.method + " " + r.target + " HTTP/1.1\r\n";
        for (const auto& [key, value] : r.headers)
            bytes += key + ": " + value + "\r\n";
        return bytes + "\r\n" + r.body;
    }
    GatewayResponse Read() {
        for (;;) {
            auto split = buffer.find("\r\n\r\n");
            if (split != buffer.npos) {
                const auto at = buffer.find("Content-Length: ");
                Check(at != buffer.npos, "missing response length");
                auto length = std::stoul(buffer.substr(at + 16));
                if (buffer.size() >= split + 4 + length) {
                    GatewayResponse result{std::stoi(buffer.substr(9, 3)),
                                           buffer.substr(split + 4, length)};
                    buffer.erase(0, split + 4 + length);
                    return result;
                }
            }
            char bytes[16384];
            const auto count = recv(fd, bytes, sizeof(bytes), 0);
            Check(count > 0, "response EOF/timeout");
            buffer.append(bytes, count);
        }
    }
    GatewayResponse Call(GatewayRequest r) {
        Send(Wire(std::move(r)));
        return Read();
    }
};

} // namespace rimes::gateway_test
