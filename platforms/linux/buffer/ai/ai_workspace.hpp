#pragma once
#include "buffer_model.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace rimes::ai {
enum class Mode { Default, Generate, Translate };
enum class State { Pending, Working, Ready, Error };
struct Unit {
    std::uint64_t id = 0;
    std::vector<buffer::Block> source;
    std::string text;
    std::string result;
    std::string error;
    State state = State::Pending;
};
// Source identities are retained until successful delivery. Stable sentence
// prefixes survive edits to the tail; stale replies cannot match a new unit.
class Workspace {
public:
    Mode mode() const { return mode_; }
    const std::vector<Unit>& units() const { return units_; }
    void SetMode(Mode mode);
    void Reset();
    void Sync(const std::vector<buffer::Block>& blocks);
    Unit* Find(std::uint64_t id);
    const Unit* NextPending() const;
    bool Complete(std::uint64_t id, std::string result, std::string error);
    void CancelWorking();
    void Retry();
    bool Consume(std::uint64_t id);
    std::string ReadyText() const;
private:
    Mode mode_ = Mode::Default;
    std::uint64_t serial_ = 0;
    std::vector<Unit> units_;
};
std::string ModeTag(Mode mode);
bool ParseMode(const std::string& value, Mode* mode);
} // namespace rimes::ai
