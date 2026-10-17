#include "process.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;

namespace rimes::runtime {
namespace {
struct FD {
    int value = -1;
    ~FD() { if (value >= 0) close(value); }
    void Reset() { if (value >= 0) close(value); value = -1; }
};
// A child may close stdin before the bounded write completes. SIGPIPE must
// not terminate the input method; restore the caller's signal mask afterward.
struct PipeSignal {
    sigset_t previous{}, blocked{};
    PipeSignal() { sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE); pthread_sigmask(SIG_BLOCK, &blocked, &previous); }
    ~PipeSignal() {
        if (!sigismember(&previous, SIGPIPE)) { timespec zero{}; while (sigtimedwait(&blocked, nullptr, &zero) >= 0) {} }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    }
};
struct Child {
    pid_t pid = -1;
    bool reaped = false;
    ~Child() {
        if (pid > 0 && !reaped) {
            kill(-pid, SIGTERM);
            // Keep the leader unreaped so its PID/process-group identity cannot
            // be reused while descendants receive their cleanup grace period.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            kill(-pid, SIGKILL);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
};
void Nonblocking(int fd) {
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) throw std::runtime_error("Process pipe unavailable");
}
}
ProcessResult RunProcess(const std::vector<std::string>& command, std::string_view input,
    const std::atomic<bool>& cancelled, std::chrono::milliseconds timeout,
    std::size_t limit, const std::function<void(std::string_view)>& receive) {
    ProcessResult result;
    if (command.empty() || command.front().empty() || command.front().front() != '/' ||
        input.size() > 2 * 1024 * 1024 || !limit || timeout.count() <= 0) return result;
    if (cancelled.load()) { result.cancelled = true; return result; }
    int to_child[2], from_child[2];
    if (pipe2(to_child, O_CLOEXEC) < 0) return result;
    FD child_in{to_child[0]}, writer{to_child[1]};
    if (pipe2(from_child, O_CLOEXEC) < 0) return result;
    FD reader{from_child[0]}, child_out{from_child[1]}, null{open("/dev/null", O_WRONLY | O_CLOEXEC)};
    if (null.value < 0) return result;
    PipeSignal signals;
    posix_spawn_file_actions_t files;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&files) != 0) return result;
    if (posix_spawnattr_init(&attributes) != 0) { posix_spawn_file_actions_destroy(&files); return result; }
    int setup = posix_spawn_file_actions_adddup2(&files, child_in.value, STDIN_FILENO);
    setup |= posix_spawn_file_actions_adddup2(&files, child_out.value, STDOUT_FILENO);
    setup |= posix_spawn_file_actions_adddup2(&files, null.value, STDERR_FILENO);
    for (const auto fd : {child_in.value, writer.value, reader.value, child_out.value, null.value})
        if (fd > STDERR_FILENO) setup |= posix_spawn_file_actions_addclose(&files, fd);
    setup |= posix_spawnattr_setpgroup(&attributes, 0);
    setup |= posix_spawnattr_setsigmask(&attributes, &signals.previous);
    setup |= posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    std::vector<char*> argv;
    for (const auto& item : command) {
        if (item.find('\0') != std::string::npos) setup = EINVAL;
        argv.push_back(const_cast<char*>(item.c_str()));
    }
    argv.push_back(nullptr);
    Child child;
    const int spawned = setup ? setup : posix_spawn(&child.pid, command.front().c_str(), &files, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attributes);
    if (spawned != 0) { child.pid = -1; return result; }
    child_in.Reset(); child_out.Reset(); null.Reset();
    Nonblocking(writer.value); Nonblocking(reader.value);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t sent = 0;
    siginfo_t exit_info{};
    bool exited = false;
    bool exceeded = false, transport_failed = false;
    while (reader.value >= 0 || !exited) {
        if (cancelled.load() || std::chrono::steady_clock::now() >= deadline) {
            result.cancelled = cancelled.load();
            result.timed_out = !result.cancelled;
            break;
        }
        if (sent == input.size()) writer.Reset();
        pollfd fds[] = {{reader.value, POLLIN, 0}, {writer.value, POLLOUT, 0}};
        if (poll(fds, 2, 20) < 0 && errno != EINTR) { transport_failed = true; break; }
        if (reader.value >= 0) {
            std::array<char, 16384> buffer{};
            for (;;) {
                const auto got = read(reader.value, buffer.data(), buffer.size());
                if (got == 0) { reader.Reset(); break; }
                if (got < 0) { if (errno == EINTR) continue; if (errno != EAGAIN) { transport_failed = true; reader.Reset(); } break; }
                const auto size = static_cast<std::size_t>(got);
                if (size > limit - result.output.size()) { exceeded = true; break; }
                result.output.append(buffer.data(), size);
                if (receive) receive(std::string_view(buffer.data(), size));
            }
        }
        if (exceeded) break;
        if (writer.value >= 0 && (fds[1].revents & (POLLOUT | POLLHUP | POLLERR))) {
            const auto written = write(writer.value, input.data() + sent, input.size() - sent);
            if (written > 0) sent += static_cast<std::size_t>(written);
            else if (written < 0 && errno != EINTR && errno != EAGAIN) writer.Reset();
        }
        if (!exited) {
            const auto waited = waitid(P_PID, child.pid, &exit_info, WEXITED | WNOHANG | WNOWAIT);
            if (waited == 0 && exit_info.si_pid == child.pid) exited = true;
            else if (waited < 0 && errno != EINTR) break;
        }
    }
    // Close pipes before killing a cancelled producer. Never return partial
    // output as a successful result, even if its process exits with status 0.
    result.success = !exceeded && !transport_failed && reader.value < 0 && !result.cancelled && !result.timed_out && exited &&
                     sent == input.size() && exit_info.si_code == CLD_EXITED && exit_info.si_status == 0;
    if (result.success) {
        // A finished one-shot helper may not leave background descendants.
        // Its unreaped leader still reserves this exact process group.
        kill(-child.pid, SIGKILL);
        int status = 0;
        pid_t waited;
        do { waited = waitpid(child.pid, &status, 0); } while (waited < 0 && errno == EINTR);
        child.reaped = waited == child.pid;
        result.success = child.reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (!result.success) result.output.clear();
    return result;
}
} // namespace rimes::runtime
