#pragma once
#include "background_jobs.hpp"
#include "configuration.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
namespace rimes::runtime {
enum class AIError { None, Configuration, Cancelled, Authentication, RateLimited, Timeout, Network, Protocol, TooLarge, Incomplete, Busy };
const char* AIErrorMessage(AIError code);
struct AIUpdate { std::string text; bool finished = false; AIError error = AIError::None; std::vector<std::string> blocks; };
using AIReceive = std::function<void(AIUpdate)>;
// Called off the Fcitx event loop. No URL, request, credential or server error
// body is written to logs. Completion messages come from the fixed enum above.
void RunAIRequest(std::string text, RequestConfiguration configuration, std::atomic<bool>& cancelled,
                  AIReceive receive, std::chrono::milliseconds timeout = std::chrono::minutes(2));
class AIWorker {
public:
    ~AIWorker();
    void CancelAll();
    void Shutdown();
    void Run(std::string text, RequestConfiguration configuration,
             std::shared_ptr<std::atomic<bool>> cancelled, AIReceive receive);
private:
    BackgroundJobs jobs_;
};
} // namespace rimes::runtime
