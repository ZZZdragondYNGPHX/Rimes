#include "ai_workspace.hpp"
#include <algorithm>

namespace rimes::ai {
namespace {
bool SameSource(const Unit& a, const Unit& b) {
    if (a.source.size() != b.source.size()) return false;
    for (std::size_t i = 0; i < a.source.size(); ++i)
        if (a.source[i].id != b.source[i].id || a.source[i].text != b.source[i].text) return false;
    return true;
}
bool SentenceEnd(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
    for (const std::string ending : {".", "!", "?", "\n", "。", "！", "？"})
        if (text.size() >= ending.size() && text.compare(text.size()-ending.size(), ending.size(), ending) == 0) return true;
    return false;
}
}
void Workspace::SetMode(Mode mode) { if (mode != mode_) { mode_ = mode; Reset(); } }
void Workspace::Reset() { units_.clear(); }
void Workspace::Sync(const std::vector<buffer::Block>& blocks) {
    if (mode_ == Mode::Default) { Reset(); return; }
    std::vector<Unit> desired;
    Unit current;
    for (const auto& block : blocks) {
        current.source.push_back(block);
        current.text += block.text;
        if (mode_ == Mode::Translate && SentenceEnd(block.text)) {
            desired.push_back(std::move(current));
            current = Unit{};
        }
    }
    if (!current.source.empty()) desired.push_back(std::move(current));
    for (auto& unit : desired) {
        const auto old = std::find_if(units_.begin(), units_.end(), [&](const Unit& previous) { return SameSource(previous, unit); });
        if (old != units_.end()) unit = *old;
        else unit.id = ++serial_;
    }
    units_ = std::move(desired);
}
Unit* Workspace::Find(std::uint64_t id) {
    const auto it = std::find_if(units_.begin(), units_.end(), [id](const Unit& unit) { return unit.id == id; });
    return it == units_.end() ? nullptr : &*it;
}
const Unit* Workspace::NextPending() const {
    for (const auto& unit : units_) if (unit.state == State::Pending) return &unit;
    return nullptr;
}
bool Workspace::Complete(std::uint64_t id, std::string result, std::string error) {
    auto* unit = Find(id);
    if (!unit || unit->state != State::Working) return false;
    std::size_t size=result.size();
    for (const auto& other : units_) if(other.id!=id) size+=other.result.size();
    if(size>256*1024) { result.clear(); error="待发送结果过多，请先发送或清理"; }
    unit->result = std::move(result);
    unit->error = std::move(error);
    unit->state = unit->error.empty() && !unit->result.empty() ? State::Ready : State::Error;
    return true;
}
void Workspace::CancelWorking() {
    for (auto& unit : units_) if (unit.state == State::Working) unit.state = State::Pending;
}
void Workspace::Retry() { for (auto& unit : units_) if (unit.state == State::Error) { unit.state = State::Pending; unit.error.clear(); } }
bool Workspace::Consume(std::uint64_t id) {
    const auto size = units_.size();
    units_.erase(std::remove_if(units_.begin(), units_.end(), [id](const Unit& unit) { return unit.id == id; }), units_.end());
    return units_.size() != size;
}
std::string Workspace::ReadyText() const {
    std::string text;
    for (const auto& unit : units_) { if (unit.state != State::Ready) return {}; text += unit.result; }
    return text;
}
std::string ModeTag(Mode mode) { return mode == Mode::Generate ? "generate" : mode == Mode::Translate ? "translate" : "default"; }
bool ParseMode(const std::string& value, Mode* mode) {
    if (value == "default") *mode = Mode::Default;
    else if (value == "generate") *mode = Mode::Generate;
    else if (value == "translate") *mode = Mode::Translate;
    else return false;
    return true;
}
} // namespace rimes::ai
