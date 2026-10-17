#include <cstdlib>
#include <iostream>
#include <string>

#include "buffer_model.hpp"
#include "buffer_protocol.hpp"

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
    using rimes::buffer::BufferModel;
    using rimes::buffer::Command;
    using rimes::buffer::CommandOp;
    using rimes::buffer::Origin;

    BufferModel model;
    model.activate_capture("tok");
    model.set_visible(true);
    model.append("你好", Origin::Rime);
    model.set_preedit("nihao");
    const auto snapshot = rimes::buffer::MakeSnapshot(model);
    const auto json = rimes::buffer::EncodeSnapshot(snapshot);
    Expect(json.find("\"type\":\"snapshot\"") != std::string::npos, "snapshot type");
    Expect(json.find("你好") != std::string::npos, "snapshot includes block text");
    Expect(json.find("nihao") != std::string::npos, "snapshot includes preedit");
    Expect(json.find("\"capturing\":true") != std::string::npos, "capturing flag");

    model.set_password_field(true);
    const auto shielded = rimes::buffer::EncodeSnapshot(rimes::buffer::MakeSnapshot(model));
    Expect(shielded.find("你好") == std::string::npos, "password field scrubs plaintext");
    Expect(shielded.find("\"secure\":true") != std::string::npos, "secure flag");

    std::string frame;
    Expect(rimes::buffer::EncodeFrame(json, &frame), "frame encodes");
    std::uint32_t length = 0;
    Expect(rimes::buffer::DecodeFrameHeader(frame.data(), &length), "frame header");
    Expect(length == json.size(), "frame length matches payload");

    Command command;
    std::string error;
    Expect(rimes::buffer::ParseCommand(R"({"v":1,"op":"send_next"})", &command, &error),
           "parse send_next");
    Expect(command.op == CommandOp::SendNext, "send_next op");
    Expect(rimes::buffer::ParseCommand(R"({"op":"paste","text":"hello \"x\""})", &command, &error),
           "parse paste");
    Expect(command.op == CommandOp::Paste && command.text == "hello \"x\"", "paste text");
    Expect(rimes::buffer::ParseCommand(R"({"op":"drag_begin"})", &command, &error),
           "parse drag_begin");
    Expect(command.op == CommandOp::DragBegin, "drag_begin op");
    Expect(!rimes::buffer::ParseCommand(R"({"op":"nope"})", &command, &error), "unknown op fails");

    const auto escaped = rimes::buffer::JsonEscape("a\"b\\c");
    Expect(escaped == "\"a\\\"b\\\\c\"", "json escape");

    Expect(rimes::buffer::ParseCommand(R"({"op":"paste","text":"\u4f60\u597d\ud83d\ude42"})", &command, &error)
           && command.text == "你好🙂", "Unicode command survives IPC");
    for (const auto* bad : {R"({"nested":{"op":"send_all"}})", R"({"op":"send_all"} garbage)",
            R"({"op":"paste","text":"\ud800"})", R"({"op":"paste","text":"\u0000"})",
            R"({"op":"set_insertion","index":4294967297})", R"({"op":"set_insertion","index":1.5})",
            R"({"v":2,"op":"show"})", R"({"op":"select_result"})"})
        Expect(!rimes::buffer::ParseCommand(bad, &command, &error), "invalid command rejected");
    auto dual = snapshot;
    dual.active_plugin = "builtin.fixture";
    dual.phase = rimes::buffer::DerivedPhase::Ready;
    dual.results = {{"result", "Result includes ],{ and 中文🙂", 0, true}};
    dual.selected_result = "result";
    dual.result_blocks = {{"child-1", "Result includes ", rimes::buffer::Origin::Local, 0},
                          {"child-2", "],{ and 中文🙂", rimes::buffer::Origin::Local, 0}};
    rimes::buffer::Snapshot decoded;
    Expect(rimes::buffer::ParseSnapshot(rimes::buffer::EncodeSnapshot(dual), &decoded), "dual snapshot round trip");
    Expect(decoded.results.size() == 1 && decoded.results.front().text == dual.results.front().text &&
           decoded.blocks.front().text == snapshot.blocks.front().text, "nested arrays and Unicode remain distinct");
    Expect(decoded.result_blocks.size() == 2 && decoded.result_blocks.back().text == "],{ and 中文🙂",
           "derived segment ranges preserve Unicode text without duplicate transport");
    auto invalid_parts = dual;
    invalid_parts.result_blocks[0].text += "x";
    Expect(!rimes::buffer::ParseSnapshot(rimes::buffer::EncodeSnapshot(invalid_parts), &decoded), "invalid result segment lengths rejected");
    Expect(!rimes::buffer::ParseSnapshot(R"({"type":"snapshot","visible":"true"})", &decoded), "wrong snapshot types rejected");
    dual.secure = true;
    dual.target = "private-target"; dual.caret = {123, 456, 20, 30, true};
    dual.target = "private-target-name";
    const auto protected_dual = rimes::buffer::EncodeSnapshot(dual);
    Expect(protected_dual.find("Result includes") == std::string::npos && protected_dual.find("你好") == std::string::npos,
           "protected snapshot scrubs both rails");
    Expect(protected_dual.find("private-target-name") == std::string::npos, "protected IPC scrubs target identity before rendering");
    Expect(rimes::buffer::ParseSnapshot(protected_dual, &decoded) && decoded.results.empty() && decoded.blocks.empty() && decoded.result_blocks.empty(),
           "protected renderer has no plaintext");
    Expect(decoded.target.empty() && !decoded.caret.valid && protected_dual.find("private-target") == std::string::npos,
           "protected target/caret leaked");
    dual.secure = false; dual.password_field = true;
    Expect(rimes::buffer::EncodeSnapshot(dual).find("private-target") == std::string::npos, "password-only target leaked");
    std::string unescaped;
    Expect(rimes::buffer::JsonUnescape(R"(\u4f60\u597d\ud83d\ude42)", &unescaped) && unescaped == "你好🙂",
           "Capsule shared unescape preserves Unicode");

    if (failures != 0) {
        std::cerr << failures << " protocol checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "ok: buffer protocol\n";
    return EXIT_SUCCESS;
}
