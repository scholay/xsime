#include "capture_process.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace rimes::capsule {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
Json RunCapture(const std::filesystem::path& output, bool video, bool portal,
                const CaptureControl& control, const std::function<void(const Json&)>& event) {
    if (control.cancel.load())
        return {{"event", "cancelled"}};
    std::array<char, 4096> self{};
    const auto count = readlink("/proc/self/exe", self.data(), self.size() - 1);
    if (count <= 0)
        throw std::runtime_error("无法找到捕获程序");
    auto helper = std::filesystem::path(std::string(self.data(), count)).parent_path() /
                  "rimes-capsule-capture";
    if (access(helper.c_str(), X_OK))
        helper = RIMES_CAPSULE_CAPTURE_PATH;
    std::array<std::string, 5> args = {helper.string(), "--explicit", video ? "video" : "image",
                                       output.string(), portal ? "portal" : "x11"};
    std::array<char*, 6> argv{};
    for (std::size_t i = 0; i < args.size(); ++i)
        argv[i] = args[i].data();
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets))
        throw std::runtime_error("无法启动捕获通信");
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_adddup2(&files, sockets[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&files, sockets[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&files, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&files, sockets[0]);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t pid = -1;
    const auto error = posix_spawn(&pid, helper.c_str(), &files, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&files);
    close(sockets[1]);
    if (error) {
        close(sockets[0]);
        throw std::runtime_error("捕获组件未安装");
    }
    fcntl(sockets[0], F_SETFL, fcntl(sockets[0], F_GETFL) | O_NONBLOCK);
    bool sent_stop = false, aborting = false, reaped = false;
    int status = 0;
    auto abort_at = Clock::time_point{}, stop_at = Clock::time_point{};
    const auto started = Clock::now();
    std::string incoming;
    std::size_t total = 0;
    Json result = {{"event", "error"}, {"message", "捕获进程意外退出"}};
    auto drain = [&] {
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto bytes = recv(sockets[0], buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (bytes < 0 && errno == EINTR)
                continue;
            if (bytes <= 0)
                break;
            incoming.append(buffer.data(), bytes);
            total += bytes;
            if (total > 32768)
                throw std::runtime_error("捕获响应超过上限");
            for (auto at = incoming.find('\n'); at != incoming.npos; at = incoming.find('\n')) {
                const auto response = Json::parse(incoming.substr(0, at));
                incoming.erase(0, at + 1);
                if (!response.is_object())
                    throw std::runtime_error("捕获响应无效");
                if (response.value("event", "") == "finished" ||
                    response.value("event", "") == "error" ||
                    response.value("event", "") == "cancelled")
                    result = response;
                if (!aborting && !control.cancel.load())
                    event(response);
            }
        }
    };
    try {
        while (!reaped) {
            const auto now = Clock::now();
            if (!aborting && (control.cancel.load() || now - started > std::chrono::minutes(13) ||
                              (sent_stop && now - stop_at > std::chrono::seconds(10)))) {
                aborting = true;
                abort_at = now;
                send(sockets[0], "cancel\n", 7, MSG_NOSIGNAL);
            }
            if (!aborting && control.stop.load() && !sent_stop) {
                sent_stop = true;
                stop_at = now;
                send(sockets[0], "stop\n", 5, MSG_NOSIGNAL);
            }
            if (aborting && now - abort_at > std::chrono::seconds(2))
                kill(-pid, SIGTERM);
            if (aborting && now - abort_at > std::chrono::seconds(4))
                kill(-pid, SIGKILL);
            pollfd item{sockets[0], POLLIN, 0};
            poll(&item, 1, 50);
            drain();
            const auto waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                reaped = true;
                // The child can write its final line between recv(EAGAIN) and
                // waitpid. Once reaped, all its remaining output is readable.
                drain();
            } else if (waited < 0 && errno != EINTR)
                throw std::runtime_error("无法回收捕获进程");
        }
    } catch (...) {
        if (!reaped) {
            kill(-pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
        close(sockets[0]);
        throw;
    }
    close(sockets[0]);
    if (control.cancel.load() || aborting)
        return {{"event", "cancelled"}};
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return {{"event", "error"}, {"message", result.value("message", "捕获进程意外退出")}};
    return result;
}
} // namespace rimes::capsule
