#include "buffer_protocol.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace rimes::buffer {
namespace {

void AppendEscaped(std::ostringstream& out, std::string_view text) {
    out << '"';
    for (unsigned char byte : text) {
        switch (byte) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (byte < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", byte);
                    out << buf;
                } else {
                    out << static_cast<char>(byte);
                }
                break;
        }
    }
    out << '"';
}

void SkipWs(std::string_view json, std::size_t* pos) {
    while (*pos < json.size() && std::isspace(static_cast<unsigned char>(json[*pos])) != 0) {
        ++*pos;
    }
}

bool FindKey(std::string_view json, std::string_view key, std::size_t* value_pos) {
    const std::string needle = std::string("\"") + std::string(key) + "\"";
    std::size_t search = 0;
    while (search < json.size()) {
        const auto found = json.find(needle, search);
        if (found == std::string_view::npos) {
            return false;
        }
        std::size_t pos = found + needle.size();
        SkipWs(json, &pos);
        if (pos < json.size() && json[pos] == ':') {
            ++pos;
            SkipWs(json, &pos);
            *value_pos = pos;
            return true;
        }
        search = found + 1;
    }
    return false;
}

bool ParseStringValue(std::string_view json, std::size_t pos, std::string* output) {
    if (pos >= json.size() || json[pos] != '"') {
        return false;
    }
    ++pos;
    std::string raw;
    while (pos < json.size()) {
        const char ch = json[pos];
        if (ch == '"') {
            return JsonUnescape(raw, output);
        }
        raw.push_back(ch);
        if (ch == '\\' && pos + 1 < json.size()) {
            raw.push_back(json[pos + 1]);
            pos += 2;
            continue;
        }
        ++pos;
    }
    return false;
}

int ReadInt(std::string_view json, std::string_view key, int fallback) {
    std::size_t pos = 0;
    if (!FindKey(json, key, &pos)) {
        return fallback;
    }
    char* end = nullptr;
    const auto value = std::strtol(json.data() + pos, &end, 10);
    if (end == json.data() + pos) {
        return fallback;
    }
    return static_cast<int>(value);
}

std::string ReadString(std::string_view json, std::string_view key) {
    std::size_t pos = 0;
    std::string value;
    if (!FindKey(json, key, &pos)) {
        return {};
    }
    if (!ParseStringValue(json, pos, &value)) {
        return {};
    }
    return value;
}

}  // namespace

Snapshot MakeSnapshot(const BufferModel& model) {
    Snapshot snapshot;
    snapshot.generation = model.generation();
    snapshot.visible = model.visible();
    snapshot.capturing = model.capture_enabled();
    snapshot.enabled = model.processing_active();
    snapshot.secure = model.secure() || model.password_field();
    snapshot.password_field = model.password_field();
    snapshot.close_after_last = model.close_after_last();
    snapshot.empty = model.empty();
    snapshot.insertion_index = model.insertion_index();
    snapshot.hold_progress = model.hold_progress();
    snapshot.preedit = model.preedit();
    snapshot.placeholder = kPlaceholder;
    snapshot.target = model.target_name();
    snapshot.caret = model.caret();
    if (snapshot.secure) {
        snapshot.preedit.clear();
        snapshot.staged_text.clear();
        snapshot.blocks.clear();
        snapshot.empty = true;
        return snapshot;
    }
    snapshot.staged_text = model.staged_text();
    snapshot.blocks = model.blocks();
    return snapshot;
}

std::string EncodeSnapshot(const Snapshot& snapshot) {
    std::ostringstream out;
    out << "{\"v\":" << kProtocolVersion << ",\"type\":\"snapshot\"";
    out << ",\"generation\":" << snapshot.generation;
    out << ",\"visible\":" << (snapshot.visible ? "true" : "false");
    out << ",\"capturing\":" << (snapshot.capturing ? "true" : "false");
    out << ",\"enabled\":" << (snapshot.enabled ? "true" : "false");
    out << ",\"secure\":" << (snapshot.secure ? "true" : "false");
    out << ",\"password_field\":" << (snapshot.password_field ? "true" : "false");
    out << ",\"close_after_last\":" << (snapshot.close_after_last ? "true" : "false");
    out << ",\"empty\":" << (snapshot.empty ? "true" : "false");
    out << ",\"insertion_index\":" << snapshot.insertion_index;
    char progress[32];
    std::snprintf(progress, sizeof(progress), "%.3f", snapshot.hold_progress);
    out << ",\"hold_progress\":" << progress;
    out << ",\"preedit\":";
    AppendEscaped(out, snapshot.preedit);
    out << ",\"placeholder\":";
    AppendEscaped(out, snapshot.placeholder);
    out << ",\"target\":";
    AppendEscaped(out, snapshot.target);
    out << ",\"staged_text\":";
    AppendEscaped(out, snapshot.staged_text);
    out << ",\"caret\":{\"x\":" << snapshot.caret.x << ",\"y\":" << snapshot.caret.y
        << ",\"w\":" << snapshot.caret.width << ",\"h\":" << snapshot.caret.height
        << ",\"valid\":" << (snapshot.caret.valid ? "true" : "false") << "}";
    out << ",\"ui_clients\":" << snapshot.ui_clients;
    out << ",\"blocks\":[";
    for (std::size_t index = 0; index < snapshot.blocks.size(); ++index) {
        const auto& block = snapshot.blocks[index];
        if (index != 0) {
            out << ',';
        }
        out << "{\"id\":";
        AppendEscaped(out, block.id);
        out << ",\"text\":";
        AppendEscaped(out, block.text);
        out << ",\"origin\":";
        AppendEscaped(out, OriginTag(block.origin));
        out << ",\"created_at_ms\":" << block.created_at_ms << '}';
    }
    out << "],\"ai_mode\":"; AppendEscaped(out, snapshot.ai_mode);
    out << ",\"ai_status\":"; AppendEscaped(out, snapshot.ai_status);
    out << ",\"ai_result\":"; AppendEscaped(out, snapshot.secure ? "" : snapshot.ai_result);
    out << ",\"ai_result_id\":"; AppendEscaped(out, snapshot.ai_result_id);
    out << ",\"suggestion_text\":"; AppendEscaped(out, snapshot.secure ? "" : snapshot.suggestion_text);
    out << ",\"suggestion_kind\":"; AppendEscaped(out, snapshot.suggestion_kind);
    out << ",\"suggestion_id\":"; AppendEscaped(out, snapshot.suggestion_id);
    out << ",\"accept_key\":"; AppendEscaped(out, snapshot.accept_key);
    out << ",\"model_status\":"; AppendEscaped(out, snapshot.model_status);
    out << ",\"suggestion_ready\":" << (snapshot.suggestion_ready && !snapshot.secure ? "true" : "false");
    out << ",\"source_can_send\":" << (snapshot.source_can_send && !snapshot.secure ? "true" : "false");
    out << ",\"ai_busy\":" << (snapshot.ai_busy ? "true" : "false");
    out << ",\"ai_can_copy\":" << (snapshot.ai_can_copy && !snapshot.secure ? "true" : "false");
    out << ",\"ai_can_send\":" << (snapshot.ai_can_send && !snapshot.secure ? "true" : "false") << '}';
    return out.str();
}

std::string EncodeError(std::string_view code, std::string_view message) {
    std::ostringstream out;
    out << "{\"v\":" << kProtocolVersion << ",\"type\":\"error\",\"code\":";
    AppendEscaped(out, code);
    out << ",\"message\":";
    AppendEscaped(out, message);
    out << '}';
    return out.str();
}

bool ParseCommand(std::string_view json, Command* command, std::string* error) {
    if (command == nullptr) {
        return false;
    }
    *command = Command{};
    const std::string op = ReadString(json, "op");
    command->result_id = ReadString(json, "result_id");
    if (op.empty()) {
        if (error != nullptr) {
            *error = "missing op";
        }
        return false;
    }
    if (op == "ai_mode") {
        command->op = CommandOp::AiMode;
        command->text = ReadString(json, "mode");
    } else if (op == "ai_generate") {
        command->op = CommandOp::AiGenerate;
    } else if (op == "ai_cancel") {
        command->op = CommandOp::AiCancel;
    } else if (op == "ai_reload") {
        command->op = CommandOp::AiReload;
    } else if (op == "ai_copy") {
        command->op = CommandOp::AiCopy;
    } else if (op == "ai_accept") {
        command->op = CommandOp::AiAccept;
    } else if (op == "ai_send") {
        command->op = CommandOp::AiSend;
    } else if (op == "hello" || op == "status") {
        command->op = op == "hello" ? CommandOp::Hello : CommandOp::Query;
    } else if (op == "toggle") {
        command->op = CommandOp::Toggle;
    } else if (op == "show") {
        command->op = CommandOp::Show;
    } else if (op == "close") {
        command->op = CommandOp::Close;
    } else if (op == "send_next") {
        command->op = CommandOp::SendNext;
    } else if (op == "send_all") {
        command->op = CommandOp::SendAll;
    } else if (op == "remove_last") {
        command->op = CommandOp::RemoveLast;
    } else if (op == "select_all") {
        command->op = CommandOp::SelectAll;
    } else if (op == "paste" || op == "import_clipboard") {
        command->op = CommandOp::Paste;
        command->text = ReadString(json, "text");
        if (command->text.empty()) {
            if (error != nullptr) {
                *error = "paste requires text";
            }
            return false;
        }
    } else if (op == "set_insertion") {
        command->op = CommandOp::SetInsertion;
        command->insertion_index = ReadInt(json, "index", -1);
        if (command->insertion_index < 0) {
            if (error != nullptr) {
                *error = "set_insertion requires index";
            }
            return false;
        }
    } else if (op == "drag_begin") {
        command->op = CommandOp::DragBegin;
    } else if (op == "drag_end") {
        command->op = CommandOp::DragEnd;
    } else {
        command->op = CommandOp::Unknown;
        if (error != nullptr) {
            *error = "unknown op";
        }
        return false;
    }
    return true;
}

std::string JsonEscape(std::string_view text) {
    std::ostringstream out;
    AppendEscaped(out, text);
    return out.str();
}

bool JsonUnescape(std::string_view text, std::string* output) {
    if (output == nullptr) {
        return false;
    }
    output->clear();
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch != '\\') {
            output->push_back(ch);
            continue;
        }
        if (index + 1 >= text.size()) {
            return false;
        }
        const char next = text[++index];
        switch (next) {
            case '"':
            case '\\':
            case '/':
                output->push_back(next);
                break;
            case 'b':
                output->push_back('\b');
                break;
            case 'f':
                output->push_back('\f');
                break;
            case 'n':
                output->push_back('\n');
                break;
            case 'r':
                output->push_back('\r');
                break;
            case 't':
                output->push_back('\t');
                break;
            case 'u':
                if (index + 4 >= text.size()) {
                    return false;
                }
                output->push_back('?');
                index += 4;
                break;
            default:
                return false;
        }
    }
    return true;
}

bool EncodeFrame(std::string_view payload, std::string* frame) {
    if (frame == nullptr || payload.size() > kMaxFrameBytes) {
        return false;
    }
    const auto length = static_cast<std::uint32_t>(payload.size());
    frame->assign(4, '\0');
    (*frame)[0] = static_cast<char>((length >> 24) & 0xFF);
    (*frame)[1] = static_cast<char>((length >> 16) & 0xFF);
    (*frame)[2] = static_cast<char>((length >> 8) & 0xFF);
    (*frame)[3] = static_cast<char>(length & 0xFF);
    frame->append(payload.data(), payload.size());
    return true;
}

bool DecodeFrameHeader(const char header[4], std::uint32_t* length) {
    if (header == nullptr || length == nullptr) {
        return false;
    }
    const auto value = (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
                       static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
    if (value > kMaxFrameBytes) {
        return false;
    }
    *length = value;
    return true;
}

}  // namespace rimes::buffer
