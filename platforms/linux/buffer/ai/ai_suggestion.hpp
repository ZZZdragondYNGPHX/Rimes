#pragma once
#include "buffer_model.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace rimes::ai {
std::string SuggestionPrompt();
bool SentenceEnd(const std::string& text);
std::string SuggestionPreview(const std::string& json,const std::string& source = {});
struct Suggestion {
    std::uint64_t revision=0;
    std::vector<buffer::Block> source;
    int insertion=0;
    std::string target, context, kind, text, preview, error;
    std::size_t context_offset=0, begin=0, end=0;
    bool working=false, complete=false, attempted=false;
    bool Sync(const buffer::BufferModel& model,const std::string& token);
    void Invalidate();
    bool Matches(const buffer::BufferModel& model,const std::string& token) const;
    bool Finish(std::uint64_t frozen_revision,const std::string& json);
};
}
