#include "buffer_service.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>

#include "buffer_process.hpp"
#include "engine/rime_key.hpp"

namespace fcitx {
namespace {

FCITX_DEFINE_LOG_CATEGORY(rimes_buffer_log, "rimes.buffer");

constexpr int kHoldTickUs = 50000;
constexpr int kDefaultFocusGraceMs = 5000;
constexpr int kDragHardTimeoutMs = 30000;

rimes::buffer::CaretRect CaretFromIc(InputContext* ic) {
    rimes::buffer::CaretRect caret;
    if (ic == nullptr) {
        return caret;
    }
    const auto& rect = ic->cursorRect();
    caret.x = rect.left();
    caret.y = rect.top();
    caret.width = rect.width();
    caret.height = rect.height();
    caret.valid = caret.width >= 0 && caret.height >= 0 &&
                  (caret.width > 0 || caret.height > 0 || caret.x != 0 || caret.y != 0);
    return caret;
}

int FocusGraceMsFromEnv() {
    const char* value = std::getenv("RIMES_BUFFER_FOCUS_GRACE_MS");
    if (value == nullptr || value[0] == '\0') {
        return kDefaultFocusGraceMs;
    }
    char* end = nullptr;
    const auto parsed = std::strtol(value, &end, 10);
    if (end == value || parsed < 50 || parsed > 30000) {
        return kDefaultFocusGraceMs;
    }
    return static_cast<int>(parsed);
}

std::string DefaultSocketPath() {
    if (const char* override_path = std::getenv("RIMES_BUFFER_SOCKET")) {
        if (override_path[0] != '\0') {
            return override_path;
        }
    }
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR")) {
        return std::string(runtime) + "/rimes-buffer.sock";
    }
    return "/tmp/rimes-buffer-" + std::to_string(geteuid()) + ".sock";
}

bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

int SetCloexec(int fd) {
    const int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    const int status = fcntl(fd, F_GETFL, 0);
    if (status >= 0) {
        fcntl(fd, F_SETFL, status | O_NONBLOCK);
    }
    return fd;
}

std::string FindUiBinary() {
    if (const char* override_path = std::getenv("RIMES_BUFFER_UI")) {
        if (override_path[0] != '\0') {
            return override_path;
        }
    }
    const char* candidates[] = {
        RIMES_BUFFER_UI_PATH,
        "/usr/libexec/rimes/rimes-buffer",
        "/usr/lib/rimes/rimes-buffer",
        "/usr/bin/rimes-buffer",
        "/usr/local/bin/rimes-buffer",
        nullptr,
    };
    for (const char* const* path = candidates; *path != nullptr; ++path) {
        if (access(*path, X_OK) == 0) {
            return *path;
        }
    }
    if (const char* path_env = std::getenv("PATH")) {
        std::string remaining = path_env;
        while (!remaining.empty()) {
            const auto split = remaining.find(':');
            const std::string dir = remaining.substr(0, split);
            const std::string candidate = dir + "/rimes-buffer";
            if (access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
            if (split == std::string::npos) {
                break;
            }
            remaining = remaining.substr(split + 1);
        }
    }
    return {};
}

}  // namespace

BufferService::BufferService(Instance* instance, PostFn post)
    : instance_(instance),
      post_(std::move(post)),
      socket_path_(DefaultSocketPath()),
      dump_path_(std::getenv("RIMES_BUFFER_DUMP") ? std::getenv("RIMES_BUFFER_DUMP") : ""),
      focus_grace_ms_(FocusGraceMsFromEnv()),
      auto_capture_(EnvFlag("RIMES_BUFFER_AUTO_CAPTURE")),
      headless_(EnvFlag("RIMES_BUFFER_HEADLESS")) {
    if (const char* close_after = std::getenv("RIMES_BUFFER_CLOSE_AFTER_LAST")) {
        model_.set_close_after_last(std::strcmp(close_after, "0") != 0);
    }
    StartSocket();
    Publish();
}

BufferService::~BufferService() {
    ai_alive_->store(false);
    CancelAi();
    CancelHoldTimer();
    CancelFocusGraceTimer();
    CancelDragHardTimer();
    ui_respawn_timer_.reset();
    StopSocket();
    ReapUi();
}

std::string BufferService::TokenFor(InputContext* ic) {
    if (ic == nullptr) {
        return {};
    }
    return std::to_string(reinterpret_cast<std::uintptr_t>(ic));
}

bool BufferService::IsToggleHotkey(const KeyEvent& event) {
    if (event.isRelease()) {
        return false;
    }
    const auto& key = event.rawKey();
    const bool shift = key.states().test(KeyState::Shift);
    const bool ctrl = key.states().test(KeyState::Ctrl);
    const bool super = key.states().test(KeyState::Super);
    const bool letter_b = key.check(FcitxKey_B) || key.check(FcitxKey_b) ||
                          key.sym() == FcitxKey_B || key.sym() == FcitxKey_b;
    return letter_b && shift && (ctrl || super) && !(ctrl && super);
}

bool BufferService::IsReturnKey(const Key& key) {
    return key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter);
}

bool BufferService::IsBackspaceKey(const Key& key) {
    return key.check(FcitxKey_BackSpace);
}

bool BufferService::IsPrintableAscii(const KeyEvent& event, char* out) {
    if (event.isRelease() || out == nullptr) {
        return false;
    }
    const auto states = event.rawKey().states();
    if (states.test(KeyState::Ctrl) || states.test(KeyState::Alt) ||
        states.test(KeyState::Super)) {
        return false;
    }
    const auto sym = static_cast<std::uint32_t>(event.key().sym());
    if (sym < 0x20 || sym > 0x7e) {
        return false;
    }
    *out = static_cast<char>(sym);
    return true;
}

InputContext* BufferService::LiveTarget() const {
    InputContext* found = nullptr;
    instance_->inputContextManager().foreachFocused([&](InputContext* ic) {
        if (model_.captures(TokenFor(ic)) && !ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive)) {
            found = ic;
            return false;
        }
        return true;
    });
    return found;
}

void BufferService::RefreshCaret(InputContext* ic) {
    if (ic == nullptr) {
        return;
    }
    model_.set_caret(CaretFromIc(ic));
    if (!ic->program().empty()) {
        model_.set_target_name(ic->program());
        target_token_ = TokenFor(ic);
    }
}

void BufferService::EndDrag() {
    // Once release is reported, another activation may be a different field
    // in Firefox's shared IC. Never retain a time window that consumes it.
    dragging_ = false;
    CancelDragHardTimer();
}

void BufferService::ConsumeDragBlip() {
    dragging_ = false;
    CancelDragHardTimer();
}

void BufferService::ArmDragHardTimer() {
    if (instance_ == nullptr) {
        return;
    }
    const uint64_t delay_us = static_cast<uint64_t>(kDragHardTimeoutMs) * 1000;
    drag_hard_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + delay_us, 0,
        [this](EventSourceTime*, uint64_t) {
            if (dragging_) {
                EndDrag();
            }
            return true;
        });
}

void BufferService::CancelDragHardTimer() {
    drag_hard_timer_.reset();
}

void BufferService::ClearClientPreedit(InputContext* ic) {
    if (ic == nullptr) {
        return;
    }
    ic->inputPanel().setClientPreedit(Text());
    ic->inputPanel().setPreedit(Text());
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
}

bool BufferService::OnCommit(InputContext* ic, std::string_view text) {
    if (ic == nullptr || text.empty()) {
        return false;
    }
    if (ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive) || model_.secure() ||
        model_.password_field()) {
        return false;
    }
    if (!model_.captures(TokenFor(ic))) {
        return false;
    }
    model_.finish_direct_run();
    model_.append(text, rimes::buffer::Origin::Rime);
    model_.set_preedit({});
    raw_input_.clear();
    if (!model_.visible()) {
        model_.set_visible(true);
    }
    RefreshCaret(ic);
    EnsureUi();
    Publish();
    FCITX_LOGC(rimes_buffer_log, Info) << "commit captured blocks=" << model_.pending_count();
    return true;
}

bool BufferService::HandleEarlyKey(KeyEvent& event, bool composing) {
    auto* ic = event.inputContext();
    if (ic == nullptr) {
        return false;
    }
    const auto physical=event.rawKey().code() ? Key(FcitxKey_None,KeyStates{},event.rawKey().code()) : Key(event.rawKey().sym());
    if(accept_latch_.isValid()) {
        if(accept_latch_target_!=TokenFor(ic) || !ic->hasFocus() || ai_.mode()!=rimes::ai::Mode::Generate || !model_.captures(TokenFor(ic)) || model_.secure() || model_.password_field() || ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive)) { accept_latch_=Key(); accept_latch_target_.clear(); }
        else if(physical==accept_latch_) {
            if(event.isRelease()) { accept_latch_=Key(); accept_latch_target_.clear(); }
            event.filterAndAccept(); return true;
        }
    }
    const bool repeat=event.rawKey().states().test(KeyState::Repeat);
    if(!event.isRelease() && !repeat && !composing && ai_.mode()==rimes::ai::Mode::Generate && !ai_config_.accept_key.empty() && event.key().normalize().check(Key(ai_config_.accept_key).normalize()) && !ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive) && AcceptSuggestion()) {
        accept_latch_=physical; accept_latch_target_=TokenFor(ic); event.filterAndAccept(); return true;
    }
    if (IsToggleHotkey(event)) {
        Toggle(ic);
        event.filterAndAccept();
        return true;
    }

    if ((IsReturnKey(event.rawKey()) || IsReturnKey(event.key())) &&
        eat_return_until_release_) {
        if (event.isRelease()) {
            eat_return_until_release_ = false;
            ApplyReturnAction(gesture_.on_release(std::chrono::steady_clock::now()), ic,
                              composing);
            CancelHoldTimer();
        }
        event.filterAndAccept();
        return true;
    }

    const bool password = ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive);
    OnPasswordField(ic, password);
    if (password || model_.secure()) {
        return false;
    }

    if (event.key().check(FcitxKey_Escape) && !event.isRelease() && !composing &&
        model_.captures(TokenFor(ic))) {
        CloseAndPause();
        event.filterAndAccept();
        return true;
    }

    if (!model_.captures(TokenFor(ic))) {
        return false;
    }

    if (IsReturnKey(event.rawKey()) || IsReturnKey(event.key())) {
        const auto now = std::chrono::steady_clock::now();
        const bool is_repeat = event.rawKey().states().test(KeyState::Repeat);
        if (composing) {
            if (event.isRelease()) {
                gesture_.on_release(now);
            } else if (!is_repeat) {
                eat_return_until_release_ = true;
                gesture_.on_press(true, is_repeat, now);
            }
            // Let Rime settle this physical Return into a commit. The same
            // press must not send; on_press(settle) keeps the hold disarmed.
            return false;
        }
        if (event.isRelease()) {
            eat_return_until_release_ = false;
            ApplyReturnAction(gesture_.on_release(now), ic, composing);
        } else {
            if (!is_repeat) {
                eat_return_until_release_ = true;
            }
            ApplyReturnAction(gesture_.on_press(false, is_repeat, now), ic, composing);
            if (!is_repeat && !gesture_.settle_only()) {
                ArmHoldTimer();
            }
        }
        event.filterAndAccept();
        return true;
    }

    if ((IsBackspaceKey(event.rawKey()) || IsBackspaceKey(event.key())) &&
        !event.isRelease()) {
        if (composing) {
            return false;
        }
        if (!model_.delete_backward_direct(TokenFor(ic))) {
            model_.remove_last_block();
        }
        Publish();
        event.filterAndAccept();
        return true;
    }

    const auto& key = event.rawKey();
    if (!event.isRelease() &&
        (key.check(FcitxKey_A, KeyState::Ctrl) || key.check(FcitxKey_a, KeyState::Ctrl))) {
        model_.select_all();
        Publish();
        event.filterAndAccept();
        return true;
    }
    if (!event.isRelease() &&
        (key.check(FcitxKey_V, KeyState::Ctrl) || key.check(FcitxKey_v, KeyState::Ctrl))) {
        WriteAll(R"({"v":1,"type":"request_clipboard"})");
        event.filterAndAccept();
        return true;
    }

    return false;
}

void BufferService::AfterRime(InputContext* ic, KeyEvent& event, bool rime_handled,
                              bool composing, std::string_view preedit,
                              std::string_view raw_input) {
    if (ic == nullptr) {
        return;
    }
    if (!model_.captures(TokenFor(ic))) {
        if (model_.preedit() != preedit && !model_.capture_enabled()) {
            model_.set_preedit({});
            raw_input_.clear();
        }
        return;
    }
    model_.set_preedit(std::string(preedit));
    raw_input_ = std::string(raw_input);
    RefreshCaret(ic);
    if (!rime_handled && !event.isRelease() && !composing) {
        char printable = 0;
        if (IsPrintableAscii(event, &printable)) {
            const char text[] = {printable, '\0'};
            model_.append_direct_fragment(text, TokenFor(ic));
            event.filterAndAccept();
        }
    }
    if (rime_handled && !preedit.empty()) {
        model_.finish_direct_run();
    }
    Publish();
}

void BufferService::OnActivate(InputContext* ic) {
    if (ic == nullptr) {
        return;
    }
    const auto token = TokenFor(ic);
    FCITX_LOGC(rimes_buffer_log, Info)
        << "activate token=" << token << " capturing=" << model_.capture_enabled()
        << " auto=" << auto_capture_ << " drag=" << dragging_
        << " pending=" << pending_unfocus_token_;
    const bool same_ic = (!pending_unfocus_token_.empty() && pending_unfocus_token_ == token) ||
                         model_.captures(token);
    if (same_ic) {
        // Firefox/Chromium keep one IC per window. Keep capture only for an
        // explicit, still-active toolbar drag. Consume one WM activation;
        // after receiving drag_end every activation is a field switch.
        // Physical release can precede that command (GTK polls every 50ms).
        pending_unfocus_token_.clear();
        CancelFocusGraceTimer();
        if (dragging_) {
            ConsumeDragBlip();
            RefreshCaret(ic);
            OnPasswordField(ic, ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive));
            Publish();
            return;
        }
        DropCaptureForSwitch("same-ic-reactivate");
    }
    if (model_.capture_enabled() && !model_.captures(token)) {
        DropCaptureForSwitch("focus-changed");
    }
    OnPasswordField(ic, ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive));
    if (auto_capture_ && !ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive)) {
        ShowAndCapture(ic);
    }
    Publish();
}

void BufferService::OnDeactivate(InputContext* ic, bool switching_im) {
    accept_latch_=Key(); accept_latch_target_.clear();
    CancelAi();
    FCITX_LOGC(rimes_buffer_log, Info)
        << "deactivate switch_im=" << switching_im << " capturing=" << model_.capture_enabled()
        << " drag=" << dragging_;
    if (switching_im && model_.captures(TokenFor(ic))) {
        CloseAndPause();
        return;
    }
    ClearClientPreedit(ic);
    if (!model_.captures(TokenFor(ic))) {
        return;
    }
    if (dragging_) {
        Publish();
        return;
    }
    // Do not drop capture yet. A toolbar drag or a brief WM grab will
    // activate this same IC again; a real field switch activates another.
    pending_unfocus_token_ = TokenFor(ic);
    ArmFocusGraceTimer();
    Publish();
}

void BufferService::OnInputContextDestroyed(InputContext* ic) {
    if (ic == nullptr) {
        return;
    }
    const auto token = TokenFor(ic);
    FCITX_LOGC(rimes_buffer_log, Info) << "ic destroyed token=" << token;
    const bool ours = model_.captures(token) || pending_unfocus_token_ == token ||
                      target_token_ == token;
    if (!ours) {
        return;
    }
    if (model_.captures(token) || pending_unfocus_token_ == token) {
        DropCaptureForSwitch("ic-destroyed");
    }
    if (target_token_ == token) {
        model_.set_target_name({});
        target_token_.clear();
    }
    Publish();
}

void BufferService::OnPasswordField(InputContext* ic, bool password) {
    (void)ic;
    if (password) {
        model_.set_password_field(true);
        if (model_.capture_enabled()) {
            model_.route_direct_preserving_content("password");
        }
        if (model_.visible()) {
            model_.set_visible(false);
        }
        Publish();
        return;
    }
    if (model_.password_field()) {
        model_.set_password_field(false);
        Publish();
    }
}

void BufferService::Toggle(InputContext* ic) {
    FCITX_LOGC(rimes_buffer_log, Info)
        << "toggle visible=" << model_.visible()
        << " capturing=" << model_.capture_enabled()
        << " token=" << model_.capture_token();
    if (model_.visible() && model_.capture_enabled()) {
        CloseAndPause();
        return;
    }
    ShowAndCapture(ic);
}

void BufferService::ShowAndCapture(InputContext* ic) {
    if (ic == nullptr) {
        instance_->inputContextManager().foreachFocused([&](InputContext* focused) {
            ic = focused;
            return false;
        });
    }
    if (ic == nullptr) {
        model_.set_visible(true);
        model_.resume_workbench_processing();
        EnsureUi();
        Publish();
        return;
    }
    if (ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive)) {
        return;
    }
    RefreshCaret(ic);
    model_.activate_capture(TokenFor(ic));
    model_.set_visible(true);
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
    EnsureUi();
    Publish();
}

void BufferService::CloseAndPause() {
    gesture_.cancel();
    CancelHoldTimer();
    CancelFocusGraceTimer();
    dragging_ = false;
    CancelDragHardTimer();
    pending_unfocus_token_.clear();
    raw_input_.clear();
    ClearClientPreedit(LiveTarget());
    model_.pause_capture_preserving_content();
    Publish();
}

void BufferService::StageOpenPreedit() {
    auto text = raw_input_;
    if (text.empty()) {
        text = model_.preedit();
    }
    raw_input_.clear();
    if (text.empty()) {
        return;
    }
    model_.append(text, rimes::buffer::Origin::Local);
    model_.set_preedit({});
}

void BufferService::DropCaptureForSwitch(std::string_view reason) {
    StageOpenPreedit();
    gesture_.cancel();
    CancelHoldTimer();
    CancelFocusGraceTimer();
    dragging_ = false;
    CancelDragHardTimer();
    pending_unfocus_token_.clear();
    model_.route_direct_preserving_content(reason);
}

void BufferService::ArmFocusGraceTimer() {
    if (instance_ == nullptr) {
        return;
    }
    const uint64_t delay_us = static_cast<uint64_t>(focus_grace_ms_) * 1000;
    focus_grace_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + delay_us, 0,
        [this](EventSourceTime*, uint64_t) {
            if (!pending_unfocus_token_.empty()) {
                DropCaptureForSwitch("focus-out");
                Publish();
            }
            return true;
        });
}

void BufferService::CancelFocusGraceTimer() {
    focus_grace_timer_.reset();
}

bool BufferService::SendNext() { return Deliver(false); }

bool BufferService::SendAll() { return Deliver(true); }

void BufferService::ApplyReturnAction(rimes::buffer::ReturnGesture::Action action,
                                      InputContext* ic, bool composing) {
    (void)composing;
    switch (action) {
        case rimes::buffer::ReturnGesture::Action::SettleOnly:
            // The caller still needs Rime to see Return so composition becomes
            // a commit. RimesState processes that after we return false from
            // a dedicated settle path. Here we only mark the gesture.
            break;
        case rimes::buffer::ReturnGesture::Action::ArmHold:
            model_.set_hold_progress(0);
            Publish();
            break;
        case rimes::buffer::ReturnGesture::Action::SendNext:
            Deliver(false);
            model_.set_hold_progress(0);
            Publish();
            break;
        case rimes::buffer::ReturnGesture::Action::SendAll:
            Deliver(true);
            model_.set_hold_progress(0);
            CancelHoldTimer();
            Publish();
            break;
        case rimes::buffer::ReturnGesture::Action::Consume:
        case rimes::buffer::ReturnGesture::Action::None:
            model_.set_hold_progress(0);
            if (ic != nullptr) {
                RefreshCaret(ic);
            }
            Publish();
            break;
    }
}

bool BufferService::Deliver(bool all) {
    if (ai_.mode() == rimes::ai::Mode::Translate || (ai_.mode()==rimes::ai::Mode::Generate && manual_ai_)) return DeliverAi(all);
    auto* ic = LiveTarget();
    if (ic == nullptr) {
        FCITX_LOGC(rimes_buffer_log, Info) << "delivery rejected: no live target";
        return false;
    }
    if (ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive) || model_.secure() ||
        model_.password_field()) {
        return false;
    }
    const auto token = TokenFor(ic);
    std::vector<std::string> sent;
    const auto blocks = model_.blocks();
    for (const auto& block : blocks) {
        if (!model_.captures(token) || LiveTarget() != ic) {
            break;
        }
        ic->commitString(block.text);
        sent.push_back(block.id);
        if (!all) {
            break;
        }
    }
    if (sent.empty()) {
        return false;
    }
    model_.consume_delivered(sent);
    const bool drained = model_.empty();
    if (drained && model_.close_after_last()) {
        CloseAndPause();
    } else {
        Publish();
    }
    return true;
}

void BufferService::HandleCommand(const rimes::buffer::Command& command) {
    InputContext* ic = LiveTarget();
    if (ic == nullptr) {
        instance_->inputContextManager().foreachFocused([&](InputContext* focused) {
            ic = focused;
            return false;
        });
    }
    switch (command.op) {
        case rimes::buffer::CommandOp::AiMode: {
            rimes::ai::Mode mode;
            if (rimes::ai::ParseMode(command.text, &mode)) {
                CancelAi();
                ai_.SetMode(mode);
                manual_ai_=false;
                ai_paused_ = false;
                ai_notice_.clear();
                ReloadAiConfig();
            }
            break;
        }
        case rimes::buffer::CommandOp::AiAccept:
            if(command.result_id==std::to_string(suggestion_.revision)) AcceptSuggestion();
            break;
        case rimes::buffer::CommandOp::AiGenerate:
            manual_ai_=ai_.mode()==rimes::ai::Mode::Generate;
            ai_paused_ = false;
            StartAi(true);
            break;
        case rimes::buffer::CommandOp::AiCancel:
            CancelAi();
            ai_paused_ = true;
            ai_notice_ = "已暂停；点击生成或翻译可继续";
            break;
        case rimes::buffer::CommandOp::AiReload:
            CancelAi();
            ai_paused_=false; ai_notice_.clear();
            ReloadAiConfig();
            manual_ai_=false;
            ai_.Reset();
            WriteAll(R"({"v":1,"type":"ai_reloaded"})");
            break;
        case rimes::buffer::CommandOp::AiSend:
            if (!ai_.units().empty() && command.result_id == std::to_string(ai_.units().front().id)) DeliverAi(false);
            break;
        case rimes::buffer::CommandOp::AiCopy: {
            ai_.Sync(model_.blocks());
            const auto text = ai_.ReadyText();
            if (!model_.secure() && !model_.password_field() && !text.empty() && !ai_.units().empty() && command.result_id == std::to_string(ai_.units().front().id))
                WriteAll("{\"v\":1,\"type\":\"copy_result\",\"text\":"+rimes::buffer::JsonEscape(text)+"}");
            break;
        }
        case rimes::buffer::CommandOp::Hello:
        case rimes::buffer::CommandOp::Query:
            break;
        case rimes::buffer::CommandOp::Toggle:
            Toggle(ic);
            break;
        case rimes::buffer::CommandOp::Show:
            ShowAndCapture(ic);
            break;
        case rimes::buffer::CommandOp::Close:
            CloseAndPause();
            break;
        case rimes::buffer::CommandOp::SendNext:
            SendNext();
            break;
        case rimes::buffer::CommandOp::SendAll:
            SendAll();
            break;
        case rimes::buffer::CommandOp::RemoveLast:
            model_.remove_last_block();
            break;
        case rimes::buffer::CommandOp::SelectAll:
            model_.select_all();
            break;
        case rimes::buffer::CommandOp::Paste:
            model_.insert_pasted_text(command.text, rimes::buffer::Origin::Clipboard);
            if (!model_.visible()) {
                model_.set_visible(true);
            }
            EnsureUi();
            break;
        case rimes::buffer::CommandOp::SetInsertion:
            model_.set_insertion_point(command.insertion_index);
            break;
        case rimes::buffer::CommandOp::DragBegin:
            FCITX_LOGC(rimes_buffer_log, Info) << "toolbar drag_begin received";
            dragging_ = true;
            pending_unfocus_token_.clear();
            CancelFocusGraceTimer();
            ArmDragHardTimer();
            return;
        case rimes::buffer::CommandOp::DragEnd:
            FCITX_LOGC(rimes_buffer_log, Info) << "toolbar drag_end received";
            EndDrag();
            return;
        case rimes::buffer::CommandOp::Unknown:
            break;
    }
    Publish();
}

void BufferService::EnsureUi() {
    if (headless_ || !model_.visible()) {
        return;
    }
    ReapUi();
    if (ui_pid_ > 0) {
        return;
    }
    const auto binary = FindUiBinary();
    if (binary.empty()) {
        FCITX_LOGC(rimes_buffer_log, Info) << "rimes-buffer UI is not installed";
        return;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        execl(binary.c_str(), "rimes-buffer", "--socket", socket_path_.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    ui_pid_ = pid;
}

void BufferService::ReapUi() {
    if (ui_pid_ <= 0) {
        return;
    }
    if (rimes::buffer::UiProcessGone(ui_pid_)) {
        ui_pid_ = 0;
    }
}

void BufferService::ReplaceDroppedUi() {
    if (!rimes::buffer::ShouldForceUiRespawn(ui_pid_, dropped_ui_pid_)) {
        return;
    }
    rimes::buffer::DiscardUiProcess(ui_pid_);
    ui_pid_ = 0;
}

int BufferService::UiClientCount() {
    std::lock_guard<std::recursive_mutex> lock(clients_mu_);
    return static_cast<int>(clients_.size());
}

void BufferService::StartSocket() {
    unlink(socket_path_.c_str());
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        FCITX_LOGC(rimes_buffer_log, Error) << "buffer socket() failed";
        return;
    }
    SetCloexec(listen_fd_);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(address.sun_path)) {
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    std::strncpy(address.sun_path, socket_path_.c_str(), sizeof(address.sun_path) - 1);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listen_fd_, 8) != 0) {
        FCITX_LOGC(rimes_buffer_log, Error) << "buffer bind/listen failed: " << socket_path_;
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    chmod(socket_path_.c_str(), 0600);
    running_.store(true);
    socket_thread_ = std::thread([this] { SocketLoop(); });
}

void BufferService::StopSocket() {
    running_.store(false);
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
    }
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        for (auto& client : clients_) {
            if (client.fd >= 0) {
                shutdown(client.fd, SHUT_RDWR);
            }
        }
    }
    if (socket_thread_.joinable()) {
        socket_thread_.join();
    }
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        for (auto& client : clients_) {
            if (client.fd >= 0) {
                close(client.fd);
            }
        }
        clients_.clear();
    }
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    unlink(socket_path_.c_str());
}

void BufferService::SocketLoop() {
    while (running_.load()) {
        std::vector<pollfd> fds;
        {
            std::lock_guard<std::recursive_mutex> lock(clients_mu_);
            if (listen_fd_ >= 0) {
                fds.push_back(pollfd{listen_fd_, POLLIN, 0});
            }
            for (const auto& client : clients_) {
                fds.push_back(pollfd{client.fd, POLLIN, 0});
            }
        }
        if (fds.empty()) {
            break;
        }
        const int ready = poll(fds.data(), fds.size(), 250);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }
        if (fds[0].fd == listen_fd_ && (fds[0].revents & POLLIN) != 0) {
            AcceptClient();
        }
        std::vector<int> readable;
        {
            std::lock_guard<std::recursive_mutex> lock(clients_mu_);
            for (const auto& item : fds) {
                if (item.fd == listen_fd_) {
                    continue;
                }
                if ((item.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                    readable.push_back(item.fd);
                }
            }
        }
        for (int fd : readable) {
            std::lock_guard<std::recursive_mutex> lock(clients_mu_);
            for (auto& client : clients_) {
                if (client.fd == fd) {
                    ReadClient(&client);
                    break;
                }
            }
        }
    }
}

void BufferService::AcceptClient() {
    const int fd = accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
        return;
    }
    SetCloexec(fd);
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        clients_.push_back(Client{fd, {}});
    }
    post_([this] { Publish(); });
}

void BufferService::ReadClient(Client* client) {
    char chunk[4096];
    const auto got = read(client->fd, chunk, sizeof(chunk));
    if (got <= 0) {
        CloseClient(client->fd);
        return;
    }
    client->incoming.append(chunk, static_cast<std::size_t>(got));
    while (client->incoming.size() >= 4) {
        std::uint32_t length = 0;
        if (!rimes::buffer::DecodeFrameHeader(client->incoming.data(), &length)) {
            CloseClient(client->fd);
            return;
        }
        if (client->incoming.size() < 4 + length) {
            return;
        }
        const std::string payload = client->incoming.substr(4, length);
        client->incoming.erase(0, 4 + length);
        rimes::buffer::Command command;
        std::string error;
        if (!rimes::buffer::ParseCommand(payload, &command, &error)) {
            WriteAll(rimes::buffer::EncodeError("bad_command", error));
            continue;
        }
        post_([this, command] { HandleCommand(command); });
    }
}

void BufferService::CloseClient(int fd) {
    bool clients_empty = false;
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        for (auto iterator = clients_.begin(); iterator != clients_.end(); ++iterator) {
            if (iterator->fd == fd) {
                close(fd);
                clients_.erase(iterator);
                clients_empty = clients_.empty();
                break;
            }
        }
    }
    if (clients_empty && !headless_) {
        post_([this] { ScheduleUiRespawn(); });
    }
}

bool BufferService::HasUiClients() {
    std::lock_guard<std::recursive_mutex> lock(clients_mu_);
    return !clients_.empty();
}

void BufferService::ScheduleUiRespawn() {
    if (headless_ || !model_.visible()) {
        return;
    }
    dropped_ui_pid_ = ui_pid_;
    ui_respawn_attempt_ = 0;
    ArmUiRespawnTimer(50000);
}

void BufferService::ArmUiRespawnTimer(int delay_us) {
    if (instance_ == nullptr) {
        return;
    }
    ui_respawn_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + delay_us, 0,
        [this](EventSourceTime*, uint64_t) {
            ReapUi();
            // Only replace the pid that dropped the socket. A child this
            // retry already started gets several seconds to connect.
            ReplaceDroppedUi();
            ++ui_respawn_attempt_;
            if (model_.visible()) {
                EnsureUi();
            }
            if (model_.visible() && !HasUiClients() && ui_respawn_attempt_ < 5) {
                const int delays[] = {2000000, 2000000, 2000000, 2000000};
                const int index = std::min(ui_respawn_attempt_ - 1, 3);
                ArmUiRespawnTimer(delays[index]);
            }
            return true;
        });
}

void BufferService::WriteAll(const std::string& payload) {
    std::string frame;
    if (!rimes::buffer::EncodeFrame(payload, &frame)) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(clients_mu_);
    for (auto iterator = clients_.begin(); iterator != clients_.end();) {
        const auto written = send(iterator->fd, frame.data(), frame.size(), MSG_NOSIGNAL);
        if (written != static_cast<ssize_t>(frame.size())) {
            close(iterator->fd);
            iterator = clients_.erase(iterator);
            continue;
        }
        ++iterator;
    }
}

void BufferService::Publish() {
    SyncAi();
    if (model_.visible()) {
        EnsureUi();
    }
    auto snapshot = rimes::buffer::MakeSnapshot(model_);
    AddAiSnapshot(&snapshot);
    snapshot.ui_clients = UiClientCount();
    const auto json = rimes::buffer::EncodeSnapshot(snapshot);
    WriteAll(json);
    if (dump_path_.empty()) {
        return;
    }
    const auto tmp = dump_path_ + ".tmp";
    FILE* file = std::fopen(tmp.c_str(), "w");
    if (file == nullptr) {
        return;
    }
    std::fwrite(json.data(), 1, json.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
    std::rename(tmp.c_str(), dump_path_.c_str());
}

void BufferService::CancelAi() {
    ++ai_epoch_;
    suggestion_.Invalidate();
    ai_preview_.clear();
    ai_worker_.Cancel();
    ai_.CancelWorking();
    ai_active_id_ = 0;
    ai_timer_id_ = 0;
    ai_timer_.reset();
}

void BufferService::ReloadAiConfig() {
    ai_worker_.SetModelActive(ai_config_,false);
    std::string error;
    if(!rimes::ai::LoadConfig(rimes::ai::ConfigPath(),&ai_config_,&error)) { ai_paused_=true; ai_notice_=error; }
}
void BufferService::SyncAi() {
    const bool valid=model_.visible() && LiveTarget() && pending_unfocus_token_.empty() && !model_.secure() && !model_.password_field();
    const bool advanced=valid && ai_.mode()!=rimes::ai::Mode::Default && !ai_paused_;
    ai_worker_.SetModelActive(ai_config_,advanced);
    if(advanced && ai_config_.channel==rimes::ai::Channel::ManagedLocal) {
        if(!model_status_timer_) model_status_timer_=instance_->eventLoop().addTimeEvent(CLOCK_MONOTONIC,now(CLOCK_MONOTONIC)+100000,1000,
            [this](EventSourceTime* source,uint64_t) {
                const auto status=ai_worker_.ModelStatus();
                if(status!=model_status_seen_) { model_status_seen_=status; Publish(); }
                source->setTime(now(CLOCK_MONOTONIC)+200000); source->setOneShot(); return true;
            });
    } else model_status_timer_.reset();
    if(!valid) { if(ai_target_valid_) { CancelAi(); ai_.Reset(); manual_ai_=false; } ai_target_valid_=false; return; }
    ai_target_valid_=true;
    if(ai_.mode()==rimes::ai::Mode::Default) return;
    const auto old_units=ai_.units().empty() ? 0 : ai_.units().front().id;
    ai_.Sync(model_.blocks());
    if(manual_ai_ && old_units && (ai_.units().empty() || ai_.units().front().id!=old_units)) manual_ai_=false;
    if(suggestion_.Sync(model_,model_.capture_token())) {
        CancelAi(); suggestion_.Sync(model_,model_.capture_token());
    }
    if(ai_active_id_ && !ai_.Find(ai_active_id_)) CancelAi();
    if(!model_.preedit().empty()) { if(ai_active_id_ || suggestion_.working || ai_timer_) CancelAi(); return; }
    if(ai_paused_ || ai_active_id_ || suggestion_.working || model_.all_content_selected()) return;
    if(ai_.mode()==rimes::ai::Mode::Generate) {
        if(manual_ai_ || suggestion_.attempted || suggestion_.context.empty()) return;
        if(ai_timer_ && ai_timer_id_==suggestion_.revision) return;
        ai_timer_id_=suggestion_.revision;
        const int delay=rimes::ai::SentenceEnd(suggestion_.context) ? 0 : ai_config_.debounce_ms;
        ai_timer_=instance_->eventLoop().addTimeEvent(CLOCK_MONOTONIC,now(CLOCK_MONOTONIC)+delay*1000,1000,
            [this](EventSourceTime*,uint64_t) { ai_timer_id_=0; StartSuggestion(); Publish(); return true; });
    } else {
        const auto* pending=ai_.NextPending();
        if(!pending) { ai_timer_.reset(); ai_timer_id_=0; return; }
        if(ai_timer_ && ai_timer_id_==pending->id) return;
        ai_timer_id_=pending->id;
        const int delay=rimes::ai::SentenceEnd(pending->text) ? 0 : ai_config_.debounce_ms;
        ai_timer_=instance_->eventLoop().addTimeEvent(CLOCK_MONOTONIC,now(CLOCK_MONOTONIC)+delay*1000,1000,
            [this](EventSourceTime*,uint64_t) { ai_timer_id_=0; StartAi(); Publish(); return true; });
    }
}
void BufferService::StartSuggestion() {
    ai_timer_.reset(); ai_timer_id_=0;
    if(ai_.mode()!=rimes::ai::Mode::Generate || manual_ai_ || ai_paused_ || !LiveTarget() || !model_.visible() || !model_.preedit().empty() || model_.all_content_selected() || model_.secure() || model_.password_field() || !pending_unfocus_token_.empty()) return;
    suggestion_.Sync(model_,model_.capture_token());
    if(suggestion_.context.empty()) return;
    suggestion_.working=true; suggestion_.attempted=true;
    const auto epoch=++ai_epoch_, revision=suggestion_.revision;
    const auto token=model_.capture_token();
    auto valid=[this,alive=ai_alive_,epoch,revision,token] { return alive->load() && epoch==ai_epoch_ && revision==suggestion_.revision && LiveTarget() && pending_unfocus_token_.empty() && model_.captures(token) && !model_.secure() && !model_.password_field() && model_.preedit().empty() && suggestion_.Matches(model_,token); };
    ai_worker_.Submit(ai_config_,suggestion_.context,rimes::ai::Purpose::Suggest,
        [this,post=post_,valid,revision](rimes::ai::Reply reply) mutable { post([this,valid,revision,reply=std::move(reply)]() mutable {
            if(!valid()) return;
            suggestion_.working=false; suggestion_.preview.clear();
            if(!reply.cancelled) { if(reply.error.empty()) suggestion_.Finish(revision,reply.text); else suggestion_.error=std::move(reply.error); }
            Publish();
        }); },
        [this,post=post_,valid,context=suggestion_.context](std::string content) mutable { auto preview=rimes::ai::SuggestionPreview(content,context); if(preview.empty()) return; post([this,valid,preview=std::move(preview)] { if(valid()) { suggestion_.preview=preview; Publish(); } }); });
}
bool BufferService::AcceptSuggestion() {
    auto* ic=LiveTarget();
    if(ai_.mode()!=rimes::ai::Mode::Generate || !ic || !model_.visible() || !model_.captures(TokenFor(ic)) || !pending_unfocus_token_.empty() || model_.secure() || model_.password_field() || ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive) || !model_.preedit().empty() || !suggestion_.complete || suggestion_.working || !suggestion_.Matches(model_,model_.capture_token())) return false;
    if(!model_.replace_range(suggestion_.source,suggestion_.insertion,suggestion_.begin,suggestion_.end,suggestion_.text)) return false;
    CancelAi(); ai_.Reset(); manual_ai_=false; Publish(); return true;
}

void BufferService::StartAi(bool retry) {
    if (ai_.mode() == rimes::ai::Mode::Default) return;
    if (!LiveTarget() || !model_.visible() || model_.secure() || model_.password_field() || !pending_unfocus_token_.empty()) { ai_notice_="请先在目标输入框按 Ctrl+Shift+B 连接 Buffer"; return; }
    if (!model_.preedit().empty()) { ai_notice_="请先完成当前组字"; return; }
    ai_.Sync(model_.blocks());
    if (retry) {
        CancelAi();
        if (ai_.mode() == rimes::ai::Mode::Generate) { ai_.Reset(); ai_.Sync(model_.blocks()); }
        ai_.Retry();
    }
    ai_worker_.SetModelActive(ai_config_,true);
    if (ai_active_id_) return;
    const auto* pending=ai_.NextPending();
    if (!pending) { if(ai_.units().empty()) ai_notice_="先输入或导入需要处理的文字"; return; }
    const auto id=pending->id;
    rimes::ai::Config config=ai_config_;
    std::string error;
    if (!rimes::ai::ValidateConfig(config, &error)) {
        ai_.Find(id)->state=rimes::ai::State::Working;
        ai_.Complete(id,{},error);
        ai_notice_.clear();
        return;
    }
    ai_timer_.reset(); ai_timer_id_=0;
    ai_notice_.clear();
    ai_.Find(id)->state=rimes::ai::State::Working;
    ai_active_id_=id;
    const auto epoch=++ai_epoch_;
    const auto token=model_.capture_token();
    const bool translation=ai_.mode()==rimes::ai::Mode::Translate;
    ai_worker_.Submit(std::move(config),pending->text,translation,
        [this, alive=ai_alive_, post=post_, epoch,id,token](rimes::ai::Reply reply) mutable {
            post([this,alive,epoch,id,token,reply=std::move(reply)]() mutable {
                if (!alive->load() || epoch!=ai_epoch_) return;
                ai_active_id_=0;
                ai_.Sync(model_.blocks());
                if (!reply.cancelled && LiveTarget() && pending_unfocus_token_.empty() && model_.captures(token) && !model_.secure() && !model_.password_field())
                    ai_.Complete(id,std::move(reply.text),std::move(reply.error));
                else ai_.CancelWorking();
                Publish();
            });
        });
}

bool BufferService::DeliverAi(bool all) {
    ai_.Sync(model_.blocks());
    auto* ic=LiveTarget();
    if (!ic || !model_.visible() || model_.secure() || model_.password_field() || ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive) || !model_.preedit().empty() || !pending_unfocus_token_.empty()) return false;
    const auto token=model_.capture_token();
    bool delivered=false;
    while (!ai_.units().empty()) {
        const auto unit=ai_.units().front();
        if (unit.state!=rimes::ai::State::Ready || unit.result.empty() || !model_.captures(token) || LiveTarget()!=ic) break;
        std::vector<std::string> ids;
        for (const auto& source : unit.source) ids.push_back(source.id);
        ic->commitString(unit.result);
        ai_.Consume(unit.id);
        model_.consume_delivered(ids);
        delivered=true;
        if (!all) break;
    }
    if (delivered && model_.empty() && model_.close_after_last()) CloseAndPause();
    else Publish();
    return delivered;
}

void BufferService::AddAiSnapshot(rimes::buffer::Snapshot* snapshot) {
    snapshot->ai_mode=rimes::ai::ModeTag(ai_.mode());
    if (snapshot->secure || ai_.mode()==rimes::ai::Mode::Default) return;
    snapshot->ai_busy=ai_active_id_!=0 || suggestion_.working;
    snapshot->model_status=ai_worker_.ModelStatus();
    snapshot->source_can_send=ai_.mode()!=rimes::ai::Mode::Translate && !manual_ai_ && snapshot->capturing && !model_.empty();
    if(ai_.mode()==rimes::ai::Mode::Generate && !manual_ai_) {
        snapshot->suggestion_id=std::to_string(suggestion_.revision);
        snapshot->suggestion_kind=suggestion_.complete ? suggestion_.kind : "preview";
        snapshot->suggestion_text=suggestion_.complete ? suggestion_.kind=="correction" ? model_.staged_text().substr(suggestion_.begin,suggestion_.end-suggestion_.begin)+" → "+(suggestion_.text.empty() ? "（删除）" : suggestion_.text) : suggestion_.text : suggestion_.preview;
        snapshot->suggestion_ready=suggestion_.complete && suggestion_.Matches(model_,model_.capture_token()) && model_.preedit().empty() && LiveTarget() && pending_unfocus_token_.empty();
        snapshot->accept_key=ai_config_.accept_key;
    }
    snapshot->ai_status=ai_notice_;
    if (!ai_.units().empty()) {
        snapshot->ai_result_id=std::to_string(ai_.units().front().id);
        for (const auto& unit : ai_.units()) {
            if (unit.state==rimes::ai::State::Ready) snapshot->ai_result+=unit.result;
            if (unit.state==rimes::ai::State::Error && snapshot->ai_status.empty()) snapshot->ai_status=unit.error;
        }
        snapshot->ai_can_send=ai_.units().front().state==rimes::ai::State::Ready && snapshot->capturing && LiveTarget() && pending_unfocus_token_.empty() && model_.preedit().empty();
        snapshot->ai_can_copy=!ai_.ReadyText().empty();
    }
    if (snapshot->ai_status.empty()) snapshot->ai_status=snapshot->ai_busy ? "正在处理，可取消" : ai_.mode()==rimes::ai::Mode::Translate ? "按原文单元翻译；译文需手动发送" : suggestion_.error.empty() ? "边写边建议；可另行主动生成" : suggestion_.error;
}

void BufferService::ArmHoldTimer() {
    if (instance_ == nullptr) {
        return;
    }
    hold_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + kHoldTickUs, kHoldTickUs,
        [this](EventSourceTime* source, uint64_t) {
            const auto now_tp = std::chrono::steady_clock::now();
            const auto action = gesture_.on_tick(now_tp);
            model_.set_hold_progress(gesture_.progress(now_tp));
            if (action == rimes::buffer::ReturnGesture::Action::SendAll) {
                Deliver(true);
                model_.set_hold_progress(0);
                Publish();
                return true;
            }
            if (gesture_.pending() && !gesture_.settle_only()) {
                source->setTime(now(CLOCK_MONOTONIC) + kHoldTickUs);
                source->setOneShot();
                Publish();
                return true;
            }
            return true;
        });
}

void BufferService::CancelHoldTimer() {
    hold_timer_.reset();
    model_.set_hold_progress(0);
}

}  // namespace fcitx
