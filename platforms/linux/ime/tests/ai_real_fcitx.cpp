#include "ai_provider.hpp"
#include "buffer_protocol.hpp"
#include <json/json.h>
#include <fcitx-utils/testing.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>
#include <testfrontend_public.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
using Clock=std::chrono::steady_clock;
namespace {
void Check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
double Ms(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
pid_t ServerPid() {
    for(const auto& entry:std::filesystem::directory_iterator("/proc/self/task")) {
        std::ifstream children(entry.path()/"children"); pid_t pid;
        while(children>>pid) { std::ifstream command("/proc/"+std::to_string(pid)+"/comm"); std::string name; std::getline(command,name); if(name=="llama-server") return pid; }
    }
    return 0;
}
Json::Value Snapshot(const std::filesystem::path& path) { std::ifstream file(path); Json::Value json; file>>json; return json; }
}
int main(int argc,char** argv) {
    if(argc!=4) return 2;
    rimes::ai::Config config; config.idle_seconds=1;
    char folder[]="/tmp/rimes-real-fcitx.XXXXXX"; if(!mkdtemp(folder)) return 2;
    const auto root=std::filesystem::path(folder); const auto socket_path=(root/"buffer.sock").string(); const auto dump=root/"snapshot.json";
    int control=-1,exit_status=0;
    try {
        fcitx::setupTestingEnvironment(argv[1],{"."},{"test-data"});
        setenv("XDG_CONFIG_HOME",(root/"config").c_str(),1);
        setenv("RIMES_SHARED_DIR",argv[2],1); setenv("RIMES_USER_DIR",(root/"user").c_str(),1); setenv("RIMES_LOG_DIR",(root/"user/log").c_str(),1);
        std::filesystem::create_directories(root/"user/log");
        setenv("RIMES_BUFFER_HEADLESS","1",1); setenv("RIMES_BUFFER_SOCKET",socket_path.c_str(),1); setenv("RIMES_BUFFER_DUMP",dump.c_str(),1); setenv("RIMES_BUFFER_AUTO_CAPTURE","0",1); setenv("RIMES_BUFFER_CLOSE_AFTER_LAST","1",1);
        const auto host=(std::filesystem::path(argv[1])/"rimes-model-host").string(); setenv("RIMES_MODEL_HOST",host.c_str(),1);
        std::string error; Check(rimes::ai::SaveConfig(rimes::ai::ConfigPath(),config,&error),"managed config");
        char arg0[]="rimes-real-fcitx"; char arg1[]="--disable=all"; char arg2[]="--enable=testfrontend,testui,rimes"; char* args[]={arg0,arg1,arg2};
        fcitx::Instance instance(3,args); instance.addonManager().registerDefaultLoader(nullptr);
        fcitx::EventDispatcher dispatcher; dispatcher.attach(&instance.eventLoop());
        fcitx::AddonInstance* frontend=nullptr; fcitx::InputContext* ic=nullptr; fcitx::ICUUID uuid{};
        Json::Value report; int phase=0,commits=0; double first_visible=-1,warm_trigger=-1,warm_visible=-1; pid_t server=0;
        auto started=Clock::now(), request_start=started, cache_start=started;
        auto key=[&](const char* name) { frontend->call<fcitx::ITestFrontend::keyEvent>(uuid,fcitx::Key(name),false); frontend->call<fcitx::ITestFrontend::keyEvent>(uuid,fcitx::Key(name),true); };
        auto command=[&](const std::string& json) {
            if(control<0) { control=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0); Check(control>=0,"control socket"); sockaddr_un address{}; address.sun_family=AF_UNIX; std::strncpy(address.sun_path,socket_path.c_str(),sizeof(address.sun_path)-1); Check(connect(control,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"control connect"); }
            std::string frame; Check(rimes::buffer::EncodeFrame(json,&frame),"frame"); Check(send(control,frame.data(),frame.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(frame.size()),"control send");
        };
        auto watch=instance.watchEvent(fcitx::EventType::InputContextCommitString,fcitx::EventWatcherPhase::Default,[&](fcitx::Event& event) { const auto& commit=static_cast<fcitx::CommitStringEvent&>(event); Check(commit.inputContext()==ic && commit.text()=="今天天气很好。","real result commits only to frozen host"); ++commits; });
        std::unique_ptr<fcitx::EventSourceTime> timer;
        dispatcher.schedule([&] {
            auto& manager=instance.inputMethodManager(); if(manager.groupCount()==0) { manager.addEmptyGroup("Default"); manager.setGroupOrder({"Default"}); }
            fcitx::InputMethodGroup group("Default"); group.setDefaultLayout("us"); group.inputMethodList().emplace_back(fcitx::InputMethodGroupItem("rimes")); group.setDefaultInputMethod("rimes"); manager.setGroup(group); manager.setCurrentGroup("Default");
            frontend=instance.addonManager().addon("testfrontend",true); uuid=frontend->call<fcitx::ITestFrontend::createInputContext>("synthetic-real-model-host"); ic=instance.inputContextManager().findByUUID(uuid); ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit}); ic->focusIn(); instance.setCurrentInputMethod(ic,"rimes",true);
            timer=instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,fcitx::now(CLOCK_MONOTONIC)+10000,1000,[&](fcitx::EventSourceTime* source,uint64_t) {
                try {
                    char drain[16384]; if(control>=0) while(recv(control,drain,sizeof(drain),MSG_DONTWAIT)>0) {}
                    Check(Ms(started)<120000,"real Fcitx timeout");
                    auto snap=Snapshot(dump);
                    if(phase==0) { auto* engine=instance.inputMethodEngine(ic); const auto* entry=instance.inputMethodEntry(ic); if(engine && entry && engine->subMode(*entry,*ic)!="Deploying") { Check(ServerPid()==0,"ordinary mode must not spawn model"); report["ordinary_no_process"]=true; key("Control+Shift+B"); request_start=Clock::now(); command(R"({"op":"ai_mode","mode":"generate"})"); ++phase; } }
                    else if(phase==1 && snap["ai_mode"]=="generate") {
                        const auto typing=Clock::now(); key("n"); report["rime_key_during_preload_ms"]=Ms(typing); Check(report["rime_key_during_preload_ms"].asDouble()<100,"typing waits for model"); key("Escape"); ++phase;
                    } else if(phase==2 && snap["model_status"]=="已预热") {
                        report["preload_ready_snapshot_ms"]=Ms(request_start); server=ServerPid(); Check(server>0,"managed child exists");
                        request_start=Clock::now(); command(R"({"op":"paste","text":"今天天气气很好。"})"); ++phase;
                    } else if(phase==3) {
                        if(first_visible<0 && !snap["suggestion_text"].asString().empty()) first_visible=Ms(request_start);
                        if(snap["suggestion_ready"].asBool()) { report["first_suggestion_snapshot_ms"]=first_visible; report["complete_suggestion_snapshot_ms"]=Ms(request_start); report["sentence_pause_ms"]=0; Check(snap["suggestion_kind"]=="correction" && snap["staged_text"]=="今天天气气很好。","real correction preserves source"); key("Tab"); ++phase; }
                    } else if(phase==4) {
                        Check(snap["staged_text"]=="今天天气很好。" && commits==0,"real Tab corrects Buffer only"); report["real_correction_accepted"]=true;
                        frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("今天天气很好。"); command(R"({"op":"send_all"})"); ++phase;
                    } else if(phase==5 && commits==1) {
                        Check(ServerPid()==server,"one shared model survives delivery"); report["commitString_verified"]=true;
                        key("Control+Shift+B"); request_start=Clock::now(); command(R"({"op":"paste","text":"I am going to the"})"); ++phase;
                    } else if(phase==6) {
                        if(warm_trigger<0 && snap["ai_busy"].asBool()) warm_trigger=Ms(request_start);
                        if(warm_visible<0 && !snap["suggestion_text"].asString().empty()) warm_visible=Ms(request_start);
                        if(snap["suggestion_ready"].asBool()) {
                            Check(snap["suggestion_kind"]=="continuation" && snap["staged_text"]=="I am going to the","real continuation retains source");
                            report["warm_pause_observed_ms"]=warm_trigger; report["warm_first_snapshot_from_trigger_ms"]=warm_visible-warm_trigger; report["warm_complete_snapshot_from_trigger_ms"]=Ms(request_start)-warm_trigger;
                            report["shared_generation_process"]=ServerPid()==server;
                            command(R"({"op":"ai_mode","mode":"default"})"); cache_start=Clock::now(); ++phase;
                        }
                    } else if(phase==7 && Ms(cache_start)>200) {
                        report["cached_after_exit"]=ServerPid()==server; Check(report["cached_after_exit"].asBool(),"cache retained"); ++phase;
                    } else if(phase==8 && ServerPid()==0) {
                        report["idle_process_gone_ms"]=Ms(cache_start); report["threads"]=config.threads; Check(Ms(cache_start)<2500,"idle child unloaded"); std::ofstream(argv[3])<<report<<'\n'; std::cout<<"ok: real GGUF through isolated Fcitx, async preload, correction Tab, commitString, warm continuation and idle unload\n"; frontend->call<fcitx::ITestFrontend::destroyInputContext>(uuid); instance.exit(); return true;
                    }
                    source->setTime(fcitx::now(CLOCK_MONOTONIC)+10000); source->setOneShot();
                } catch(const std::exception& e) { std::cerr<<"FAIL real phase "<<phase<<": "<<e.what()<<'\n'; exit_status=1; instance.exit(); }
                return true;
            });
        });
        instance.exec();
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; exit_status=1; }
    if(control>=0) close(control);
    std::error_code ignored; std::filesystem::remove_all(root,ignored);
    return exit_status;
}
