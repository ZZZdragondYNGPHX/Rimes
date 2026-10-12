#include "ai_settings.hpp"
#include "ai_provider.hpp"
#include "buffer_protocol.hpp"
#include <gtk/gtk.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <chrono>
#include <cstring>
#include <string>

namespace {
struct Form { GtkWidget* window; GtkWidget* endpoint; GtkWidget* model; GtkWidget* key; GtkWidget* language; GtkWidget* timeout; GtkWidget* instruction; GtkWidget* status; GtkWidget* channel; GtkWidget* model_file; GtkWidget* runtime_file; GtkWidget* idle; GtkWidget* threads; GtkWidget* debounce; GtkWidget* accept_key; rimes::ai::Config saved; bool preview=false; };
void Reload() {
    std::string path;
    if (const char* runtime=g_getenv("XDG_RUNTIME_DIR")) path=std::string(runtime)+"/rimes-buffer.sock";
    else path="/tmp/rimes-buffer-"+std::to_string(geteuid())+".sock";
    if (const char* socket=g_getenv("RIMES_BUFFER_SOCKET")) path=socket;
    if (path.size()>=sizeof(sockaddr_un::sun_path)) return;
    const int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if (fd<0) return;
    sockaddr_un address{}; address.sun_family=AF_UNIX;
    std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    if (connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0) {
        std::string frame; rimes::buffer::EncodeFrame(R"({"v":1,"op":"ai_reload"})",&frame);
        (void)send(fd,frame.data(),frame.size(),MSG_NOSIGNAL);
        // Keep this one-shot client alive until the addon has applied reload;
        // otherwise its initial snapshot can hit EPIPE before the command is read.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        std::string incoming;
        while(std::chrono::steady_clock::now()<deadline) {
            pollfd ready{fd,POLLIN,0};
            if(poll(&ready,1,100)<=0) continue;
            char bytes[4096]; const auto got=recv(fd,bytes,sizeof(bytes),MSG_DONTWAIT);
            if(got<=0) break;
            incoming.append(bytes,static_cast<std::size_t>(got));
            if(incoming.find("\"type\":\"ai_reloaded\"")!=std::string::npos || incoming.size()>rimes::buffer::kMaxFrameBytes) break;
        }
    }
    close(fd);
}
GtkWidget* Entry(GtkGrid* grid, int row, const char* title, const std::string& value, const char* placeholder) {
    GtkWidget* label=gtk_label_new(title); gtk_widget_set_halign(label,GTK_ALIGN_START);
    GtkWidget* entry=gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(entry),value.c_str()); gtk_entry_set_placeholder_text(GTK_ENTRY(entry),placeholder);
    gtk_widget_set_hexpand(entry,TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label),entry);
    gtk_grid_attach(grid,label,0,row,1,1); gtk_grid_attach(grid,entry,1,row,1,1);
    return entry;
}
}
int RunAiSettings(bool preview) {
    Form form{}; form.preview=preview;
    rimes::ai::Config config; std::string error;
    if (!preview) rimes::ai::LoadConfig(rimes::ai::ConfigPath(),&config,&error);
    form.saved=config;
    form.window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(form.window),"RIMES · AI 与翻译");
    gtk_window_set_icon_name(GTK_WINDOW(form.window),"rimes");
    gtk_window_set_default_size(GTK_WINDOW(form.window),680,740);
    gtk_container_set_border_width(GTK_CONTAINER(form.window),24);
    GtkWidget* page=gtk_scrolled_window_new(nullptr,nullptr); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC); gtk_container_add(GTK_CONTAINER(form.window),page);
    GtkWidget* content=gtk_box_new(GTK_ORIENTATION_VERTICAL,16); gtk_container_add(GTK_CONTAINER(page),content);
    GtkWidget* title=gtk_label_new(nullptr); gtk_label_set_markup(GTK_LABEL(title),"<b>AI 生成与实时翻译</b>"); gtk_widget_set_halign(title,GTK_ALIGN_START); gtk_box_pack_start(GTK_BOX(content),title,FALSE,FALSE,0);
    GtkWidget* description=gtk_label_new("普通输入只用 Rime。进入 AI / 翻译时异步预热本地模型。\nAI 建议由你采纳；翻译按原文更新。云端须明确选择，不自动回退。");
    gtk_label_set_line_wrap(GTK_LABEL(description),TRUE); gtk_label_set_xalign(GTK_LABEL(description),0); gtk_box_pack_start(GTK_BOX(content),description,FALSE,FALSE,0);
    GtkWidget* grid_widget=gtk_grid_new(); auto* grid=GTK_GRID(grid_widget); gtk_grid_set_row_spacing(grid,12); gtk_grid_set_column_spacing(grid,16); gtk_box_pack_start(GTK_BOX(content),grid_widget,FALSE,FALSE,0);
    GtkWidget* channel_label=gtk_label_new("推理渠道"); gtk_widget_set_halign(channel_label,GTK_ALIGN_START); gtk_grid_attach(grid,channel_label,0,0,1,1);
    form.channel=gtk_combo_box_text_new(); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(form.channel),"managed_local","本地轻量模型（推荐）"); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(form.channel),"local_api","已有 localhost API"); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(form.channel),"cloud_api","云端 API（明确启用）"); gtk_combo_box_set_active(GTK_COMBO_BOX(form.channel),config.channel==rimes::ai::Channel::ManagedLocal ? 0 : config.channel==rimes::ai::Channel::LocalApi ? 1 : 2); gtk_grid_attach(grid,form.channel,1,0,1,1);
    form.endpoint=Entry(grid,1,"服务地址",config.endpoint,"https://服务域名/v1/chat/completions");
    gtk_widget_set_tooltip_text(form.endpoint,"填写完整 Chat Completions 地址。本机服务可用 http://127.0.0.1:端口/v1/chat/completions");
    form.model=Entry(grid,2,"模型",config.model,"服务提供的模型名称");
    form.key=Entry(grid,3,"API 密钥",config.api_key,"本机服务可留空");
    gtk_entry_set_visibility(GTK_ENTRY(form.key),FALSE); gtk_entry_set_input_purpose(GTK_ENTRY(form.key),GTK_INPUT_PURPOSE_PASSWORD);
    GtkWidget* language_label=gtk_label_new("翻译目标"); gtk_widget_set_halign(language_label,GTK_ALIGN_START); gtk_grid_attach(grid,language_label,0,4,1,1);
    form.language=gtk_combo_box_text_new();
    for (const auto& language : {std::pair<const char*,const char*>{"en","英语"},{"zh","中文"},{"ja","日语"},{"ko","韩语"},{"fr","法语"},{"de","德语"}}) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(form.language),language.first,language.second);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(form.language),config.target_language.c_str()); if(gtk_combo_box_get_active(GTK_COMBO_BOX(form.language))<0) gtk_combo_box_set_active(GTK_COMBO_BOX(form.language),0);
    gtk_grid_attach(grid,form.language,1,4,1,1);
    GtkWidget* timeout_label=gtk_label_new("请求超时（秒）"); gtk_widget_set_halign(timeout_label,GTK_ALIGN_START); gtk_grid_attach(grid,timeout_label,0,5,1,1);
    form.timeout=gtk_spin_button_new_with_range(5,180,5); gtk_spin_button_set_value(GTK_SPIN_BUTTON(form.timeout),config.timeout_seconds); gtk_grid_attach(grid,form.timeout,1,5,1,1);
    form.model_file=Entry(grid,6,"GGUF 文件",config.model_file,"绝对路径");
    form.runtime_file=Entry(grid,7,"llama-server",config.runtime_file,"绝对路径");
    auto spin=[&](int row,const char* label,int low,int high,int value) { auto* title=gtk_label_new(label); gtk_widget_set_halign(title,GTK_ALIGN_START); gtk_grid_attach(grid,title,0,row,1,1); auto* widget=gtk_spin_button_new_with_range(low,high,1); gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget),value); gtk_grid_attach(grid,widget,1,row,1,1); return widget; };
    form.idle=spin(8,"退出后闲置卸载（秒）",1,3600,config.idle_seconds);
    form.threads=spin(9,"CPU 线程",1,4,config.threads);
    form.debounce=spin(10,"输入停顿（毫秒）",0,2000,config.debounce_ms);
    form.accept_key=Entry(grid,11,"采纳快捷键",config.accept_key,"留空关闭，例如 Control+semicolon");
    GtkWidget* reset=gtk_button_new_with_label("恢复默认 Tab"); gtk_grid_attach(grid,reset,1,12,1,1); g_signal_connect_swapped(reset,"clicked",G_CALLBACK((+[](GtkEntry* entry) { gtk_entry_set_text(entry,"Tab"); })),form.accept_key);
    auto visibility=+[](GtkComboBox* combo,gpointer data) { auto* f=static_cast<Form*>(data); const bool managed=gtk_combo_box_get_active(combo)==0; for(auto* w:{f->endpoint,f->model,f->key}) gtk_widget_set_sensitive(w,!managed); for(auto* w:{f->model_file,f->runtime_file,f->idle,f->threads}) gtk_widget_set_sensitive(w,managed); };
    g_signal_connect(form.channel,"changed",G_CALLBACK(visibility),&form); visibility(GTK_COMBO_BOX(form.channel),&form);
    GtkWidget* prompt_label=gtk_label_new("AI 生成指令"); gtk_widget_set_halign(prompt_label,GTK_ALIGN_START); gtk_box_pack_start(GTK_BOX(content),prompt_label,FALSE,FALSE,0);
    GtkWidget* scroll=gtk_scrolled_window_new(nullptr,nullptr); gtk_widget_set_size_request(scroll,-1,80); gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),GTK_SHADOW_IN);
    form.instruction=gtk_text_view_new(); gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(form.instruction),GTK_WRAP_WORD_CHAR); gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(form.instruction)),config.instruction.c_str(),-1); gtk_container_add(GTK_CONTAINER(scroll),form.instruction); gtk_box_pack_start(GTK_BOX(content),scroll,TRUE,TRUE,0);
    form.status=gtk_label_new(error.empty() ? "密钥只保存在本机私有配置文件。" : error.c_str()); gtk_label_set_line_wrap(GTK_LABEL(form.status),TRUE); gtk_label_set_xalign(GTK_LABEL(form.status),0); gtk_box_pack_start(GTK_BOX(content),form.status,FALSE,FALSE,0);
    GtkWidget* actions=gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL); gtk_button_box_set_layout(GTK_BUTTON_BOX(actions),GTK_BUTTONBOX_END); gtk_box_pack_end(GTK_BOX(content),actions,FALSE,FALSE,0);
    GtkWidget* close=gtk_button_new_with_label("关闭"); gtk_container_add(GTK_CONTAINER(actions),close); g_signal_connect_swapped(close,"clicked",G_CALLBACK(gtk_widget_destroy),form.window);
    GtkWidget* save=gtk_button_new_with_label("保存"); gtk_style_context_add_class(gtk_widget_get_style_context(save),"suggested-action"); gtk_container_add(GTK_CONTAINER(actions),save);
    g_signal_connect(save,"clicked",G_CALLBACK((+[](GtkButton*,gpointer data) {
        auto* form=static_cast<Form*>(data); if(form->preview) { gtk_label_set_text(GTK_LABEL(form->status),"预览不会保存配置"); return; }
        rimes::ai::Config config=form->saved;
        const auto channel=gtk_combo_box_get_active(GTK_COMBO_BOX(form->channel)); config.channel=channel==0 ? rimes::ai::Channel::ManagedLocal : channel==1 ? rimes::ai::Channel::LocalApi : rimes::ai::Channel::CloudApi;
        config.model_file=gtk_entry_get_text(GTK_ENTRY(form->model_file)); config.runtime_file=gtk_entry_get_text(GTK_ENTRY(form->runtime_file)); config.idle_seconds=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(form->idle)); config.threads=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(form->threads)); config.debounce_ms=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(form->debounce)); config.accept_key=gtk_entry_get_text(GTK_ENTRY(form->accept_key));
        config.endpoint=gtk_entry_get_text(GTK_ENTRY(form->endpoint)); config.model=gtk_entry_get_text(GTK_ENTRY(form->model)); config.api_key=gtk_entry_get_text(GTK_ENTRY(form->key));
        config.target_language=gtk_combo_box_get_active_id(GTK_COMBO_BOX(form->language)); config.timeout_seconds=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(form->timeout));
        GtkTextIter start,end; GtkTextBuffer* buffer=gtk_text_view_get_buffer(GTK_TEXT_VIEW(form->instruction)); gtk_text_buffer_get_bounds(buffer,&start,&end); gchar* text=gtk_text_buffer_get_text(buffer,&start,&end,FALSE); config.instruction=text; g_free(text);
        std::string error;
        if (!rimes::ai::SaveConfig(rimes::ai::ConfigPath(),config,&error)) gtk_label_set_text(GTK_LABEL(form->status),error.c_str());
        else { Reload(); gtk_label_set_text(GTK_LABEL(form->status),"已保存。回到目标输入框，按 Ctrl+Shift+B 开始使用。"); }
    })),&form);
    g_signal_connect(form.window,"destroy",G_CALLBACK((+[](GtkWidget*,gpointer){ if(gtk_main_level()) gtk_main_quit(); })),nullptr);
    gtk_widget_show_all(form.window);
    if(preview) g_timeout_add(1500,+[](gpointer data)->gboolean { gtk_widget_destroy(static_cast<Form*>(data)->window); return G_SOURCE_REMOVE; },&form);
    gtk_main();
    return 0;
}
