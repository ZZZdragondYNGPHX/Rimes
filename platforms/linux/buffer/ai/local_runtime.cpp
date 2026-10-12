#include "local_runtime.hpp"
#include <curl/curl.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fstream>
#include <iomanip>
#include <sstream>
extern char** environ;
namespace rimes::ai {
namespace {
bool Same(const Config& a,const Config& b) { return a.channel==b.channel && a.model_file==b.model_file && a.runtime_file==b.runtime_file && a.threads==b.threads && a.context_size==b.context_size && a.max_tokens==b.max_tokens; }
size_t Discard(char*,size_t size,size_t count,void*) { return size*count; }
bool Healthy(const std::string& endpoint) {
    CURL* c=curl_easy_init(); if(!c) return false;
    const auto health=endpoint.substr(0,endpoint.find("/v1/"))+"/health";
    curl_easy_setopt(c,CURLOPT_URL,health.c_str()); curl_easy_setopt(c,CURLOPT_PROXY,""); curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L); curl_easy_setopt(c,CURLOPT_TIMEOUT_MS,300L); curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,Discard);
    const auto result=curl_easy_perform(c); long http=0; curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&http); curl_easy_cleanup(c); return result==CURLE_OK && http==200;
}
}
LocalRuntime::~LocalRuntime() { { std::lock_guard<std::mutex> lock(mutex_); stopping_=true; } changed_.notify_all(); if(thread_.joinable()) thread_.join(); }
void LocalRuntime::SetActive(const Config& config,bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    active=active && config.channel==Channel::ManagedLocal;
    if(!Same(config_,config)) { config_=config; ++revision_; ready_=false; } else config_=config;
    if(active_ && !active) idle_=std::chrono::steady_clock::now();
    if(active && !active_ && status_=="加载失败") ++revision_;
    active_=active;
    if(active && !thread_.joinable()) thread_=std::thread([this]{ Run(); });
    changed_.notify_all();
}
pid_t LocalRuntime::ProcessId() const { std::lock_guard<std::mutex> lock(mutex_); return child_; }
std::string LocalRuntime::Status() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }
bool LocalRuntime::Resolve(Config* config,const std::atomic<bool>& cancelled,std::string* error) {
    if(config->channel!=Channel::ManagedLocal) return true;
    std::unique_lock<std::mutex> lock(mutex_);
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(config->timeout_seconds);
    while(!cancelled.load() && !stopping_ && active_ && Same(*config,config_) && !ready_ && status_!="加载失败" && std::chrono::steady_clock::now()<end) changed_.wait_for(lock,std::chrono::milliseconds(30));
    if(cancelled.load()) return false;
    if(!ready_ || !active_ || !Same(*config,config_)) { *error="本地模型未就绪；检查模型与运行程序路径"; return false; }
    config->owned_local=true; config->channel=Channel::LocalApi; config->endpoint=endpoint_; config->api_key=key_; config->model="rimes-local"; return true;
}
bool LocalRuntime::Start(const Config& c) {
    if(access(c.runtime_file.c_str(),X_OK)!=0 || access(c.model_file.c_str(),R_OK)!=0) return false;
    const int socket_fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0); if(socket_fd<0) return false;
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    socklen_t length=sizeof(addr);
    if(bind(socket_fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)) || getsockname(socket_fd,reinterpret_cast<sockaddr*>(&addr),&length)) { close(socket_fd); return false; }
    const auto port=std::to_string(ntohs(addr.sin_port)); close(socket_fd);
    unsigned char random[24]; const int random_fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if(random_fd<0) return false;
    const auto read_count=read(random_fd,random,sizeof(random)); close(random_fd); if(read_count!=sizeof(random)) return false;
    std::ostringstream token; for(auto byte:random) token<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(byte);
    std::string key_file="/tmp/rimes-model-key-XXXXXX"; const int key_fd=mkstemp(key_file.data()); if(key_fd<0) return false;
    const auto key=token.str(); const auto written=write(key_fd,key.data(),key.size()); close(key_fd);
    if(written!=static_cast<ssize_t>(key.size())) { unlink(key_file.c_str()); return false; }
    const char* override_host=std::getenv("RIMES_MODEL_HOST"); const std::string host=override_host ? override_host : RIMES_MODEL_HOST_PATH;
    std::vector<std::string> args={host,c.runtime_file,"-m",c.model_file,"--alias","rimes-local","--host","127.0.0.1","--port",port,"--api-key-file",key_file,"-t",std::to_string(c.threads),"-tb",std::to_string(c.threads),"-c",std::to_string(c.context_size),"-n",std::to_string(c.max_tokens),"-b","128","-ub","128","--parallel","1","--n-gpu-layers","0","--reasoning","off","--chat-template-kwargs","{\"enable_thinking\":false}","--no-webui","--poll","0"};
    std::vector<char*> argv; for(auto& arg:args) argv.push_back(arg.data()); argv.push_back(nullptr);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0); posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    pid_t child=0; const int result=posix_spawn(&child,host.c_str(),&actions,nullptr,argv.data(),environ); posix_spawn_file_actions_destroy(&actions);
    if(result) { unlink(key_file.c_str()); return false; }
    { std::lock_guard<std::mutex> lock(mutex_); child_=child; key_=key; key_file_=key_file; endpoint_="http://127.0.0.1:"+port+"/v1/chat/completions"; }
    return true;
}
void LocalRuntime::Stop() {
    pid_t child; std::string key_file;
    { std::lock_guard<std::mutex> lock(mutex_); child=child_; ready_=false; key_file=std::move(key_file_); key_.clear(); endpoint_.clear(); }
    if(child>0) {
        kill(child,SIGTERM);
        bool reaped=false;
        for(int i=0;i<20;++i) { if(waitpid(child,nullptr,WNOHANG)==child) { reaped=true; break; } std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
        if(!reaped) { kill(child,SIGKILL); waitpid(child,nullptr,0); }
    }
    if(!key_file.empty()) unlink(key_file.c_str());
    { std::lock_guard<std::mutex> lock(mutex_); child_=0; }
}
void LocalRuntime::Run() {
    std::uint64_t launched_revision=0;
    bool failed=false;
    while(true) {
        Config c; bool active,stop,ready; std::uint64_t revision; pid_t child; std::string endpoint; std::chrono::steady_clock::time_point idle;
        { std::unique_lock<std::mutex> lock(mutex_); changed_.wait_for(lock,std::chrono::milliseconds(50)); c=config_; active=active_; stop=stopping_; revision=revision_; child=child_; ready=ready_; endpoint=endpoint_; idle=idle_; }
        if(stop) break;
        if(child && (revision!=launched_revision || (!active && !ready) || (!active && std::chrono::steady_clock::now()-idle>=std::chrono::seconds(c.idle_seconds)))) { Stop(); child=0; std::lock_guard<std::mutex> lock(mutex_); status_="未加载"; failed=false; }
        if(!child && active && (!failed || revision!=launched_revision)) {
            { std::lock_guard<std::mutex> lock(mutex_); status_="正在预加载"; }
            launched_revision=revision;
            if(!Start(c)) { std::lock_guard<std::mutex> lock(mutex_); status_="加载失败"; failed=true; changed_.notify_all(); }
            continue;
        }
        if(child && !ready) {
            if(waitpid(child,nullptr,WNOHANG)==child) { { std::lock_guard<std::mutex> lock(mutex_); child_=0; status_="加载失败"; } Stop(); failed=true; changed_.notify_all(); }
            else if(Healthy(endpoint)) { std::lock_guard<std::mutex> lock(mutex_); ready_=true; status_=active_ ? "已预热" : "短暂缓存"; changed_.notify_all(); }
        } else if(child && ready) {
            if(waitpid(child,nullptr,WNOHANG)==child) { { std::lock_guard<std::mutex> lock(mutex_); child_=0; status_="加载失败"; } Stop(); failed=true; changed_.notify_all(); }
            else { std::lock_guard<std::mutex> lock(mutex_); status_=active_ ? "已预热" : "短暂缓存"; }
        }
    }
    Stop();
}
}
