#include "local_translation.hpp"
#include "configuration.hpp"
#include "process.hpp"
#include "translation_command.hpp"
namespace rimes::runtime {
LocalTranslationWorker::~LocalTranslationWorker() { Shutdown(); }
void LocalTranslationWorker::Shutdown() {
    jobs_.Shutdown();
}
void LocalTranslationWorker::CancelAll() { jobs_.CancelAll(); }
void LocalTranslationWorker::Run(std::string text, std::string source, std::string target,
    std::shared_ptr<std::atomic<bool>> cancelled, Complete complete) {
    auto work = [text = std::move(text), source = std::move(source), target = std::move(target), cancelled, complete] {
        std::string translated;
        bool valid = false;
        try {
            const auto input = Json{{"op", "translate"}, {"text", text}, {"source", source}, {"target", target}}.dump();
            // Own Python directly: nesting the CLI helper would create another
            // process group and race the two cancellation grace periods.
            const auto result = RunProcess(TranslationCommand(), input, *cancelled, std::chrono::minutes(2));
            const auto response = result.success ? Json::parse(result.output) : Json::object();
            valid = result.success && response.is_object() && response.size() == 1 && response.contains("text") && response["text"].is_string() && !cancelled->load();
            if (valid) translated = response["text"].get<std::string>();
        } catch (const std::exception&) { valid = false; }
        complete(valid ? translated : "", valid);
    };
    if (!jobs_.Run(std::move(cancelled), std::move(work), [complete] { complete("", false); })) complete("", false);
}
}
