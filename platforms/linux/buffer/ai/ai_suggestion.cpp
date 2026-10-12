#include "ai_suggestion.hpp"
#include <json/json.h>
#include <algorithm>
#include <memory>
namespace rimes::ai {
namespace {
bool Equal(const std::vector<buffer::Block>& a,const std::vector<buffer::Block>& b) { if(a.size()!=b.size()) return false; for(std::size_t i=0;i<a.size();++i) if(a[i].id!=b[i].id || a[i].text!=b[i].text) return false; return true; }
std::vector<std::string> Chars(const std::string& s) {
    std::vector<std::string> chars;
    for(std::size_t i=0;i<s.size();) { unsigned char b=s[i]; const std::size_t n=b<128 ? 1 : b>=0xc2 && b<=0xdf ? 2 : b>=0xe0 && b<=0xef ? 3 : b>=0xf0 && b<=0xf4 ? 4 : 0; if(!n || i+n>s.size() || b==0) return {}; for(std::size_t j=1;j<n;++j) if((static_cast<unsigned char>(s[i+j])&0xc0)!=0x80) return {};
        if(n>=3) { const auto second=static_cast<unsigned char>(s[i+1]); if((b==0xe0 && second<0xa0) || (b==0xed && second>=0xa0) || (b==0xf0 && second<0x90) || (b==0xf4 && second>=0x90)) return {}; } chars.push_back(s.substr(i,n)); i+=n; }
    return chars;
}
std::size_t Bytes(const std::vector<std::string>& chars,std::size_t n) { std::size_t size=0; for(std::size_t i=0;i<n;++i) size+=chars[i].size(); return size; }
}
std::string SuggestionPrompt() {
    return "Keep the input language. NEVER translate. Fix only obvious typos, duplicated words, missing characters or punctuation. Preserve meaning and wording. Otherwise append a very short natural continuation. If no useful suggestion, return the original. Output JSON with text containing the ENTIRE corrected or continued text. 中文输入必须输出中文，禁止翻译。";
}
bool SentenceEnd(const std::string& text) { if(text.empty()) return false; for(const std::string ending : {".","!","?","\n","。","！","？"}) if(text.size()>=ending.size() && text.compare(text.size()-ending.size(),ending.size(),ending)==0) return true; return false; }
std::string SuggestionPreview(const std::string& json,const std::string& source) {
    // Decode only a JSON string value; incomplete escapes stay hidden.
    const auto key=json.find("\"text\""); if(key==std::string::npos) return {};
    auto pos=json.find(':',key+6); if(pos==std::string::npos) return {};
    pos=json.find('"',pos+1); if(pos==std::string::npos) return {};
    std::string value; bool escape=false;
    for(++pos;pos<json.size();++pos) { const char c=json[pos]; if(escape) { if(c=='"' || c=='\\' || c=='/') value+=c; else if(c=='n') value+='\n'; else return value; escape=false; } else if(c=='\\') escape=true; else if(c=='"') break; else value+=c; }
    // Network fragments may split one UTF-8 code point.
    while(!value.empty() && Chars(value).empty()) value.pop_back();
    if(!source.empty()) { if(source.rfind(value,0)==0) return {}; if(value.rfind(source,0)==0) return value.substr(source.size()); }
    return value;
}
void Suggestion::Invalidate() { ++revision; working=false; complete=false; attempted=false; kind.clear(); text.clear(); preview.clear(); error.clear(); }
bool Suggestion::Matches(const buffer::BufferModel& model,const std::string& token) const { return target==token && insertion==model.insertion_index() && !model.all_content_selected() && Equal(source,model.blocks()); }
bool Suggestion::Sync(const buffer::BufferModel& model,const std::string& token) {
    if(Matches(model,token)) return false;
    Invalidate(); source=model.blocks(); insertion=model.insertion_index(); target=token;
    context.clear(); for(int i=0;i<insertion && i<static_cast<int>(source.size());++i) context+=source[i].text;
    end=begin=context.size(); context_offset=0;
    auto chars=Chars(context); if(chars.size()>64) { context_offset=Bytes(chars,chars.size()-64); context.erase(0,context_offset); }
    return true;
}
bool Suggestion::Finish(std::uint64_t frozen_revision,const std::string& json) {
    if(frozen_revision!=revision) return false;
    working=false; complete=false; preview.clear();
    Json::CharReaderBuilder builder; builder["rejectDupKeys"]=true; builder["failIfExtra"]=true; std::unique_ptr<Json::CharReader> reader(builder.newCharReader()); Json::Value result; std::string ignored;
    if(!reader->parse(json.data(),json.data()+json.size(),&result,&ignored) || !result.isObject() || !result["text"].isString()) { error="建议格式未通过校验"; return false; }
    std::string original;
    text=result["text"].asString();
    // The model proposes text; labels and replacement ranges are derived from
    // the frozen source, rather than trusting model-generated offsets or labels.
    if(!result.isMember("kind")) {
        if(text==context) { text.clear(); return false; }
        if(text.rfind(context,0)==0) { kind="continuation"; text.erase(0,context.size()); }
        else { kind="correction"; original=context; }
    } else {
        if(!result["kind"].isString() || !result["original"].isString()) { text.clear(); return false; }
        kind=result["kind"].asString(); original=result["original"].asString();
    }
    if(kind=="none") { text.clear(); kind.clear(); return false; }
    auto replacement=Chars(text); if((!text.empty() && replacement.empty()) || text.find_first_of("\n\r")!=std::string::npos || replacement.size()>80) { error="建议长度或文本无效"; text.clear(); return false; }
    if(kind=="continuation") { if(text.empty() || !original.empty() || replacement.size()>24) { text.clear(); return false; } complete=true; return true; }
    if(kind!="correction" || original.empty() || original==text) { text.clear(); return false; }
    const auto at=context.find(original);
    if(at==std::string::npos || context.find(original,at+1)!=std::string::npos) { error="校正范围不唯一或与原文不符"; text.clear(); return false; }
    auto old=Chars(original); if(old.empty() || old.size()>64) { text.clear(); return false; }
    // Conservative edit distance: reject paragraph rewrites from a small model.
    std::vector<std::size_t> d(replacement.size()+1); for(std::size_t i=0;i<d.size();++i) d[i]=i;
    for(std::size_t i=0;i<old.size();++i) { auto prior=d[0]; d[0]=i+1; for(std::size_t j=0;j<replacement.size();++j) { auto saved=d[j+1]; d[j+1]=std::min({d[j+1]+1,d[j]+1,prior+(old[i]!=replacement[j])}); prior=saved; } }
    if(d.back()>3 || (old.size()>8 && d.back()*4>old.size())) { error="校正改动过大，保留原文"; text.clear(); return false; }
    std::size_t prefix=0,suffix=0;
    while(prefix<old.size() && prefix<replacement.size() && old[prefix]==replacement[prefix]) ++prefix;
    while(suffix+prefix<old.size() && suffix+prefix<replacement.size() && old[old.size()-1-suffix]==replacement[replacement.size()-1-suffix]) ++suffix;
    begin=context_offset+at+Bytes(old,prefix); end=context_offset+at+original.size()-Bytes(std::vector<std::string>(old.end()-suffix,old.end()),suffix);
    text.clear(); for(std::size_t i=prefix;i<replacement.size()-suffix;++i) text+=replacement[i];
    complete=true; return true;
}
}
