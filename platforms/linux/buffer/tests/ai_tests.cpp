#include "ai_workspace.hpp"
#include "ai_provider.hpp"
#include "ai_suggestion.hpp"
#include "ai_shortcut.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool condition, const char* message) { if(!condition) throw std::runtime_error(message); }
void WorkspaceTests() {
    using namespace rimes::ai;
    Workspace work; work.SetMode(Mode::Translate);
    std::vector<rimes::buffer::Block> blocks={{"a","你好。"},{"b","hello"}};
    work.Sync(blocks); Check(work.units().size()==2,"sentence units");
    const auto stable=work.units()[0].id, tail=work.units()[1].id;
    work.Find(stable)->state=State::Working; Check(work.Complete(stable,"Hello.",{}),"stable translation");
    work.Find(tail)->state=State::Working;
    blocks[1].text="hello world"; work.Sync(blocks);
    Check(!work.Complete(tail,"stale",{}),"tail edit must reject stale result");
    Check(work.units()[0].id==stable && work.units()[0].result=="Hello.","tail edit must preserve stable prefix");
    Check(work.ReadyText().empty(),"copy waits for complete workspace");
    work.Consume(stable); blocks.erase(blocks.begin()); work.Sync(blocks);
    Check(work.units().size()==1 && work.units()[0].text=="hello world","sent source cannot be translated again");
    work.SetMode(Mode::Generate); work.Sync(blocks);
    const auto generated=work.units()[0].id; work.Find(generated)->state=State::Working;
    blocks[0].id="replacement"; work.Sync(blocks);
    Check(!work.Complete(generated,"old identity",{}),"same text with different provenance is stale");
    work.Find(work.units()[0].id)->state=State::Working; work.CancelWorking();
    Check(work.NextPending()!=nullptr,"cancel makes unfinished request resumable");
    work.Reset(); Check(!work.Complete(generated,"late",{}),"reset rejects late completion");
}
void SuggestionTests() {
    using namespace rimes::ai;
    rimes::buffer::BufferModel model; model.activate_capture("target"); model.append("今天天气"); model.append("气很好。"); model.append("后文"); model.set_insertion_point(2);
    Suggestion s; s.Sync(model,"target");
    Check(s.Finish(s.revision,R"({"kind":"correction","original":"今天天气气很好。","text":"今天天气很好。"})"),"small correction validates");
    Check(s.begin==12 && s.end==15 && s.text.empty(),"trim correction to repeated Chinese character");
    const auto original=model.staged_text();
    model.set_preedit("ni"); Check(!model.replace_range(s.source,s.insertion,s.begin,s.end,s.text) && model.staged_text()==original,"preedit blocks adoption and preserves source"); model.set_preedit("");
    Check(model.replace_range(s.source,s.insertion,s.begin,s.end,s.text) && model.staged_text()=="今天天气很好。后文","range edit retains suffix");
    Check(!model.replace_range(s.source,s.insertion,s.begin,s.end,s.text),"same suggestion cannot apply twice");
    s.Sync(model,"target"); Check(s.Finish(s.revision,R"({"kind":"continuation","original":"","text":"继续"})"),"continuation validates");
    Check(model.replace_range(s.source,s.insertion,s.begin,s.end,s.text) && model.staged_text()=="今天天气很好。继续后文","continuation uses frozen cursor before suffix");
    s.Sync(model,"target"); model.set_insertion_point(0); Check(!s.Matches(model,"target"),"cursor drift invalidates suggestion");
    const auto frozen=s.revision; s.Sync(model,"target"); Check(!s.Finish(frozen,R"({"text":"旧结果"})"),"old revision completion rejected without changing source");
    s.Sync(model,"target"); Check(!s.Finish(s.revision,R"({"kind":"correction","original":"不存在","text":"替换"})"),"invented source range rejected");
    Check(!s.Finish(s.revision,R"({"kind":"correction","original":"今天天气很好。","text":"这是一段完全不同的内容。"})"),"paragraph rewrite rejected");
    Check(SentenceEnd("完成。") && !SentenceEnd("未完成"),"sentence trigger");
    Check(SuggestionPreview("{\"text\":\"你好")=="你好","stream preview decodes text only");
    Check(!s.Finish(s.revision,"{\"text\":\"未完成"),"incomplete preview cannot be adopted");
    std::string error;
    Check(ValidateShortcut("Tab",&error) && ValidateShortcut("Control+semicolon",&error) && ValidateShortcut("",&error),"default custom and disabled shortcuts");
    Check(!ValidateShortcut("1",&error) && !ValidateShortcut("Control+Shift+b",&error) && !ValidateShortcut("Control+space",&error),"recognizable conflicts rejected");
}
void ConfigTests() {
    using namespace rimes::ai;
    char folder[]="/tmp/rimes-ai-config.XXXXXX";
    Check(mkdtemp(folder)!=nullptr,"temporary config root");
    const auto root=std::filesystem::path(folder);
    try {
        Config config; config.channel=Channel::LocalApi; config.endpoint="http://127.0.0.1:12345/v1/chat/completions"; config.model="fixture"; config.api_key="local-fixture-key";
        std::string error;
        const auto file=root/"rimes/ai.json";
        Check(SaveConfig(file,config,&error),"save private config");
        struct stat info{}; Check(stat(file.c_str(),&info)==0 && (info.st_mode&0777)==0600,"private file permissions");
        Check(stat(file.parent_path().c_str(),&info)==0 && (info.st_mode&0777)==0700,"private directory permissions");
        Config loaded; Check(LoadConfig(file,&loaded,&error) && loaded.api_key==config.api_key,"private config roundtrip");
        const auto before=std::filesystem::file_size(file); config.accept_key="Control+space"; Check(!SaveConfig(file,config,&error),"conflict rejects save"); Check(std::filesystem::file_size(file)==before && LoadConfig(file,&loaded,&error) && loaded.accept_key=="Tab","conflict preserves old config"); config.accept_key="Tab";
        chmod(file.c_str(),0644); Check(!LoadConfig(file,&loaded,&error),"world-readable secret rejected"); chmod(file.c_str(),0600);
        std::filesystem::create_symlink(file,root/"link"); Check(!LoadConfig(root/"link",&loaded,&error),"symlink config rejected");
        config.endpoint="http://example.invalid/v1/chat/completions"; Check(!ValidateConfig(config,&error),"remote plain HTTP rejected");
        config.endpoint="https://example.invalid/v1/chat/completions"; config.api_key="bad\r\nheader"; Check(!ValidateConfig(config,&error),"header injection rejected");
        std::ofstream(file)<<"{\"version\":{},\"endpoint\":42}"; Check(!LoadConfig(file,&loaded,&error),"malformed config fails without exception");
    } catch (...) { std::filesystem::remove_all(root); throw; }
    std::filesystem::remove_all(root);
}
void NetworkTests(const std::string& endpoint) {
    using namespace rimes::ai;
    Config config; config.channel=Channel::LocalApi; config.endpoint=endpoint; config.model="fixture"; config.api_key="local-fixture-key";
    std::atomic<bool> cancelled{false};
    auto reply=Request(config,"synthetic source",false,cancelled);
    Check(reply.text=="fixture-output" && reply.error.empty(),"generation response");
    reply=Request(config,"synthetic source",true,cancelled); Check(reply.text=="fixture-translation","translation prompt");
    for(const char* model : {"bad-json","wrong-shape","status-401","oversize"}) {
        config.model=model; reply=Request(config,"synthetic source",false,cancelled);
        Check(reply.text.empty() && !reply.error.empty(),"failed response cannot expose a deliverable result");
        Check(reply.error.find("local-fixture-key")==std::string::npos,"service errors do not expose secrets");
    }
    config.model="stream"; int previews=0; reply=Request(config,"synthetic source",Purpose::Generate,cancelled,[&](const std::string&) { ++previews; }); Check(reply.text=="fixture-output" && previews>=2,"SSE complete content and previews");
    for(const char* model:{"truncated-stream","broken-stream"}) { config.model=model; reply=Request(config,"synthetic source",Purpose::Generate,cancelled); Check(reply.text.empty() && !reply.error.empty(),"incomplete SSE cannot be delivered"); }
    config.model="array-content"; reply=Request(config,"synthetic source",false,cancelled); Check(reply.text=="fixture-array","array content response");
    config.model="slow";
    const auto start=std::chrono::steady_clock::now();
    std::thread request([&] { reply=Request(config,"synthetic source",false,cancelled); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); cancelled.store(true); request.join();
    Check(reply.cancelled && reply.text.empty(),"cancel cannot publish a result");
    Check(std::chrono::steady_clock::now()-start<std::chrono::seconds(3),"cancel is bounded");
}
}
int main(int argc,char** argv) {
    try { WorkspaceTests(); SuggestionTests(); ConfigTests(); if(argc==2) NetworkTests(argv[1]); std::cout<<"ok: AI workspace, private config and requested local transport checks\n"; return 0; }
    catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
