#include "local_runtime.hpp"
#include "ai_suggestion.hpp"
#include <json/json.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
using Clock=std::chrono::steady_clock;
static double Ms(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
static long Memory(pid_t pid) { std::ifstream file("/proc/"+std::to_string(pid)+"/status"); std::string line; long peak=0; while(std::getline(file,line)) if(line.rfind("VmHWM:",0)==0) { std::istringstream stream(line.substr(6)); stream>>peak; } return peak; }
int main(int argc,char** argv) {
    using namespace rimes::ai;
    Config config; if(argc>1) config.threads=std::stoi(argv[1]); config.idle_seconds=1;
    LocalRuntime runtime; std::atomic<bool> cancelled{false}, polling{true}; std::atomic<long> peak{0};
    Json::Value report; report["threads"]=config.threads; report["context"]=config.context_size; report["max_tokens"]=config.max_tokens; report["model_bytes"]=Json::UInt64(std::filesystem::file_size(config.model_file)); report["runtime_bytes"]=Json::UInt64(std::filesystem::file_size(config.runtime_file));
    runtime.SetActive(config,false); std::this_thread::sleep_for(std::chrono::milliseconds(100)); report["default_no_process"]=runtime.ProcessId()==0;
    const auto cold=Clock::now(); runtime.SetActive(config,true); Config resolved=config; std::string error;
    std::thread sampler([&] { while(polling.load()) { peak.store(std::max(peak.load(),Memory(runtime.ProcessId()))); std::this_thread::sleep_for(std::chrono::milliseconds(10)); } });
    if(!runtime.Resolve(&resolved,cancelled,&error)) { polling=false; sampler.join(); std::cerr<<error<<"\n"; return 1; }
    report["cold_ready_ms"]=Ms(cold); const auto pid=runtime.ProcessId();
    // Actual inference first request, followed by warmed requests using the same system prompt.
    for(const std::string sentence : {"我今天很开心。","我今天很开心。","今天天气气很好。","我明天去学校。","I am going to the"}) {
        Json::Value row; row["source"]=sentence; row["pause_ms"]=SentenceEnd(sentence) ? 0 : config.debounce_ms;
        const auto start=Clock::now(); double first_delta=-1,first_visible=-1;
        const auto reply=Request(resolved,sentence,Purpose::Suggest,cancelled,[&](const std::string& text) { if(first_delta<0) first_delta=Ms(start); if(first_visible<0 && !SuggestionPreview(text,sentence).empty()) first_visible=Ms(start); });
        row["first_content_delta_ms"]=first_delta; row["first_visible_ms"]=first_visible; row["complete_ms"]=Ms(start); row["response"]=reply.text; row["error"]=reply.error;
        rimes::buffer::BufferModel model; model.activate_capture("benchmark"); model.append(sentence); Suggestion suggestion; suggestion.Sync(model,"benchmark"); row["valid_suggestion"]=reply.error.empty() && suggestion.Finish(suggestion.revision,reply.text); row["kind"]=suggestion.kind; row["accepted_text"]=suggestion.text;
        report["samples"].append(row);
    }
    auto translation=Request(resolved,"我明天去学校。",Purpose::Translate,cancelled); report["translation"]=translation.text; report["translation_error"]=translation.error;
    runtime.SetActive(config,false); std::this_thread::sleep_for(std::chrono::milliseconds(200)); report["retained_in_cache"]=runtime.ProcessId()==pid;
    runtime.SetActive(config,true); report["shared_pid"]=runtime.ProcessId()==pid;
    const auto idle=Clock::now(); runtime.SetActive(config,false);
    while(runtime.ProcessId()!=0 && Ms(idle)<4000) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    report["idle_unload_ms"]=Ms(idle); report["idle_unloaded"]=runtime.ProcessId()==0;
    polling=false; sampler.join(); report["server_peak_rss_kib"]=Json::Int64(peak.load());
    std::cout<<report<<"\n";
    return 0;
}
