#pragma once
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace rimes::ai {
enum class Channel { ManagedLocal, LocalApi, CloudApi };
enum class Purpose { Generate, Translate, Suggest };
class LocalRuntime;
using Preview = std::function<void(std::string)>;
struct Config {
    Channel channel = Channel::ManagedLocal;
    bool owned_local = false;
    std::string model_file;
    std::string runtime_file;
    int idle_seconds = 60;
    int threads = 4;
    int context_size = 1024;
    int max_tokens = 96;
    int debounce_ms = 250;
    std::string accept_key = "Tab";
    Config();
    std::string endpoint;
    std::string model;
    std::string api_key;
    std::string instruction = "Respond to the user's request. Return only the requested answer.";
    std::string target_language = "en";
    int timeout_seconds = 60;
};
struct Reply { std::string text; std::string error; bool cancelled = false; };
std::filesystem::path ConfigPath();
bool LoadConfig(const std::filesystem::path& path, Config* config, std::string* error);
bool SaveConfig(const std::filesystem::path& path, const Config& config, std::string* error);
bool ValidateConfig(const Config& config, std::string* error);
Reply Request(const Config& config, const std::string& source, Purpose purpose, const std::atomic<bool>& cancelled, Preview preview = {});
inline Reply Request(const Config& c, const std::string& s, bool t, const std::atomic<bool>& x) { return Request(c,s,t ? Purpose::Translate : Purpose::Generate,x); }

// One cancellable network thread per addon, never on the input/event thread.
class Worker {
public:
    using Completion = std::function<void(Reply)>;
    Worker();
    ~Worker();
    void Submit(Config config, std::string source, bool translation, Completion completion);
    void Submit(Config config, std::string source, Purpose purpose, Completion completion, Preview preview = {});
    void SetModelActive(const Config& config, bool active);
    std::string ModelStatus() const;
    void Cancel();
private:
    struct Task { Config config; std::string source; Purpose purpose; Completion completion; Preview preview; std::shared_ptr<std::atomic<bool>> cancelled; };
    void Run();
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Task> pending_;
    std::shared_ptr<std::atomic<bool>> active_;
    bool stopping_ = false;
    std::thread thread_;
    std::unique_ptr<LocalRuntime> runtime_;
};
} // namespace rimes::ai
