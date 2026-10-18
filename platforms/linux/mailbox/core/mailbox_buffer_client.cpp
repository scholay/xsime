#include "buffer_protocol.hpp"
#include "capsule_hash.hpp"
#include "mailbox_buffer_client.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace rimes::mailbox {
namespace {
void Need(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
std::string ReceiveInBuffer(const Json& thread, const std::string& message,
                            const std::function<bool()>& current) {
    std::string path;
    if (const auto* value = std::getenv("RIMES_BUFFER_SOCKET"); value && *value)
        path = value;
    else if (const auto* value = std::getenv("XDG_RUNTIME_DIR"); value && *value)
        path = std::string(value) + "/rimes-buffer.sock";
    else
        path = "/tmp/rimes-buffer-" + std::to_string(geteuid()) + ".sock";
    struct stat info {};
    Need(!lstat(path.c_str(), &info) && S_ISSOCK(info.st_mode) && info.st_uid == geteuid() &&
             (info.st_mode & 0077) == 0,
         "请先启动 Fcitx5 并启用学术海输入法");
    const int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    Need(socket_fd >= 0, "无法连接 Buffer");
    struct FD {
        int value;
        ~FD() { close(value); }
    } owner{socket_fd};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    Need(path.size() < sizeof(address.sun_path), "Buffer 地址过长");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int connected =
        connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    Need(connected == 0, "Buffer 暂时无法连接，请重试");
    ucred peer{};
    socklen_t length = sizeof(peer);
    Need(!getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) && peer.uid == geteuid(),
         "Buffer 身份校验失败");
    auto send_command = [&](const Json& command) {
        std::string frame;
        Need(rimes::buffer::EncodeFrame(command.dump(), &frame), "收件请求过长");
        Need(current() && send(socket_fd, frame.data(), frame.size(), MSG_NOSIGNAL) ==
                              static_cast<ssize_t>(frame.size()),
             "接收已取消或连接中断");
    };
    send_command({{"v", 1}, {"op", "status"}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto request = rimes::capsule::NewUuidV4();
    bool sent = false;
    std::string incoming;
    while (current() && std::chrono::steady_clock::now() < deadline) {
        pollfd wait{socket_fd, POLLIN, 0};
        const auto ready = poll(&wait, 1, 50);
        if (ready < 0 && errno == EINTR)
            continue;
        Need(ready >= 0, "Buffer 连接中断");
        if (!ready)
            continue;
        char bytes[16384];
        const auto size = recv(socket_fd, bytes, sizeof(bytes), 0);
        if (size < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        Need(size > 0 && incoming.size() + size <= rimes::buffer::kMaxFrameBytes + 16384,
             "Buffer 连接中断或响应过大");
        incoming.append(bytes, size);
        while (incoming.size() >= 4) {
            std::uint32_t count = 0;
            Need(rimes::buffer::DecodeFrameHeader(incoming.data(), &count), "Buffer 响应无效");
            if (incoming.size() < count + 4)
                break;
            const auto value = Json::parse(incoming.substr(4, count));
            incoming.erase(0, count + 4);
            if (!sent && value.value("type", "") == "snapshot") {
                Need(!value.value("secure", true) && !value.value("password_field", true),
                     "当前输入状态无法接收，请返回普通输入框后重试");
                if (value.value("mailbox_scope", "").empty())
                    continue;
                send_command({{"v", 1},
                              {"op", "receive_mailbox"},
                              {"id", request},
                              {"thread", thread.at("id")},
                              {"message", message},
                              {"module", Module(thread)},
                              {"revision", Revision(thread)},
                              {"scope", value.at("mailbox_scope")},
                              {"generation", value.at("generation")}});
                sent = true;
            } else if (value.value("type", "") == "mailbox_result" &&
                       value.value("id", "") == request) {
                if (!value.value("ok", false))
                    throw std::runtime_error(value.value("message", "未能加入 Buffer"));
                return value.at("message");
            }
        }
    }
    throw std::runtime_error("接收已取消或超时，请刷新收件状态后重试");
}
} // namespace rimes::mailbox
