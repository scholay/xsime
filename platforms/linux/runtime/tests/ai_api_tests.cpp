#include "ai_api.hpp"
#include <iostream>
#include <stdexcept>
using namespace rimes::runtime;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string base = argv[1];
    auto configuration = [&](const std::string& path) {
        return RequestConfiguration{"fixture", {}, {{"baseURL", base + path}, {"model", "fixture-model"}}, "fixture-credential"};
    };
    for (const auto& [path, expected] : std::vector<std::pair<std::string, AIError>>{
        {"/fragmented", AIError::None}, {"/auth", AIError::Authentication}, {"/limited", AIError::RateLimited},
        {"/redirect", AIError::Network}, {"/incomplete", AIError::Incomplete}, {"/malformed", AIError::Protocol},
        {"/tool", AIError::Protocol}, {"/oversized", AIError::TooLarge}, {"/provider", AIError::Protocol},
        {"/timeout", AIError::Timeout}, {"/cancel", AIError::Cancelled}}) {
        std::atomic<bool> cancelled{false};
        std::thread cancel;
        if (path == "/cancel") cancel = std::thread([&] { std::this_thread::sleep_for(std::chrono::milliseconds(40)); cancelled.store(true); });
        int updates = 0, completions = 0;
        AIUpdate final;
        const auto start = std::chrono::steady_clock::now();
        RunAIRequest("原文🙂", configuration(path), cancelled, [&](AIUpdate event) {
            if (event.finished) { final = event; ++completions; }
            else { Check(event.text.find("fixture-credential") == event.text.npos, "credential leaked in preview"); ++updates; }
        }, path == "/timeout" ? std::chrono::milliseconds(80) : std::chrono::seconds(3));
        if (cancel.joinable()) cancel.join();
        Check(completions == 1 && final.error == expected, ("unexpected completion for " + path).c_str());
        if (expected == AIError::None) {
            Check(updates > 0 && final.text == "First 🙂.第二段。" && final.blocks.size() == 2, "stream did not preserve logical output");
            Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "DONE waited for HTTP EOF");
        } else Check(final.text.empty() && final.blocks.empty(), "failed request became deliverable");
    }
    for (int scenario = 0; scenario < 3; ++scenario) {
        auto config = configuration("/trap");
        if (scenario == 0) config.credential = "bad\r\nInjected: header";
        if (scenario == 1) config.provider["baseURL"] = "http://remote.invalid/v1";
        if (scenario == 2) config.provider["model"] = "";
        std::atomic<bool> cancelled{false};
        AIUpdate result;
        RunAIRequest("原文🙂", config, cancelled, [&](AIUpdate event) { result = std::move(event); });
        Check(result.error == AIError::Configuration, "invalid configuration reached network");
    }
    {
        AIWorker worker;
        std::mutex mutex;
        std::condition_variable changed;
        bool release = false, replacement = false;
        unsigned started = 0, completed = 0;
        AIError replacement_error = AIError::Busy;
        try {
            for (unsigned i = 0; i < 4; ++i) {
                worker.Run("原文🙂", configuration("/fragmented"), std::make_shared<std::atomic<bool>>(false),
                    [&, held = false](AIUpdate update) mutable {
                        std::unique_lock lock(mutex);
                        if (!update.finished && !held) {
                            held = true; ++started; changed.notify_all();
                            changed.wait(lock, [&] { return release; });
                        }
                        if (update.finished) { ++completed; changed.notify_all(); }
                    });
            }
            {
                std::unique_lock lock(mutex);
                Check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return started == 4; }), "AI streams did not start");
            }
            worker.CancelAll();
            unsigned cancelled_queued = 0;
            AIError queued_error = AIError::None;
            worker.Run("queued", configuration("/fragmented"), std::make_shared<std::atomic<bool>>(false), [&](AIUpdate update) {
                if(update.finished) { ++cancelled_queued; queued_error=update.error; }
            });
            worker.CancelAll();
            Check(cancelled_queued==1 && queued_error==AIError::Cancelled, "queued AI cancellation lost its terminal callback");
            worker.Run("原文🙂", configuration("/fragmented"), std::make_shared<std::atomic<bool>>(false), [&](AIUpdate update) {
                if (update.finished) {
                    std::lock_guard lock(mutex);
                    replacement = true; replacement_error = update.error; changed.notify_all();
                }
            });
            {
                std::unique_lock lock(mutex);
                Check(!replacement, "cancelled AI callbacks blocked the replacement request");
                release = true; changed.notify_all();
                Check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return replacement && completed == 4; }), "AI replacement did not complete");
                Check(replacement_error == AIError::None, "AI replacement lost its request");
            }
            worker.Shutdown();
            worker.Shutdown(); // teardown is explicitly idempotent
        } catch (...) {
            { std::lock_guard lock(mutex); release = true; changed.notify_all(); }
            worker.Shutdown();
            throw;
        }
    }
    std::cout << "AI HTTP: native contract, fragmented stream, early DONE, cancellation, timeout, auth, limits, redirect, size guards and joined shutdown passed\n";
}
