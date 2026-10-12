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
    auto protected_snapshot = rimes::buffer::MakeSnapshot(model);
    protected_snapshot.ai_result = "private generated output";
    protected_snapshot.ai_can_copy = true;
    protected_snapshot.ai_can_send = true;
    const auto shielded = rimes::buffer::EncodeSnapshot(protected_snapshot);
    Expect(shielded.find("你好") == std::string::npos, "password field scrubs plaintext");
    Expect(shielded.find("\"secure\":true") != std::string::npos, "secure flag");
    Expect(shielded.find("private generated output") == std::string::npos, "protected snapshot scrubs AI output");
    Expect(shielded.find("\"ai_can_copy\":false") != std::string::npos, "protected snapshot disables result copy");

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

    if (failures != 0) {
        std::cerr << failures << " protocol checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "ok: buffer protocol\n";
    return EXIT_SUCCESS;
}
