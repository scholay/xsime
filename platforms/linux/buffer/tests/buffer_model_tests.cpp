#include <cstdlib>
#include <iostream>
#include <string>

#include "buffer_model.hpp"
#include "buffer_application.hpp"

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    {
        rimes::buffer::BufferModel source;
        source.append("甲乙丙丁");
        const auto id = source.blocks().front().id;
        const auto original = source.staged_text();
        Expect(!source.consume_source({{id, "乙", 4}}) && source.staged_text() == original, "partial UTF-8 range rejected without mutation");
        Expect(!source.consume_source({{id, "乙丙", 3}, {id, "丙", 6}}), "overlapping ranges rejected atomically");
        Expect(!source.consume_source({{id, "乙丙", 3}, {"missing", "值", 0}}) && source.staged_text() == original, "invalid second range cannot partially retire first");
        Expect(source.consume_source({{id, "乙丙", 3}}) && source.staged_text() == "甲丁" && source.blocks().front().id == id, "exact source consumption preserves surrounding text and identity");
        Expect(!source.consume_source({{id, "乙丙", 3}}), "source cannot be consumed twice");
    }
    for (const auto* program : {"rimes-settings", "/usr/bin/rimes-settings", "org.scholay.rimes.settings", "org.scholay.rimes.settings.desktop", "/usr/libexec/rimes/rimes-buffer", "rimes-schemes"})
        Expect(rimes::buffer::IsCompanionApplication(program), "companion identity must preserve staging");
    for (const auto* program : {"", "editor", "rimes-settings-other", "org.scholay.rimes.settings.other", "/rimes-settings/editor"})
        Expect(!rimes::buffer::IsCompanionApplication(program), "external application identity must not match a prefix");
    using rimes::buffer::BufferModel;
    using rimes::buffer::InputRoute;
    using rimes::buffer::Origin;

    BufferModel model;
    Expect(model.empty(), "fresh model is empty");
    Expect(model.input_route() == InputRoute::DirectToHost, "default route is direct");
    Expect(!model.visible(), "default hidden");

    model.activate_capture("ic-1");
    Expect(model.captures("ic-1"), "capture binds the token");
    Expect(!model.captures("ic-2"), "other token is not captured");
    Expect(model.append("你好", Origin::Rime), "rime commit appends");
    Expect(model.append("世界", Origin::Rime), "second commit appends");
    Expect(model.pending_count() == 2, "two staged blocks");
    Expect(model.staged_text() == "你好世界", "concatenated staged text");
    Expect(model.insertion_index() == 2, "caret after last block");

    model.consume_delivered({model.blocks().front().id});
    Expect(model.pending_count() == 1, "sendNext consumes one block");
    Expect(model.staged_text() == "世界", "remaining block is untouched");

    model.pause_capture_preserving_content();
    Expect(!model.capture_enabled(), "pause clears capture");
    Expect(model.pending_count() == 1, "pause preserves blocks");
    Expect(!model.visible(), "pause hides the workbench");

    model.resume_workbench_processing();
    Expect(model.visible(), "resume shows content");
    Expect(!model.capture_enabled(), "resume does not steal keys");

    model.activate_capture("ic-1");
    model.route_direct_preserving_content("focus-changed");
    Expect(!model.captures("ic-1"), "focus change returns to direct");
    Expect(model.pending_count() == 1, "focus change keeps blocks");

    Expect(model.append_direct_fragment("hel", "ic-1"), "direct fragment");
    Expect(model.append_direct_fragment("lo ", "ic-1"), "direct tail grows");
    Expect(model.delete_backward_direct("ic-1"), "direct backspace");
    Expect(model.remove_last_block(), "block backspace");

    model.select_all();
    Expect(model.all_content_selected(), "select all");
    Expect(model.insert_pasted_text("paste me", Origin::Clipboard), "paste replaces selection");
    Expect(!model.staged_text().empty(), "paste produced text");

    const auto before_privacy = model.pending_count();
    Expect(before_privacy > 0, "privacy starts with content");
    model.discard_for_privacy();
    Expect(model.empty(), "privacy discard clears blocks");

    BufferModel segmenter;
    const auto parts = segmenter.SplitSegments("Hello world this is four. Next");
    Expect(parts.size() >= 2, "latin phrases split");
    std::string joined;
    for (const auto& part : parts) {
        joined += part;
    }
    Expect(joined == "Hello world this is four. Next", "segmenter is lossless");

    const auto sentences = segmenter.SplitSegments("你好。世界！");
    Expect(sentences.size() == 2, "CJK sentence punctuation splits");

    if (failures != 0) {
        std::cerr << failures << " buffer model checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "ok: buffer model\n";
    return EXIT_SUCCESS;
}
