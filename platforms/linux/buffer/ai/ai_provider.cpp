#include "ai_provider.hpp"
#include "local_runtime.hpp"
#include "ai_suggestion.hpp"
#include "ai_shortcut.hpp"
#include <curl/curl.h>
#include <json/json.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <limits>

namespace rimes::ai {
namespace {
constexpr std::size_t kConfigLimit = 65536;
constexpr std::size_t kTextLimit = 32768;
bool Fail(std::string* error, const char* message) { if (error) *error = message; return false; }
bool Control(const std::string& text) { return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }); }
bool Parse(const std::string& text, Json::Value* value) {
    Json::CharReaderBuilder builder;
    builder["rejectDupKeys"] = true;
    builder["failIfExtra"] = true;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string ignored;
    return reader->parse(text.data(), text.data()+text.size(), value, &ignored);
}
std::string Encode(const Json::Value& value) { Json::StreamWriterBuilder builder; builder["indentation"] = ""; return Json::writeString(builder, value); }
bool CurlReady() { static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; return ready; }
bool ValidUtf8(const std::string& text) {
    std::size_t i=0;
    while(i<text.size()) {
        const auto first=static_cast<unsigned char>(text[i++]);
        if(first<128) { if(first==0) return false; continue; }
        unsigned count=0, code=0, minimum=0;
        if(first>=0xc2 && first<=0xdf) { count=1; code=first&31; minimum=0x80; }
        else if(first>=0xe0 && first<=0xef) { count=2; code=first&15; minimum=0x800; }
        else if(first>=0xf0 && first<=0xf4) { count=3; code=first&7; minimum=0x10000; }
        else return false;
        if(i+count>text.size()) return false;
        while(count--) { const auto byte=static_cast<unsigned char>(text[i++]); if((byte&0xc0)!=0x80) return false; code=(code<<6)|(byte&63); }
        if(code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff)) return false;
    }
    return true;
}
struct Response { std::string body, lines, content; const std::atomic<bool>* cancelled; Preview preview; bool streaming=false, done=false, truncated=false; };
size_t Write(char* data, size_t size, size_t count, void* opaque) {
    auto* r=static_cast<Response*>(opaque);
    if(r->cancelled->load() || (size && count>std::numeric_limits<std::size_t>::max()/size)) return 0;
    const auto bytes=size*count;
    if(bytes>1024*1024 || r->body.size()>1024*1024-bytes) return 0;
    try {
        r->body.append(data,bytes); r->lines.append(data,bytes);
        std::size_t end;
        while((end=r->lines.find('\n'))!=std::string::npos) {
            auto line=r->lines.substr(0,end); r->lines.erase(0,end+1);
            if(line.rfind("data:",0)!=0) continue;
            r->streaming=true; auto json_text=line.substr(5);
            if(json_text.find("[DONE]")!=std::string::npos) { r->done=true; continue; }
            Json::Value json;
            if(!Parse(json_text,&json) || !json["choices"].isArray() || json["choices"].empty()) continue;
            const auto& choice=json["choices"][0];
            if(choice["delta"]["content"].isString()) { r->content+=choice["delta"]["content"].asString(); if(r->content.size()>kTextLimit) return 0; if(r->preview) r->preview(r->content); }
            if(choice["finish_reason"].isString()) { r->done=true; r->truncated=choice["finish_reason"].asString()!="stop"; }
        }
    } catch(...) { return 0; }
    return bytes;
}
int Progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return static_cast<const std::atomic<bool>*>(opaque)->load() ? 1 : 0; }
}

Config::Config() {
    const char* data=std::getenv("XDG_DATA_HOME"); const char* home=std::getenv("HOME");
    std::filesystem::path root=data && data[0]=='/' ? data : home ? std::string(home)+"/.local/share" : "";
    model_file=(root/"rimes/models/Qwen3-0.6B-Q4_K_M.gguf").string();
    runtime_file=(root/"rimes/models/runtime/llama-server").string();
}
std::filesystem::path ConfigPath() {
    if (const char* root = std::getenv("XDG_CONFIG_HOME"); root && root[0] == '/') return std::filesystem::path(root)/"rimes/ai.json";
    if (const char* home = std::getenv("HOME"); home && home[0] == '/') return std::filesystem::path(home)/".config/rimes/ai.json";
    return {};
}
bool ValidateConfig(const Config& config, std::string* error) {
    if (!CurlReady()) return Fail(error, "无法初始化网络服务");
    if (!ValidateShortcut(config.accept_key,error)) return false;
    if (config.idle_seconds<1 || config.idle_seconds>3600 || config.threads<1 || config.threads>4 || config.context_size<512 || config.context_size>2048 || config.max_tokens<16 || config.max_tokens>256 || config.debounce_ms<0 || config.debounce_ms>2000) return Fail(error,"本地资源或停顿配置超出允许范围");
    if (config.instruction.size()>kTextLimit || config.target_language.empty() || config.target_language.size()>64 || Control(config.target_language)) return Fail(error, "指令或目标语言无效");
    if (config.timeout_seconds<5 || config.timeout_seconds>180) return Fail(error, "请求超时须为 5–180 秒");
    if (config.channel==Channel::ManagedLocal) {
        if (!std::filesystem::path(config.model_file).is_absolute() || !std::filesystem::path(config.runtime_file).is_absolute() || Control(config.model_file) || Control(config.runtime_file)) return Fail(error,"本地模型和运行程序须为绝对路径");
        return true;
    }
    if (config.endpoint.empty() || config.model.empty()) return Fail(error, "请先设置 AI 服务地址与模型");
    if (config.endpoint.size()>2048 || config.model.size()>256 || config.api_key.size()>8192 || Control(config.endpoint) || Control(config.model) || Control(config.api_key)) return Fail(error, "服务配置包含无效字符或过长内容");
    CURLU* url = curl_url();
    if (!url) return Fail(error, "无法解析服务地址");
    bool valid = curl_url_set(url, CURLUPART_URL, config.endpoint.c_str(), 0) == CURLUE_OK;
    char* scheme = nullptr; char* host = nullptr; char* user = nullptr; char* password = nullptr;
    if (valid) {
        valid = curl_url_get(url,CURLUPART_SCHEME,&scheme,0)==CURLUE_OK && curl_url_get(url,CURLUPART_HOST,&host,0)==CURLUE_OK;
        if (valid) { const bool local=std::string(host)=="127.0.0.1" || std::string(host)=="localhost" || std::string(host)=="[::1]"; valid=(std::string(scheme)=="https" || (std::string(scheme)=="http" && local)) && (config.channel==Channel::CloudApi || local); }
        if (curl_url_get(url,CURLUPART_USER,&user,0)==CURLUE_OK || curl_url_get(url,CURLUPART_PASSWORD,&password,0)==CURLUE_OK) valid = false;
    }
    curl_free(scheme); curl_free(host); curl_free(user); curl_free(password); curl_url_cleanup(url);
    if (!valid) return Fail(error, "使用 HTTPS 服务地址；本机服务可使用 http://127.0.0.1");
    return true;
}
bool LoadConfig(const std::filesystem::path& path, Config* config, std::string* error) {
    if (!config || path.empty()) return Fail(error, "无法定位本机配置目录");
    *config = Config{};
    const int fd = open(path.c_str(), O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) { if(errno==ENOENT) return ValidateConfig(*config,error); return Fail(error,"无法读取 AI 配置"); }
    struct stat info{};
    if (fstat(fd,&info)!=0 || !S_ISREG(info.st_mode) || info.st_uid!=geteuid() || (info.st_mode&077)!=0 || info.st_size<0 || static_cast<std::size_t>(info.st_size)>kConfigLimit) { close(fd); return Fail(error, "AI 配置须为当前用户所有的 0600 普通文件"); }
    std::string bytes;
    char chunk[4096];
    ssize_t got;
    while ((got=read(fd,chunk,sizeof(chunk)))>0) {
        bytes.append(chunk,static_cast<std::size_t>(got));
        if (bytes.size()>kConfigLimit) { close(fd); return Fail(error,"AI 配置过大"); }
    }
    close(fd);
    Json::Value json;
    if (got<0 || !Parse(bytes,&json) || !json.isObject() || !json["version"].isInt() || (json["version"].asInt()!=1 && json["version"].asInt()!=2)) return Fail(error, "AI 配置格式无效");
    for (const char* key : {"endpoint","model","api_key","instruction","target_language"}) if (json.isMember(key) && !json[key].isString()) return Fail(error,"AI 配置字段类型无效");
    if (json.isMember("timeout_seconds") && !json["timeout_seconds"].isInt()) return Fail(error,"AI 超时配置无效");
    config->endpoint=json.get("endpoint", "").asString(); config->model=json.get("model", "").asString(); config->api_key=json.get("api_key", "").asString();
    config->instruction=json.get("instruction",config->instruction).asString(); config->target_language=json.get("target_language","en").asString(); config->timeout_seconds=json.get("timeout_seconds",60).asInt();
    if (json["version"].asInt()==1) config->channel=config->endpoint.find("127.0.0.1")!=std::string::npos || config->endpoint.find("localhost")!=std::string::npos || config->endpoint.find("[::1]")!=std::string::npos ? Channel::LocalApi : Channel::CloudApi;
    else {
        for(const char* key : {"channel","model_file","runtime_file","accept_key"}) if(json.isMember(key) && !json[key].isString()) return Fail(error,"配置字段类型无效");
        const auto channel=json.get("channel","managed_local").asString();
        if(channel!="managed_local" && channel!="local_api" && channel!="cloud_api") return Fail(error,"未知 AI 渠道");
        config->channel=channel=="managed_local" ? Channel::ManagedLocal : channel=="local_api" ? Channel::LocalApi : Channel::CloudApi;
        config->model_file=json.get("model_file",config->model_file).asString(); config->runtime_file=json.get("runtime_file",config->runtime_file).asString(); config->accept_key=json.get("accept_key","Tab").asString();
        for(auto item : {std::pair<const char*,int*>{"idle_seconds",&config->idle_seconds},{"threads",&config->threads},{"context_size",&config->context_size},{"max_tokens",&config->max_tokens},{"debounce_ms",&config->debounce_ms}}) { if(json.isMember(item.first)) { if(!json[item.first].isInt()) return Fail(error,"配置字段类型无效"); *item.second=json[item.first].asInt(); } }
    }
    return ValidateConfig(*config,error);
}
bool SaveConfig(const std::filesystem::path& path, const Config& config, std::string* error) {
    if (path.empty() || !path.is_absolute()) return Fail(error,"无法定位本机配置目录");
    if (!ValidateConfig(config,error)) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(),ec);
    struct stat parent{};
    if (ec || lstat(path.parent_path().c_str(),&parent)!=0 || !S_ISDIR(parent.st_mode) || parent.st_uid!=geteuid()) return Fail(error,"无法写入私有配置目录");
    if (chmod(path.parent_path().c_str(),0700)!=0) return Fail(error,"无法保护配置目录权限");
    Json::Value json; json["version"]=2; json["channel"]=config.channel==Channel::ManagedLocal ? "managed_local" : config.channel==Channel::LocalApi ? "local_api" : "cloud_api"; json["model_file"]=config.model_file; json["runtime_file"]=config.runtime_file; json["idle_seconds"]=config.idle_seconds; json["threads"]=config.threads; json["context_size"]=config.context_size; json["max_tokens"]=config.max_tokens; json["debounce_ms"]=config.debounce_ms; json["accept_key"]=config.accept_key; json["endpoint"]=config.endpoint; json["model"]=config.model; json["api_key"]=config.api_key; json["instruction"]=config.instruction; json["target_language"]=config.target_language; json["timeout_seconds"]=config.timeout_seconds;
    const auto bytes=Encode(json);
    std::string temporary=(path.parent_path()/".ai-XXXXXX").string();
    const int fd=mkstemp(temporary.data());
    if (fd<0) return Fail(error,"无法创建私有配置文件");
    bool ok=fchmod(fd,0600)==0;
    std::size_t written=0;
    while (ok && written<bytes.size()) { const auto n=write(fd,bytes.data()+written,bytes.size()-written); if (n<=0) ok=false; else written+=static_cast<std::size_t>(n); }
    if (ok) ok=fsync(fd)==0;
    close(fd);
    if (ok) ok=rename(temporary.c_str(),path.c_str())==0;
    if (!ok) { unlink(temporary.c_str()); return Fail(error,"无法保存 AI 配置"); }
    return true;
}
Reply Request(const Config& config, const std::string& source, Purpose purpose, const std::atomic<bool>& cancelled, Preview preview) {
    Reply reply;
    if (cancelled.load()) { reply.cancelled=true; return reply; }
    if (!ValidateConfig(config,&reply.error)) return reply;
    if (source.empty() || source.size()>kTextLimit) { reply.error="输入须为 1–32768 字节，请分段处理"; return reply; }
    Json::Value request; request["model"]=config.model; request["stream"]=true; request["max_tokens"]=config.max_tokens; request["temperature"]=0.0; request["chat_template_kwargs"]["enable_thinking"]=false;
    Json::Value system; system["role"]="system";
    system["content"]=purpose==Purpose::Suggest ? SuggestionPrompt() : purpose==Purpose::Translate ? "Translate the user's text into "+config.target_language+". Detect the source language. Return only the translation, without commentary or quotes." : config.instruction;
    Json::Value user; user["role"]="user"; user["content"]=source;
    request["messages"].append(system);
    if(purpose==Purpose::Suggest) {
        Json::Value example_user,example_reply;
        example_user["role"]="user"; example_user["content"]="我今天今天去学校。";
        example_reply["role"]="assistant"; example_reply["content"]=R"({"text":"我今天去学校。"})";
        request["messages"].append(example_user); request["messages"].append(example_reply);
        example_user["content"]="I am going to"; example_reply["content"]=R"({"text":"I am going to school."})";
        request["messages"].append(example_user); request["messages"].append(example_reply);
        example_user["content"]="今天很开心。"; example_reply["content"]=R"({"text":"今天很开心。期待明天。"})";
        request["messages"].append(example_user); request["messages"].append(example_reply);
        if(config.owned_local) request["grammar"]=R"GBNF(root ::= "{\"text\":" ws string ws "}" ws
string ::= "\"" char* "\""
char ::= [^"\\\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
ws ::= [ \t\n\r]*
)GBNF";
        else request["response_format"]["type"]="json_object";
    }
    request["messages"].append(user);
    const auto body=Encode(request);
    CURL* curl=curl_easy_init();
    if (!curl) { reply.error="无法初始化请求"; return reply; }
    curl_slist* headers=nullptr;
    headers=curl_slist_append(headers,"Content-Type: application/json");
    const std::string authorization="Authorization: Bearer "+config.api_key;
    if (!config.api_key.empty()) headers=curl_slist_append(headers,authorization.c_str());
    Response response{{},{},{},&cancelled,std::move(preview)};
    curl_easy_setopt(curl,CURLOPT_URL,config.endpoint.c_str());
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"http,https");
    if(config.channel==Channel::LocalApi) curl_easy_setopt(curl,CURLOPT_PROXY,"");
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L); curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L); curl_easy_setopt(curl,CURLOPT_TIMEOUT,static_cast<long>(config.timeout_seconds));
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,body.c_str()); curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,static_cast<long>(body.size()));
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,Write); curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);
    curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L); curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,Progress); curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&cancelled);
    const auto status=curl_easy_perform(curl);
    long http=0; curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http);
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    if (cancelled.load()) { reply.cancelled=true; return reply; }
    if (status!=CURLE_OK) { reply.error=status==CURLE_OPERATION_TIMEDOUT ? "请求超时，可重试" : "网络请求失败，请检查地址与连接"; return reply; }
    if (http<200 || http>=300) { reply.error=http==401 || http==403 ? "服务拒绝认证，请检查 API 密钥" : http==429 ? "服务请求限额已达到，请稍后重试" : "服务返回错误（HTTP "+std::to_string(http)+"）"; return reply; }
    if(response.streaming) {
        if(!response.done || response.truncated) { reply.error="建议尚未完整生成；保留原文，请重试"; return reply; }
        reply.text=std::move(response.content);
    } else {
        Json::Value json;
        if (!Parse(response.body,&json) || !json.isObject() || !json["choices"].isArray() || json["choices"].empty() || !json["choices"][0].isObject() || !json["choices"][0]["message"].isObject()) { reply.error="服务响应不是兼容的 Chat Completions 格式"; return reply; }
        if(json["choices"][0]["finish_reason"].isString() && json["choices"][0]["finish_reason"].asString()!="stop") { reply.error="结果超过输出预算；保留原文"; return reply; }
        const auto& content=json["choices"][0]["message"]["content"];
        if(content.isString()) reply.text=content.asString();
        else if(content.isArray()) for(const auto& part:content) if(part.isObject() && part["text"].isString()) reply.text+=part["text"].asString();
    }
    if (reply.text.empty() || reply.text.size()>kTextLimit || !ValidUtf8(reply.text)) { reply.text.clear(); reply.error="服务返回空结果、无效文本或结果过长"; }
    return reply;
}
Worker::Worker() : thread_([this] { Run(); }), runtime_(std::make_unique<LocalRuntime>()) {}
Worker::~Worker() { { std::lock_guard<std::mutex> lock(mutex_); stopping_=true; if(active_) active_->store(true); if(pending_) pending_->cancelled->store(true); pending_.reset(); } changed_.notify_one(); thread_.join(); }
void Worker::Cancel() { std::lock_guard<std::mutex> lock(mutex_); if(active_) active_->store(true); if(pending_) pending_->cancelled->store(true); pending_.reset(); }
void Worker::Submit(Config config, std::string source, bool translation, Completion completion) {
    Submit(std::move(config),std::move(source),translation ? Purpose::Translate : Purpose::Generate,std::move(completion));
}
void Worker::SetModelActive(const Config& config, bool active) { runtime_->SetActive(config,active); }
std::string Worker::ModelStatus() const { return runtime_->Status(); }
void Worker::Submit(Config config, std::string source, Purpose purpose, Completion completion, Preview preview) {
    { std::lock_guard<std::mutex> lock(mutex_); if(active_) active_->store(true); pending_=Task{std::move(config),std::move(source),purpose,std::move(completion),std::move(preview),std::make_shared<std::atomic<bool>>(false)}; } changed_.notify_one();
}
void Worker::Run() {
    while (true) {
        std::optional<Task> task;
        { std::unique_lock<std::mutex> lock(mutex_); changed_.wait(lock,[this] { return stopping_ || pending_.has_value(); }); if(stopping_) return; task=std::move(pending_); pending_.reset(); active_=task->cancelled; }
        Reply reply;
        try { Config resolved=task->config; if(runtime_->Resolve(&resolved,*task->cancelled,&reply.error)) reply=Request(resolved,task->source,task->purpose,*task->cancelled,task->preview); else reply.cancelled=task->cancelled->load(); }
        catch (...) { reply.error="无法处理服务响应"; }
        task->completion(std::move(reply));
        { std::lock_guard<std::mutex> lock(mutex_); if(active_==task->cancelled) active_.reset(); }
    }
}
} // namespace rimes::ai
