#include "private_files.hpp"
#include "plugin_store.hpp"
#include "process.hpp"
#include "translation_command.hpp"
#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace { std::atomic<bool> cancelled{false}; }
int main(int argc, char** argv) {
    static_assert(std::atomic<bool>::is_always_lock_free);
    std::signal(SIGTERM, +[](int) { cancelled.store(true); });
    std::signal(SIGINT, +[](int) { cancelled.store(true); });
    try {
        std::string request;
        if (argc == 2 && std::string_view(argv[1]) == "--request") {
            char chunk[4096];
            while (std::cin.read(chunk, sizeof(chunk)) || std::cin.gcount()) {
                request.append(chunk, static_cast<std::size_t>(std::cin.gcount()));
                if (request.size() > 2 * 1024 * 1024) throw std::runtime_error("Request too large");
            }
        } else if (argc == 2 && (std::string_view(argv[1]) == "--status" || std::string_view(argv[1]) == "--install")) {
            request = rimes::plugins::Json{{"op", std::string(argv[1] + 2)}}.dump();
        } else {
            std::cerr << "Usage: rimes-translation --status|--install|--request\n";
            return 2;
        }
        bool emitted = false;
        const auto result = rimes::runtime::RunProcess(rimes::runtime::TranslationCommand(),
            request, cancelled, std::chrono::minutes(10), 1024 * 1024,
            [&](std::string_view data) { emitted = true; std::cout << data << std::flush; });
        if (!result.success && (cancelled.load() || !emitted))
            std::cout << rimes::plugins::Json{{"state", cancelled.load() ? "cancelled" : "error"},
                {"error", cancelled.load() ? "cancelled" : "resources_unavailable"}}.dump() << '\n';
        return result.success ? 0 : cancelled.load() ? 2 : 1;
    } catch (const std::exception&) {
        std::cout << "{\"state\":\"error\",\"error\":\"resources_unavailable\"}\n";
        return 1;
    }
}
