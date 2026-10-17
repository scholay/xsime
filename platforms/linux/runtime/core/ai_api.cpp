#include "ai_api.hpp"
#include "chat_codec.hpp"
#include "native_ai_contract.hpp"
#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <memory>
namespace rimes::runtime {
namespace {
struct CurlClose { void operator()(CURL* p) const { if (p) curl_easy_cleanup(p); } };
struct HeaderClose { void operator()(curl_slist* p) const { curl_slist_free_all(p); } };
AIError DecodeError(openai::Error code) {
    return code == openai::Error::TooLarge ? AIError::TooLarge : code == openai::Error::Incomplete ? AIError::Incomplete : AIError::Protocol;
}
struct Transfer {
    CURL* curl;
    std::atomic<bool>& cancelled;
    AIReceive receive;
    openai::Decoder decoder;
    AIError error = AIError::None;
    std::string content_type, previous;
    std::size_t wire = 0;
    std::chrono::steady_clock::time_point last;
    Transfer(CURL* handle, std::atomic<bool>& stop, AIReceive callback) : curl(handle), cancelled(stop), receive(std::move(callback)) {}
};
size_t Header(char* bytes, size_t size, size_t count, void* opaque) noexcept {
    const auto length = size * count;
    auto& state = *static_cast<Transfer*>(opaque);
    try {
        if (length > 16384) return 0;
        std::string line(bytes, length);
        std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c) { return std::tolower(c); });
        if (line.compare(0, 5, "http/") == 0) state.content_type.clear();
        if (line.compare(0, 13, "content-type:") == 0) {
            const auto start = line.find_first_not_of(" \t", 13);
            state.content_type = start == line.npos ? "" : line.substr(start, line.find_first_of("; \t\r\n", start) - start);
        }
        return length;
    } catch (...) { state.error = AIError::Protocol; return 0; }
}
int Progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<Transfer*>(opaque)->cancelled.load() ? 1 : 0;
}
size_t Write(char* bytes, size_t size, size_t count, void* opaque) noexcept {
    auto& state = *static_cast<Transfer*>(opaque);
    if (state.cancelled.load()) return 0;
    if (size && count > openai::kWireLimit / size) { state.error = AIError::TooLarge; return 0; }
    const auto length = size * count;
    if (length > openai::kWireLimit - state.wire) { state.error = AIError::TooLarge; return 0; }
    state.wire += length;
    try {
        long status = 0; curl_easy_getinfo(state.curl, CURLINFO_RESPONSE_CODE, &status);
        if (status != 200) return length;
        if (state.content_type != "text/event-stream") { state.error = AIError::Protocol; return 0; }
        state.decoder.Feed(std::string_view(bytes, length));
        // DONE terminates the protocol even if the server keeps its body open.
        if (state.decoder.Done()) return 0;
        const auto now = std::chrono::steady_clock::now();
        if (now - state.last >= std::chrono::milliseconds(33)) {
            auto preview = state.decoder.Preview();
            if (!preview.empty() && preview != state.previous) {
                state.previous = preview; state.last = now;
                state.receive({std::move(preview), false, AIError::None, {}});
            }
        }
        return length;
    } catch (const openai::Failure& failure) { state.error = DecodeError(failure.code); }
    catch (...) { state.error = AIError::Protocol; }
    return 0;
}
std::string Endpoint(std::string url) {
    if (!ValidEndpoint(url)) throw std::runtime_error("Invalid endpoint");
    while (!url.empty() && url.back() == '/') url.pop_back();
    constexpr std::string_view suffix = "/chat/completions";
    if (url.size() < suffix.size() || url.compare(url.size() - suffix.size(), suffix.size(), suffix) != 0) url += suffix;
    if (!ValidEndpoint(url)) throw std::runtime_error("Invalid endpoint");
    return url;
}
}
const char* AIErrorMessage(AIError code) {
    switch (code) {
        case AIError::None: return "";
        case AIError::Configuration: return "请在连接器中配置 API 地址和模型";
        case AIError::Cancelled: return "生成已取消";
        case AIError::Authentication: return "API 鉴权失败，请检查密钥";
        case AIError::RateLimited: return "API 请求受限，请稍后重试";
        case AIError::Timeout: return "API 请求超时，请重试";
        case AIError::Network: return "无法连接 API，请检查网络和服务地址";
        case AIError::Protocol: return "API 回复格式无效，原文已保留";
        case AIError::TooLarge: return "API 回复过长，请缩短请求后重试";
        case AIError::Incomplete: return "API 回复未完整结束，原文已保留";
        case AIError::Busy: return "前一个请求正在停止，请稍后重试";
    }
    return "生成失败，原文已保留";
}
void RunAIRequest(std::string text, RequestConfiguration configuration, std::atomic<bool>& cancelled,
                  AIReceive receive, std::chrono::milliseconds timeout) {
    AIError error = AIError::None;
    std::string endpoint, body;
    try {
        endpoint = Endpoint(configuration.provider.at("baseURL"));
        body = openai::RequestBody(configuration.provider.at("model").get<std::string>(), text, rimes::native::aiSystemPrompt);
        if (configuration.credential.size() > 8192 || std::any_of(configuration.credential.begin(), configuration.credential.end(),
            [](unsigned char c) { return c <= 32 || c >= 127; })) throw std::runtime_error("Invalid credential");
    } catch (...) { receive({"", true, AIError::Configuration, {}}); return; }
    if (cancelled.load()) { receive({"", true, AIError::Cancelled, {}}); return; }
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    std::unique_ptr<CURL, CurlClose> curl(initialized == CURLE_OK ? curl_easy_init() : nullptr);
    if (!curl) { receive({"", true, AIError::Network, {}}); return; }
    curl_slist* headers = nullptr;
    auto add = [&](const std::string& value) {
        auto* next = curl_slist_append(headers, value.c_str());
        if (!next) { curl_slist_free_all(headers); headers = nullptr; throw std::bad_alloc(); }
        headers = next;
    };
    try {
        add("Content-Type: application/json"); add("Accept: text/event-stream"); add("Expect:");
        if (!configuration.credential.empty()) add("Authorization: Bearer " + configuration.credential);
    } catch (...) { receive({"", true, AIError::Network, {}}); return; }
    std::unique_ptr<curl_slist, HeaderClose> owned_headers(headers);
    Transfer state{curl.get(), cancelled, receive};
    curl_easy_setopt(curl.get(), CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "localhost,127.0.0.1,::1");
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, std::clamp(static_cast<long>(timeout.count()), 1L, 120000L));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, Write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, Header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, Progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    const auto result = curl_easy_perform(curl.get());
    long status = 0; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (cancelled.load()) error = AIError::Cancelled;
    else if (status == 401 || status == 403) error = AIError::Authentication;
    else if (status == 429) error = AIError::RateLimited;
    else if (state.error != AIError::None) error = state.error;
    else if (result == CURLE_OPERATION_TIMEDOUT) error = AIError::Timeout;
    else if (status != 200 || (result != CURLE_OK && !(result == CURLE_WRITE_ERROR && state.decoder.Done()))) error = AIError::Network;
    else {
        try {
            state.decoder.Finish(); auto blocks = state.decoder.Blocks();
            receive({openai::Joined(blocks), true, AIError::None, std::move(blocks)}); return;
        } catch (const openai::Failure& failure) { error = DecodeError(failure.code); }
    }
    receive({"", true, error, {}});
}
AIWorker::~AIWorker() { Shutdown(); }
void AIWorker::Shutdown() { jobs_.Shutdown(); }
void AIWorker::CancelAll() { jobs_.CancelAll(); }
void AIWorker::Run(std::string text, RequestConfiguration configuration,
    std::shared_ptr<std::atomic<bool>> cancelled, AIReceive receive) {
    auto work = [text = std::move(text), configuration = std::move(configuration), cancelled, receive] {
        try { RunAIRequest(text, configuration, *cancelled, receive); }
        catch (...) { try { receive({"", true, AIError::Network, {}}); } catch (...) {} }
    };
    if (!jobs_.Run(std::move(cancelled), std::move(work), [receive] { receive({"", true, AIError::Cancelled, {}}); }))
        receive({"", true, AIError::Busy, {}});
}
} // namespace rimes::runtime
