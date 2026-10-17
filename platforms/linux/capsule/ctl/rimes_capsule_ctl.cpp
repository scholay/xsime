#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "buffer_protocol.hpp"
#include "capsule_protocol.hpp"
#include "capsule_transport.hpp"

namespace {

std::string DefaultSocketPath() {
    if (const char* override_path = std::getenv("RIMES_CAPSULE_SOCKET")) {
        if (override_path[0] != '\0') {
            return override_path;
        }
    }
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR")) {
        return std::string(runtime) + "/rimes-capsule.sock";
    }
    return "/tmp/rimes-capsule-" + std::to_string(geteuid()) + ".sock";
}

int Connect(const std::string& path, int tries) {
    for (int attempt = 0; attempt < tries; ++attempt) {
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
}

bool SendJson(int fd, const std::string& json) {
    std::string frame;
    if (!rimes::capsule::EncodeFrame(json, &frame)) {
        return false;
    }
    return send(fd, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size());
}

bool ReadSnapshot(int fd, std::string* json, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string incoming;
    while (std::chrono::steady_clock::now() < deadline) {
        char header[4];
        const auto got = recv(fd, header, 4, MSG_DONTWAIT);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            return false;
        }
        if (got == 0) {
            return false;
        }
        incoming.append(header, static_cast<std::size_t>(got));
        if (incoming.size() < 4) {
            continue;
        }
        std::uint32_t length = 0;
        if (!rimes::capsule::DecodeFrameHeader(incoming.data(), &length)) {
            return false;
        }
        incoming.erase(0, 4);
        incoming.reserve(length);
        while (incoming.size() < length) {
            char chunk[4096];
            const auto need = length - incoming.size();
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            const auto n = recv(fd, chunk, need > sizeof(chunk) ? sizeof(chunk) : need, MSG_DONTWAIT);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (n <= 0) {
                return false;
            }
            incoming.append(chunk, static_cast<std::size_t>(n));
        }
        *json = incoming.substr(0, length);
        return true;
    }
    return false;
}

void Usage() {
    std::cerr << "Usage: rimes-capsule-ctl [--socket PATH] "
                 "hello|status|toggle|show|close|next|prev|activate|copy|manage|rail|"
                 "refresh|detail-close\n"
                 "       rimes-capsule-ctl [--socket PATH] module|filter|search TEXT\n"
                 "       rimes-capsule-ctl [--socket PATH] select|detail UUID\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path = DefaultSocketPath();
    std::string op;
    std::string argument;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--socket") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (argv[index][0] != '-') {
            if (op.empty()) op = argv[index];
            else if (argument.empty()) argument = argv[index];
            else { Usage(); return 2; }
        } else {
            Usage();
            return 2;
        }
    }
    if (op.empty()) {
        Usage();
        return 2;
    }
    nlohmann::json encoded{{"v",1},{"op",op}};
    if(op=="select" || op == "detail") encoded["id"]=argument;
    else if(op=="module" || op=="filter" || op=="search") encoded["text"]=argument;
    else if(!argument.empty()) { Usage(); return 2; }
    rimes::capsule::Command parsed;std::string error;
    const auto command=encoded.dump();
    if(!rimes::capsule::ParseCommand(command,&parsed,&error)) { Usage(); return 2; }
    const int fd = Connect(socket_path, 40);
    if (fd < 0) {
        std::cerr << "rimes-capsule-ctl: cannot connect to " << socket_path << '\n';
        return 1;
    }
    std::string connect_snapshot;
    if (!ReadSnapshot(fd, &connect_snapshot, 3000)) {
        close(fd);
        std::cerr << "rimes-capsule-ctl: no snapshot\n";
        return 1;
    }
    if (!SendJson(fd, command)) {
        close(fd);
        return 1;
    }
    if (op == "hello") {
        std::cout << connect_snapshot << '\n';
        close(fd);
        return 0;
    }
    std::string snapshot;
    if (!ReadSnapshot(fd, &snapshot, 3000)) {
        close(fd);
        std::cerr << "rimes-capsule-ctl: no snapshot after " << op << '\n';
        return 1;
    }
    std::cout << snapshot << '\n';
    close(fd);
    return 0;
}
