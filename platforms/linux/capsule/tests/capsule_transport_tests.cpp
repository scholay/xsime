#include "capsule_protocol.hpp"
#include "capsule_transport.hpp"
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
using namespace rimes::capsule;
static void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    int fd[2]{-1, -1};
    try {
        Check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fd) == 0,
              "socketpair");
        int small = 4096;
        setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
        Snapshot copy;
        copy.visible = true;
        copy.copy_seq = 1;
        for (int i = 0; i < 220000; ++i)
            copy.last_copied += "👩🏽‍💻";
        const auto json = EncodeSnapshot(copy);
        Snapshot decoded;
        Check(json.size() > 1024 * 1024 && DecodeSnapshot(json, &decoded) &&
                  decoded.last_copied == copy.last_copied,
              "large Unicode copy was truncated");
        auto frame = std::make_shared<std::string>();
        Check(EncodeFrame(json, frame.get()), "large frame rejected");
        SnapshotWriter writer;
        Check(writer.Queue(frame, false) && writer.Flush(fd[0]) && writer.Pending(),
              "partial nonblocking write expected");
        std::string second, latest;
        Check(EncodeFrame("superseded", &second) && EncodeFrame("latest", &latest), "small frame");
        Check(writer.Queue(std::make_shared<std::string>(second), false) &&
                  writer.Queue(std::make_shared<std::string>(latest), false),
              "coalesce snapshots");
        std::string received;
        for (int attempt = 0; attempt < 20000 &&
                              (writer.Pending() || received.size() < frame->size() + latest.size());
             ++attempt) {
            Check(writer.Flush(fd[0]), "flush");
            char chunk[7919];
            for (;;) {
                const auto size = recv(fd[1], chunk, sizeof(chunk), MSG_DONTWAIT);
                if (size <= 0)
                    break;
                received.append(chunk, size);
            }
        }
        Check(!writer.Pending() && received == *frame + latest,
              "fragmented/coalesced frame content differs");
        Check(writer.Queue(frame, false) && writer.Flush(fd[0]) && writer.Pending(),
              "second partial write expected");
        Check(!writer.Queue(std::make_shared<std::string>(latest), true) && !writer.Pending(),
              "revocation must discard partial private frame");
        const char oversized[4] = {1, 0, 0, 1};
        std::uint32_t length = 0;
        Check(!DecodeFrameHeader(oversized, &length), "oversized frame accepted");
        close(fd[0]);
        close(fd[1]);
        std::cout << "large Unicode copy, fragmented writes, coalescing and "
                     "revocation passed\n";
    } catch (const std::exception& error) {
        for (int item : fd)
            if (item >= 0)
                close(item);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
