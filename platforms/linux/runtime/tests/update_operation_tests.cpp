#include "update_operation.hpp"
#include <filesystem>
#include <future>
#include <iostream>
#include <unistd.h>

int main() {
    using namespace rimes::runtime;
    const auto root = std::filesystem::temp_directory_path() / ("xsime-update-lock-" + RandomID());
    try {
        PrivateDirectory directory(root);
        std::atomic<bool> cancelled{false};
        {
            auto owner = directory.Lock(false);
            auto waiter = std::async(std::launch::async, [&] {
                try { auto lock = WaitForUpdateStorage(directory, cancelled); return false; }
                catch (const std::runtime_error&) { return cancelled.load(); }
            });
            if (waiter.wait_for(std::chrono::milliseconds(120)) != std::future_status::timeout)
                throw std::runtime_error("second operation entered a held update transaction");
            cancelled = true;
            if (waiter.wait_for(std::chrono::seconds(2)) != std::future_status::ready || !waiter.get())
                throw std::runtime_error("cancelled update kept waiting for another operation");
        }
        cancelled = false;
        { auto next = WaitForUpdateStorage(directory, cancelled); directory.Write("sentinel", "retained"); }
        cancelled = true;
        bool rejected = false;
        try { auto next = WaitForUpdateStorage(directory, cancelled); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected || directory.Read("sentinel", 32) != std::optional<std::string>("retained"))
            throw std::runtime_error("cancelled operation entered or changed stored data");
        std::filesystem::remove_all(root);
        std::cout << "Update storage contention, cancellation and retry passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root); std::cerr << error.what() << '\n'; return 1;
    }
}
