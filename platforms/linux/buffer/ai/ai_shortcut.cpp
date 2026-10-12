#include "ai_shortcut.hpp"
#include <fcitx-utils/key.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
namespace rimes::ai {
bool ValidateShortcut(const std::string& text,std::string* error) {
    if(text.empty()) return true;
    const fcitx::Key key(text); if(!key.isValid() || key.code()!=0 || text.size()>128) { if(error) *error="采纳快捷键无效，例如 Tab 或 Control+semicolon"; return false; }
    const auto normalized=key.normalize();
    for(const char* reserved : {"Return","KP_Enter","Escape","BackSpace","Delete","space","Left","Right","Up","Down","Home","End","Control+a","Control+v","Control+Shift+b","Super+Shift+b","Control+Shift+v","Super+Shift+v","Control+space","Shift+space","Control+grave","F4","Alt+Tab","Super+Tab","Control+Alt+Delete"}) if(normalized==fcitx::Key(reserved).normalize()) { if(error) *error="快捷键与输入法或 Buffer 操作冲突；原配置未改变"; return false; }
    if(normalized.states()==fcitx::KeyStates{} && normalized.sym()>=FcitxKey_0 && normalized.sym()<=FcitxKey_9) { if(error) *error="数字键用于 Rime 选词；原配置未改变"; return false; }
    if(normalized.sym()!=FcitxKey_Tab && normalized.states()==fcitx::KeyStates{} && normalized.sym()<128) { if(error) *error="可打印按键须带修饰键；原配置未改变"; return false; }
    const char* xdg=std::getenv("XDG_CONFIG_HOME"); const char* home=std::getenv("HOME");
    const std::filesystem::path root=xdg && xdg[0]=='/' ? xdg : home ? std::string(home)+"/.config" : "";
    std::ifstream config(root/"fcitx5/config"); std::string line,section;
    while(std::getline(config,line)) { if(!line.empty() && line.front()=='[') section=line; else if(section.find("Hotkey/")!=std::string::npos) { const auto eq=line.find('='); if(eq!=std::string::npos) for(const auto& other:fcitx::Key::keyListFromString(line.substr(eq+1))) if(other.isValid() && other.normalize()==normalized) { if(error) *error="快捷键与已配置的 Fcitx5 热键冲突；原配置未改变"; return false; } } }
    std::ifstream kde(root/"kglobalshortcutsrc");
    while(std::getline(kde,line)) {
        const auto eq=line.find('='); if(eq==std::string::npos) continue;
        auto qt=line.substr(eq+1); qt=qt.substr(0,qt.find(','));
        for(const auto& [from,to] : {std::pair<std::string,std::string>{"Ctrl+","Control+"},{"Meta+","Super+"},{"Esc","Escape"},{"Space","space"},{"Del","Delete"}}) { auto at=qt.find(from); if(at!=std::string::npos) qt.replace(at,from.size(),to); }
        if(qt.size()>1 && qt[qt.size()-2]=='+' && qt.back()>='A' && qt.back()<='Z') qt.back()=static_cast<char>(qt.back()-'A'+'a');
        const fcitx::Key other(qt);
        if(other.isValid() && other.normalize()==normalized) { if(error) *error="快捷键与 KDE 全局快捷键冲突；原配置未改变"; return false; }
    }
    return true;
}
}
