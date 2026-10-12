#pragma once
#include "ai_provider.hpp"
#include <sys/types.h>
namespace rimes::ai {
// Controller thread is created only on explicit advanced-mode entry.
// The inference child is shared by generation, suggestions and translation.
class LocalRuntime {
public:
    ~LocalRuntime();
    void SetActive(const Config& config, bool active);
    bool Resolve(Config* config, const std::atomic<bool>& cancelled, std::string* error);
    std::string Status() const;
    pid_t ProcessId() const;
private:
    void Run();
    bool Start(const Config& config);
    void Stop();
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::thread thread_;
    Config config_;
    bool active_=false, stopping_=false, ready_=false;
    std::uint64_t revision_=0;
    std::string status_="未加载", endpoint_, key_, key_file_;
    pid_t child_=0;
    std::chrono::steady_clock::time_point idle_;
};
}
