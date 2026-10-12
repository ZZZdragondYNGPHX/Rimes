#include "ai_provider.hpp"
#include "buffer_protocol.hpp"
#include <json/json.h>
#include <curl/curl.h>
#include <fcitx-utils/testing.h>
#include <fcitx-utils/event.h>
#include <fcitx/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/instance.h>
#include <testfrontend_public.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void Check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int control_fd=-1;
struct Cleanup { std::filesystem::path root; ~Cleanup() { if(control_fd>=0) close(control_fd); std::error_code ec; std::filesystem::remove_all(root,ec); } };
void Drain() { char bytes[16384]; if(control_fd>=0) while(recv(control_fd,bytes,sizeof(bytes),MSG_DONTWAIT)>0) {} }
void Command(const std::string& socket,const std::string& command) {
    Drain();
    if(control_fd<0) {
        control_fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        Check(control_fd>=0,"socket"); sockaddr_un address{}; address.sun_family=AF_UNIX;
        std::strncpy(address.sun_path,socket.c_str(),sizeof(address.sun_path)-1);
        Check(connect(control_fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"connect Buffer");
    }
    const int fd=control_fd;
    std::string frame; Check(rimes::buffer::EncodeFrame(command,&frame),"encode command");
    const auto written=send(fd,frame.data(),frame.size(),MSG_NOSIGNAL);
    Check(written==static_cast<ssize_t>(frame.size()),"send command");
}
size_t Collect(char* data,size_t size,size_t count,void* opaque) { static_cast<std::string*>(opaque)->append(data,size*count); return size*count; }
int RequestCount(const std::string& endpoint) {
    auto url=endpoint.substr(0,endpoint.find("/v1/"))+"/stats"; std::string body; CURL* c=curl_easy_init(); Check(c!=nullptr,"stats curl"); curl_easy_setopt(c,CURLOPT_URL,url.c_str()); curl_easy_setopt(c,CURLOPT_PROXY,""); curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,Collect); curl_easy_setopt(c,CURLOPT_WRITEDATA,&body); const auto result=curl_easy_perform(c); curl_easy_cleanup(c); Check(result==CURLE_OK,"request stats"); Json::Value stats; Json::CharReaderBuilder builder; std::istringstream in(body); std::string error; Check(Json::parseFromStream(builder,in,&stats,&error),"stats JSON"); return stats["requests"].asInt();
}
Json::Value Snapshot(const std::filesystem::path& path) {
    std::ifstream in(path); Json::Value json; Json::CharReaderBuilder reader; std::string error;
    Check(Json::parseFromStream(reader,in,&json,&error),"snapshot parse"); return json;
}
}
int main(int argc,char** argv) {
    if(argc!=4) return 2;
    try {
        char folder[]="/tmp/rimes-ai-fcitx.XXXXXX"; Check(mkdtemp(folder)!=nullptr,"test root"); Cleanup cleanup{folder};
        fcitx::setupTestingEnvironment(argv[1],{"."},{"test-data"});
        const auto root=cleanup.root;
        const auto socket=(root/"buffer.sock").string(); const auto dump=root/"snapshot.json";
        std::filesystem::create_directories(root/"user/log");
        setenv("RIMES_USER_DIR",(root/"user").c_str(),1); setenv("RIMES_LOG_DIR",(root/"user/log").c_str(),1);
        setenv("RIMES_SHARED_DIR",argv[3],1); setenv("RIMES_BUFFER_SOCKET",socket.c_str(),1); setenv("RIMES_BUFFER_DUMP",dump.c_str(),1);
        setenv("RIMES_BUFFER_HEADLESS","1",1); setenv("RIMES_BUFFER_AUTO_CAPTURE","0",1); setenv("RIMES_BUFFER_CLOSE_AFTER_LAST","1",1);
        setenv("XDG_CONFIG_HOME",(root/"config").c_str(),1);
        rimes::ai::Config config; config.channel=rimes::ai::Channel::LocalApi; config.endpoint=argv[2]; config.model="fixture"; config.api_key="local-fixture-key";
        std::string error; Check(rimes::ai::SaveConfig(rimes::ai::ConfigPath(),config,&error),"isolated config");
        char arg0[]="rimes-ai-fcitx-e2e"; char arg1[]="--disable=all"; char arg2[]="--enable=testfrontend,testui,rimes"; char* args[]={arg0,arg1,arg2};
        fcitx::Instance instance(3,args); instance.addonManager().registerDefaultLoader(nullptr);
        fcitx::EventDispatcher dispatcher; dispatcher.attach(&instance.eventLoop());
        fcitx::AddonInstance* frontend=nullptr; fcitx::ICUUID uuid{},other{}; fcitx::InputContext* ic=nullptr;
        int phase=0,commits=0,exit_status=0,request_count=0;
        std::string stable_id;
        auto commit_watch=instance.watchEvent(fcitx::EventType::InputContextCommitString,fcitx::EventWatcherPhase::Default,[&](fcitx::Event& event) {
            const auto& commit=static_cast<fcitx::CommitStringEvent&>(event);
            Check(commit.inputContext()==ic,"result must stay in captured context");
            Check(commit.text()=="fixture-output" || commit.text()=="fixture-translation" || commit.text()=="我们去" || commit.text()=="散步。","result must follow existing delivery route");
            ++commits;
        });
        auto phase_started=std::chrono::steady_clock::now();
        const auto deadline=phase_started+std::chrono::seconds(180);
        std::unique_ptr<fcitx::EventSourceTime> timer;
        auto key=[&](const char* name) { frontend->call<fcitx::ITestFrontend::keyEvent>(uuid,fcitx::Key(name),false); frontend->call<fcitx::ITestFrontend::keyEvent>(uuid,fcitx::Key(name),true); };
        dispatcher.schedule([&] {
            auto& manager=instance.inputMethodManager();
            if(manager.groupCount()==0) { manager.addEmptyGroup("Default"); manager.setGroupOrder({"Default"}); }
            fcitx::InputMethodGroup group("Default"); group.setDefaultLayout("us"); group.inputMethodList().emplace_back(fcitx::InputMethodGroupItem("rimes")); group.setDefaultInputMethod("rimes"); manager.setGroup(group); manager.setCurrentGroup("Default");
            frontend=instance.addonManager().addon("testfrontend",true); Check(frontend!=nullptr,"testfrontend");
            uuid=frontend->call<fcitx::ITestFrontend::createInputContext>("synthetic-ai-host");
            ic=instance.inputContextManager().findByUUID(uuid); Check(ic!=nullptr,"input context");
            ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit});
            ic->focusIn(); instance.setCurrentInputMethod(ic,"rimes",true);
            timer=instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,fcitx::now(CLOCK_MONOTONIC)+50000,0,[&](fcitx::EventSourceTime* source,uint64_t) {
                try {
                    Drain();
                    Check(std::chrono::steady_clock::now()<deadline,"AI integration timeout");
                    auto step=[&] { ++phase; std::cerr<<"phase "<<phase<<"\n"; phase_started=std::chrono::steady_clock::now(); };
                    if(phase==0) {
                        auto* engine=instance.inputMethodEngine(ic); const auto* entry=instance.inputMethodEntry(ic);
                        if(engine && entry && engine->subMode(*entry,*ic)!="Deploying") { key("Control+Shift+B"); Command(socket,R"({"op":"ai_mode","mode":"generate"})"); Command(socket,R"({"op":"paste","text":"synthetic generation source"})"); Command(socket,R"({"op":"ai_generate"})"); step(); }
                    } else {
                        const auto snapshot=Snapshot(dump);
                        if(phase==1 && snapshot["ai_can_send"].asBool()) {
                            Check(commits==0 && snapshot["staged_text"]=="synthetic generation source","generation is manual and preserves source");
                            frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("fixture-output");
                            const auto id=snapshot["ai_result_id"].asString();
                            Command(socket,"{\"op\":\"ai_send\",\"result_id\":"+rimes::buffer::JsonEscape(id)+"}"); step();
                        } else if(phase==2 && commits==1) {
                            Check(snapshot["empty"].asBool(),"successful result retires source");
                            key("Control+Shift+B"); Command(socket,R"({"op":"ai_mode","mode":"translate"})"); Command(socket,R"({"op":"paste","text":"First."})"); step();
                        } else if(phase==3 && snapshot["ai_can_send"].asBool()) {
                            stable_id=snapshot["ai_result_id"].asString();
                            Command(socket,R"({"op":"paste","text":"tail"})"); step();
                        } else if(phase==4 && snapshot["ai_can_copy"].asBool() && snapshot["staged_text"]=="First.tail") {
                            Check(snapshot["ai_result_id"]==stable_id,"tail append preserves translated prefix");
                            Check(commits==1,"translation does not auto-send");
                            frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("fixture-translation");
                            Command(socket,"{\"op\":\"ai_send\",\"result_id\":"+rimes::buffer::JsonEscape(stable_id)+"}"); step();
                        } else if(phase==5 && commits==2) {
                            Check(snapshot["staged_text"]=="tail","sending prefix consumes only prefix source");
                            Command(socket,"{\"op\":\"ai_send\",\"result_id\":"+rimes::buffer::JsonEscape(stable_id)+"}"); step();
                        } else if(phase==6 && std::chrono::steady_clock::now()-phase_started>std::chrono::milliseconds(100)) {
                            Check(commits==2,"stale UI identity cannot send a newer result"); frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("fixture-translation"); key("Return"); step();
                        } else if(phase==7 && commits==3) {
                            config.model="slow"; Check(rimes::ai::SaveConfig(rimes::ai::ConfigPath(),config,&error),"slow config");
                            key("Control+Shift+B"); Command(socket,R"({"op":"ai_reload"})"); Command(socket,R"({"op":"ai_mode","mode":"generate"})"); Command(socket,R"({"op":"paste","text":"cancelled source"})"); Command(socket,R"({"op":"ai_generate"})"); step();
                        } else if(phase==8 && snapshot["ai_busy"].asBool()) {
                            other=frontend->call<fcitx::ITestFrontend::createInputContext>("synthetic-other-field");
                            ic->focusOut(); instance.inputContextManager().findByUUID(other)->focusIn(); step();
                        } else if(phase==9 && std::chrono::steady_clock::now()-phase_started>std::chrono::milliseconds(1300)) {
                            Check(snapshot["ai_result"].asString().empty() && commits==3,"focus drift must reject late result");
                            config.model="fixture"; Check(rimes::ai::SaveConfig(rimes::ai::ConfigPath(),config,&error),"restore fixture");
                            instance.inputContextManager().findByUUID(other)->focusOut(); ic->focusIn(); key("Control+Shift+B");
                            Command(socket,R"({"op":"ai_reload"})"); Command(socket,R"({"op":"ai_generate"})"); step();
                        } else if(phase==10 && snapshot["ai_can_send"].asBool()) {
                            ic->focusOut(); ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Password}); ic->focusIn(); step();
                        } else if(phase==11 && snapshot["secure"].asBool()) {
                            Check(snapshot["ai_result"].asString().empty() && !snapshot["ai_can_send"].asBool() && !snapshot["ai_can_copy"].asBool(),"protected field scrubs results");
                            ic->focusOut(); ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit}); ic->focusIn(); key("Control+Shift+B");
                            Command(socket,R"({"op":"ai_mode","mode":"default"})"); Command(socket,R"({"op":"select_all"})"); Command(socket,R"({"op":"paste","text":"ordinary source"})"); request_count=RequestCount(argv[2]); step();
                        } else if(phase==12 && std::chrono::steady_clock::now()-phase_started>std::chrono::milliseconds(500)) {
                            Check(snapshot["ai_mode"]=="default" && !snapshot["ai_busy"].asBool() && RequestCount(argv[2])==request_count,"ordinary mode has no inference");
                            Check(!frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Tab"),false),"no suggestion Tab passes through");
                            frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Tab"),true);
                            config.model="suggestion"; Check(rimes::ai::SaveConfig(rimes::ai::ConfigPath(),config,&error),"suggestion config"); Command(socket,R"({"op":"ai_reload"})"); Command(socket,R"({"op":"ai_mode","mode":"generate"})"); Command(socket,R"({"op":"select_all"})"); Command(socket,R"({"op":"paste","text":"今天天气气很好。"})"); step();
                        } else if(phase==13 && snapshot["suggestion_ready"].asBool()) {
                            Check(snapshot["suggestion_kind"]=="correction" && snapshot["staged_text"]=="今天天气气很好。","correction preserves source until acceptance");
                            Check(frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Tab"),false),"Tab adopts complete suggestion");
                            Check(frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key(FcitxKey_Tab,fcitx::KeyStates{fcitx::KeyState::Repeat}),false),"accepted physical key repeat consumed");
                            Check(frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Tab"),true),"accepted physical key release consumed"); step();
                        } else if(phase==14) {
                            Check(snapshot["staged_text"]=="今天天气很好。" && commits==3,"Tab changes only correction range once without sending");
                            config.accept_key="Control+semicolon"; Check(rimes::ai::SaveConfig(rimes::ai::ConfigPath(),config,&error),"custom shortcut config"); Command(socket,R"({"op":"ai_reload"})"); Command(socket,R"({"op":"select_all"})"); Command(socket,R"({"op":"paste","text":"我们去"})"); step();
                        } else if(phase==15 && snapshot["suggestion_ready"].asBool()) {
                            Check(!frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Tab"),false),"custom configuration preserves Tab"); frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Tab"),true);
                            Check(frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Control+semicolon"),false),"custom shortcut adopts");
                            Check(frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key(FcitxKey_semicolon,fcitx::KeyStates{fcitx::KeyState::Ctrl}|fcitx::KeyState::Repeat),false),"custom repeat consumed");
                            Check(frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Control+semicolon"),true),"custom release consumed"); step();
                        } else if(phase==16) {
                            Check(snapshot["staged_text"]=="我们去散步。" && commits==3,"continuation inserted once without sending"); frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("我们去"); frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("散步。"); Command(socket,R"({"op":"send_all"})"); step();
                        } else if(phase==17 && commits==5) {
                            config.accept_key=""; Check(rimes::ai::SaveConfig(rimes::ai::ConfigPath(),config,&error),"disabled shortcut config"); key("Control+Shift+B"); Command(socket,R"({"op":"ai_reload"})"); Command(socket,R"({"op":"paste","text":"我们去"})"); step();
                        } else if(phase==18 && snapshot["suggestion_ready"].asBool()) {
                            Check(!frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Tab"),false),"disabled shortcut passes Tab"); frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Tab"),true);
                            Check(!frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Control+semicolon"),false),"disabled shortcut passes custom key"); frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid,fcitx::Key("Control+semicolon"),true);
                            const auto obsolete=snapshot["suggestion_id"].asString(); config.accept_key="Tab"; Check(rimes::ai::SaveConfig(rimes::ai::ConfigPath(),config,&error),"restore Tab"); Command(socket,R"({"op":"ai_reload"})"); Command(socket,R"({"op":"select_all"})"); Command(socket,R"({"op":"paste","text":"我们去"})"); Command(socket,"{\"op\":\"ai_accept\",\"result_id\":"+rimes::buffer::JsonEscape(obsolete)+"}"); step();
                        } else if(phase==19 && snapshot["suggestion_ready"].asBool()) {
                            Check(snapshot["staged_text"]=="我们去","source/config revision discards old accept button");
                            key("n"); request_count=RequestCount(argv[2]); step();
                        } else if(phase==20 && std::chrono::steady_clock::now()-phase_started>std::chrono::milliseconds(350)) {
                            Check(!snapshot["preedit"].asString().empty() && !snapshot["suggestion_ready"].asBool() && RequestCount(argv[2])==request_count,"Rime preedit stops inference and adoption"); key("Tab"); key("Escape");
                            config.model="slow-suggestion"; Check(rimes::ai::SaveConfig(rimes::ai::ConfigPath(),config,&error),"slow suggestion config"); Command(socket,R"({"op":"ai_reload"})"); Command(socket,R"({"op":"select_all"})"); Command(socket,R"({"op":"paste","text":"我们去"})"); step();
                        } else if(phase==21 && snapshot["ai_busy"].asBool()) {
                            Check(snapshot["staged_text"]=="我们去","Tab during preedit must not adopt old continuation"); ic->focusOut(); instance.inputContextManager().findByUUID(other)->focusIn(); step();
                        } else if(phase==22 && std::chrono::steady_clock::now()-phase_started>std::chrono::milliseconds(1300)) {
                            Check(snapshot["suggestion_text"].asString().empty() && !snapshot["suggestion_ready"].asBool() && commits==5,"focus drift rejects late automatic suggestion");
                            config.model="suggestion"; Check(rimes::ai::SaveConfig(rimes::ai::ConfigPath(),config,&error),"restore suggestion config"); instance.inputContextManager().findByUUID(other)->focusOut(); ic->focusIn(); key("Control+Shift+B"); Command(socket,R"({"op":"ai_reload"})"); step();
                        } else if(phase==23 && snapshot["suggestion_ready"].asBool()) {
                            ic->focusOut(); ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Password}); ic->focusIn(); step();
                        } else if(phase==24 && snapshot["secure"].asBool()) {
                            Check(snapshot["suggestion_text"].asString().empty() && !snapshot["suggestion_ready"].asBool(),"password protection scrubs automatic suggestion");
                            ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Sensitive}); step();
                        } else if(phase==25 && snapshot["secure"].asBool()) {
                            Check(snapshot["suggestion_text"].asString().empty() && !snapshot["suggestion_ready"].asBool(),"sensitive field protects suggestions");
                            std::cout<<"ok: ordinary no inference; manual AI/translation; correction and continuation; Tab/custom/disabled/repeat/release; preedit priority; commitString; late focus and password rejection\n";
                            frontend->call<fcitx::ITestFrontend::destroyInputContext>(other); frontend->call<fcitx::ITestFrontend::destroyInputContext>(uuid); instance.exit(); return true;
                        }
                    }
                    source->setTime(fcitx::now(CLOCK_MONOTONIC)+50000); source->setOneShot();
                } catch(const std::exception& error) { std::cerr<<"FAIL phase "<<phase<<": "<<error.what()<<'\n'; exit_status=1; instance.exit(); }
                return true;
            });
        });
        instance.exec(); return exit_status;
    } catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
