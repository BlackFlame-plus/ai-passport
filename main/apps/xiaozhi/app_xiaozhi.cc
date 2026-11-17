#include "apps/xiaozhi/app_xiaozhi.h"
#include "apps/xiaozhi/grok_protocol.h"
#include "apps/xiaozhi/grok_config.h"
#include "apps/xiaozhi/xiaozhi_audio.h"
#include "apps/xiaozhi/xiaozhi_ui.h"
#include "wifi_manager.h"
#include "bsp_audio.h"
#include "launcher/settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <atomic>
#include <freertos/semphr.h>
#include <cstring>
#include <string>

static const char *TAG = "app_xiaozhi";

// ---------------------------------------------------------------------------
// 架构：单所有者控制任务（xzg_ctl）
//
// 修复前的真实故障：多个任务同时操作同一份音频状态，且全程无锁 ——
//   * websocket 任务：收到云端音频 → StartPlayback()/PlayAudioFrame()
//   * 输入任务：按键 → StopPlayback()/StartRecording()
// 「AI 正在说话时按 OK 打断」会让两者并发：输入任务 free 掉 playback_buf_、
// 关掉 opus_dec_handle_ 的同时，websocket 任务正在用它们解码 → use-after-free
// → panic → 重启。已在真机复现（日志停在 "Opus decoder closed" 紧接
// "Starting audio playback" 之后，设备随即重启）。
//
// 修复后的职责划分：
//   * 只有控制任务能调用 Protocol::Start/Stop/Trigger*、AudioService 的
//     Start*/Stop*、以及所有 ui_*。
//   * websocket 回调只写事件队列 + 把 Opus 塞进环形缓冲，立即返回。
//   * 按键回调只投命令，立即返回。
//   * Protocol 实例是"永不析构"的函数内静态对象，从根上消除回调访问已销毁对象。
// ---------------------------------------------------------------------------

namespace {

using xiaozhi::EvType;
using xiaozhi::Event;
using xiaozhi::State;

constexpr int kProtoQueueDepth = 8;  // text only; critical transitions use mailbox
constexpr int kCmdQueueDepth = 8;
// 控制任务只做状态机 / UI / 小报文发送（ssl_write 约 2-3KB 栈）。
// 真正吃栈的两件事已经拆走：OTA 的 mbedTLS 握手 → xzg_ota 临时任务；
// Opus 编解码 → xzg_audio 常驻共用任务（24KB，见 xiaozhi_audio.cc 的说明）。
// MCP tool calls run here (JSON, NVS, device control). 2026-11-07 device probe:
// 1364 B left of 6144 after an in-session music search; heap-allocated per session.
constexpr size_t kControlTaskStack = 7168;
constexpr uint32_t kLoopTickMs = 20;

// 定时器（毫秒）
constexpr int64_t kConnectTimeoutMs = 12000;    // 连上后等 session.updated 的上限
constexpr int64_t kThinkingTimeoutMs = 50000;   // 弱网留出响应时间，仍防止永久卡死
constexpr int64_t kListeningTimeoutMs = 30000;
constexpr int64_t kSpeakingStallTimeoutMs = 30000; // no audio/PCM progress, not reply duration
constexpr int64_t kReconnectDelayMs = 6000;     // 断线重连退避
constexpr int64_t kWifiRetryMs = 1000;
constexpr int kRestartAfterFailures = 3;

enum class Cmd : uint8_t {
    Connect = 0,
    StartListen,
    StopListen,
    Abort,
    VolUp,
    VolDown,
    ToggleListen,
    Stop,
};

QueueHandle_t s_proto_q = nullptr;
QueueHandle_t s_cmd_q = nullptr;
StaticQueue_t s_proto_queue_storage{}, s_cmd_queue_storage{};
uint8_t s_proto_items[kProtoQueueDepth * sizeof(Event)];
uint8_t s_cmd_items[kCmdQueueDepth * sizeof(Cmd)];
StaticSemaphore_t s_stopped_storage{};
TaskHandle_t s_ctl_task = nullptr;
std::atomic<bool> s_control_running{false};
bool s_start_failed = false;
SemaphoreHandle_t s_stopped_sem = nullptr;
std::atomic<bool> s_app_active{false};
// Stop 命令的独立兜底标志:见 post_cmd() 注释(队列满时仍要能停)。
std::atomic<bool> s_stop_requested{false};
// 协议单例是否已经构造完成。构造它会写 NVS（含 SPI-Flash 擦写，调用链很深），
// 只允许在栈足够大的控制任务里发生；按键任务（3072 字节栈）绝不能触发它。
std::atomic<bool> s_protocol_ready{false};
// Separate mailbox: a full volume/key queue cannot lose a double-click's
// conversation request while OTA/TLS connection is still in progress.
std::atomic<bool> s_conversation_requested{false};

// 控制任务私有状态
State s_cur_state = State::Disconnected;
bool s_connect_pending = false;
int64_t s_connect_started_ms = 0;
int64_t s_thinking_since_ms = 0;
int64_t s_speaking_since_ms = 0;
uint32_t s_playback_progress = 0;
int64_t s_reconnect_at_ms = 0;
int64_t s_reconnect_delay_ms = kReconnectDelayMs;
int64_t s_listening_since_ms = 0;
int64_t s_next_connect_attempt_ms = 0;
bool s_audio_ready = false;
bool s_session_ready = false;
bool s_audio_recovery_required = false;
std::string s_retry_reason;
// 从用户按 OK 开始，到用户再次按 OK/异常中止为止。只有该标志为 true，TTS
// 播完后才自动回到下一轮聆听；避免用户主动打断时又被状态事件重新拉起麦克风。
bool s_conversation_active = false;
// A user's OK while disconnected resumes listening only after the new hello
// and audio reservation succeed. Background reconnects never open the mic.
bool s_listen_after_connect = false;

int s_connect_failures = 0;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

void post_cmd(Cmd c) {
    if (s_cmd_q == nullptr) return;
    // 2026-09-13 修复(A4):Stop 绝不能被丢弃。原来一律非阻塞投递,控制任务
    // 正卡在 connect_xiaozhi()(OTA/websocket,可达 20s)时队列一满 Stop 就被丢,
    // stop() 等到超时但任务还活着;而 start() 只看 s_app_active 就又建一个
    // 控制任务 → 两个任务并发操作音频 = 文件头记录过的 UAF 复发。
    // 现在:Stop 用 500ms 阻塞投递 + 独立的 s_stop_requested 标志双保险,
    // 即便投递失败,控制任务下一轮循环也会看到标志退出。
    if (c == Cmd::Stop) {
        s_stop_requested = true;
        xiaozhi::AudioService::RequestStop();
    }
    const TickType_t wait = (c == Cmd::Stop) ? pdMS_TO_TICKS(500) : 0;
    if (xQueueSend(s_cmd_q, &c, wait) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, dropped cmd=%d", (int)c);
    }
}

// 永不析构的协议实例：websocket 回调/录音任务都可能引用它，静态生命周期最安全。
xiaozhi::Protocol &proto() {
    static xiaozhi::Protocol inst;
    return inst;
}

void schedule_reconnect(int64_t delay_ms) {
    if (s_reconnect_at_ms != 0) return; // error/disconnect can describe the same failure
    if (delay_ms == kReconnectDelayMs) {
        delay_ms = s_reconnect_delay_ms;
        s_reconnect_delay_ms = delay_ms >= 30000 ? 60000 : delay_ms * 2;
    }
    s_reconnect_at_ms = now_ms() + delay_ms;
    s_connect_pending = false;
}

void clear_timers() {
    s_listening_since_ms = 0;
    s_connect_pending = false;
    s_thinking_since_ms = 0;
    s_speaking_since_ms = 0;
}

void on_state_changed(State st) {
    State old = s_cur_state;
    s_cur_state = st;
    // TTS stop can arrive with seconds of buffered speech still playing. Keep
    // the speaking face/waveform until the existing bounded drain finishes.
    if (old == State::Speaking && st != State::Speaking) {
        xiaozhi::AudioService::GetInstance().StopPlayback();
    }
    if (st == State::Listening && old != st) s_listening_since_ms = now_ms();
    if (st != State::Listening) s_listening_since_ms = 0;
    xiaozhi::ui_set_state(st);
    if (!s_retry_reason.empty() &&
        (st == State::Disconnected || st == State::Error)) {
        xiaozhi::ui_set_status_text(s_retry_reason);
    }

    if (st == State::Thinking) {
        s_thinking_since_ms = now_ms();
        s_speaking_since_ms = 0;
        // auto 模式的服务端 VAD 已完成断句。先等待录音 worker 完全退出，保证
        // 后续 TTS 不会与麦克风并行，也不需要 C3 承担 AEC/全双工的内存开销。
        xiaozhi::AudioService::GetInstance().StopRecording();
    } else if (st == State::Speaking) {
        s_thinking_since_ms = 0;
        s_speaking_since_ms = now_ms();
        s_playback_progress = xiaozhi::AudioService::GetInstance().PlaybackProgress();
        // 进入"回复中"就立刻起播。少了这一步，云端已经推下来的音频只能先堆在
        // 环形缓冲里、等 tick 兜底才发现 —— 表现就是开头丢帧 + 回复延迟约 1 秒。
        if (!xiaozhi::AudioService::GetInstance().IsPlaying()) {
            if (!s_session_ready || !xiaozhi::AudioService::GetInstance().StartPlayback()) {
                s_session_ready = false;
                s_audio_recovery_required = true;
                s_conversation_active = false;
                proto().Stop();
                s_retry_reason = "音频启动失败，自动重试";
                schedule_reconnect(kReconnectDelayMs);
            }
        }
    } else {
        s_thinking_since_ms = 0;
        s_speaking_since_ms = 0;
    }

    // 离开"回复中"：把环形缓冲里剩余的音频播完再收尾（尾音不被切掉）
    if (old == State::Speaking && st != State::Speaking) {
        // 对话模式：上一句回复已彻底播完、扬声器已静音后，再开启下一轮录音。
        // 通过命令队列执行，避免在协议事件处理栈里递归切换音频生命周期。
        if (st == State::Idle && s_conversation_active && s_session_ready &&
            proto().HandshakeDone() && proto().IsConnected()) {
            post_cmd(Cmd::StartListen);
        }
    }
}

void show_missing_key() {
    s_cur_state = State::Disconnected;
    xiaozhi::ui_set_state(State::Disconnected);
    xiaozhi::ui_set_status_text("未配置密钥");
    xiaozhi::ui_set_ai_text(
        "请连接热点 Passport-WiFi，打开 192.168.4.1，在 Grok 一栏填写 xAI API Key。");
    xiaozhi::ui_set_hint("填写后按确定重试");
    schedule_reconnect(kReconnectDelayMs);
}

void do_connect() {
    if (proto().NeedsRestart() && proto().HasClient()) {
        s_conversation_active = false;
        s_session_ready = false;
        clear_timers();
        xiaozhi::AudioService::GetInstance().StopRecording();
        xiaozhi::AudioService::GetInstance().AbortPlayback();
        if (!xiaozhi::AudioService::GetInstance().ReleaseSessionResources()) {
            s_audio_recovery_required = true;
        }
        // Only the controller may join/destroy the websocket worker.
        proto().Stop();
    }
    if (proto().IsConnected()) return;

    // 客户端对象还在 → esp_websocket_client 正按自己的 reconnect_timeout_ms 重连，
    // 先别把它拆了重建（否则两边互相打断，恢复反而更慢）。连续失败多次才重建。
    if (proto().HasClient() && s_connect_failures < kRestartAfterFailures) {
        ESP_LOGI(TAG, "WebSocket client still present, letting it auto-reconnect");
        s_next_connect_attempt_ms = now_ms() + 2000;
        return;
    }

    char key[256];
    char proxy_host[16];
    uint16_t proxy_port = 0;
    const bool have_key = grok_copy_key(key, sizeof(key));
    grok_proxy_get(proxy_host, sizeof(proxy_host), &proxy_port);
    if (!have_key) {
        std::memset(key, 0, sizeof(key));
        show_missing_key();
        return;
    }
    if (!WifiManager::GetInstance().IsConnected()) {
        std::memset(key, 0, sizeof(key));
        xiaozhi::ui_set_state(State::Disconnected);
        xiaozhi::ui_set_status_text("网络断开");
        s_next_connect_attempt_ms = now_ms() + kWifiRetryMs;
        return;
    }

    // A dead client cannot reconnect on its own. Stop it before the next TLS handshake.
    if (proto().HasClient()) proto().Stop();
    if (s_audio_recovery_required && s_audio_ready) {
        if (!xiaozhi::AudioService::GetInstance().Deinit()) {
            std::memset(key, 0, sizeof(key));
            schedule_reconnect(kReconnectDelayMs);
            return;
        }
        s_audio_ready = false;
        s_audio_recovery_required = false;
    }
    s_retry_reason.clear();

    // The resident audio worker is created here, while the heap is still one piece.
    if (!s_audio_ready) {
        if (xiaozhi::AudioService::GetInstance().Init() != ESP_OK) {
            ESP_LOGE(TAG, "Audio init failed; Grok stays unusable");
            std::memset(key, 0, sizeof(key));
            s_retry_reason = "音频启动失败，自动重试";
            xiaozhi::ui_set_status_text(s_retry_reason);
            schedule_reconnect(kReconnectDelayMs);
            return;
        }
        s_audio_ready = true;
    }

    xiaozhi::ui_set_status_text(proxy_host[0] ? "正在通过代理连接 Grok" : "正在连接 Grok");
    const bool started = proto().Start(key, proxy_host, proxy_port);
    std::memset(key, 0, sizeof(key));
    if (!started) {
        s_retry_reason = "无法连接服务器";
        xiaozhi::ui_set_status_text(s_retry_reason);
        s_connect_failures++;
        schedule_reconnect(kReconnectDelayMs);
        return;
    }
    s_connect_pending = true;
    s_connect_started_ms = now_ms();
}

void do_start_listen() {
    if (s_stop_requested || !s_app_active) return;
    if (proto().NeedsRestart() || !proto().IsConnected()) {
        post_cmd(Cmd::Connect);
        return;
    }
    if (!s_session_ready || !proto().HandshakeDone()) {
        xiaozhi::ui_set_status_text("连接准备中，请稍候");
        return;
    }
    // 用协议层的实时状态判断，而不是本任务缓存的 s_cur_state（后者可能滞后一个循环）
    const State st = proto().GetState();
    if (st == State::Listening || st == State::Thinking) return;

    xiaozhi::ui_clear_text();

    if (!proto().TriggerStartListen()) {
        xiaozhi::ui_set_status_text("发送失败，请重试");
        return;
    }
    s_conversation_active = true;
    s_listening_since_ms = now_ms();

    // 录音任务只负责采集+编码，帧由本任务（drain_audio_tx）负责发送
    if (!xiaozhi::AudioService::GetInstance().StartRecording()) {
        // 服务端已收到 listen/start，必须回一个 abort 收尾，否则会话会悬着
        ESP_LOGE(TAG, "StartRecording failed; aborting session");
        s_conversation_active = false;
        s_session_ready = false;
        s_audio_recovery_required = true;
        proto().TriggerAbort();
        proto().Stop();
        s_retry_reason = "麦克风启动失败，自动重试";
        xiaozhi::ui_set_status_text(s_retry_reason);
        schedule_reconnect(kReconnectDelayMs);
    }
}

// 把录音任务编码好的 Opus 帧发出去，返回本次发出的帧数。
// 只在控制任务里调用 —— 这样 mbedTLS 的发送栈用量落在 10KB 的控制任务上，
// 而不是把录音任务撑到 ~14KB（真机上 10KB 会在 ssl_write 里栈溢出重启）。
unsigned s_tx_frames = 0;

int drain_audio_tx() {
    static uint8_t frame[xiaozhi::kOpusFrameMaxBytes];
    size_t len = 0;
    int sent = 0;
    while (xiaozhi::AudioService::GetInstance().PopEncodedFrame(frame, sizeof(frame), &len)) {
        if (!proto().SendAudio(frame, len)) {
            ESP_LOGW(TAG, "SendAudio failed, dropping frame (%u bytes)", (unsigned)len);
            break;
        }
        s_tx_frames++;
        if (s_tx_frames % 50 == 0) {
            ESP_LOGI(TAG, "Sent %u audio frames so far (%u bytes last)", (unsigned)s_tx_frames,
                     (unsigned)len);
        }
        if (++sent >= 8) break;   // 单次最多发 8 帧，避免长时间占住控制循环
    }
    return sent;
}

void do_stop_listen() {
    if (proto().GetState() != State::Listening) return;
    // Push-to-talk: OK ends the utterance, commits it, and waits for a reply.
    s_conversation_active = false;
    xiaozhi::AudioService::GetInstance().StopRecording();
    while (drain_audio_tx() > 0) { }
    if (!proto().TriggerStopListen()) {
        xiaozhi::ui_set_status_text("发送失败，请重试");
    }
}

void do_abort() {
    // 先掐声音，再通知服务端：体感上"立刻安静"
    s_conversation_active = false;
    xiaozhi::AudioService::GetInstance().AbortPlayback();
    if (proto().IsConnected()) {
        proto().TriggerAbort();
    } else {
        on_state_changed(State::Idle);
    }
}

void do_stop_app() {
    ESP_LOGI(TAG, "Stopping Xiaozhi app: free heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    s_conversation_active = false;
    s_listen_after_connect = false;
    s_conversation_requested = false;
    s_session_ready = false;
    xiaozhi::AudioService::GetInstance().StopRecording();
    xiaozhi::AudioService::GetInstance().AbortPlayback();
    if (s_protocol_ready) {
        proto().Stop();
    }
    s_protocol_ready = false;
    std::string{}.swap(s_retry_reason);
    // 连常驻录音任务和 PCM 缓冲一起还给堆：切到别的 App 后小智不该占着内存
    if (s_audio_ready) {
        if (xiaozhi::AudioService::GetInstance().Deinit()) {
            s_audio_ready = false;
        } else {
            ESP_LOGE(TAG, "Audio teardown incomplete; retained resources for safety");
        }
    }
    // The registry keeps the last UI alive until its safe screen handoff.
    clear_timers();

    ESP_LOGI(TAG, "Xiaozhi app stopped cleanly. free heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void set_volume(int delta) {
    int vol = (int)bsp_audio_get_master_volume() + delta;
    if (vol > 100) vol = 100;
    if (vol < 0) vol = 0;
    settings_set_volume((uint8_t)vol);
    xiaozhi::ui_set_volume((unsigned)vol);
    ESP_LOGI(TAG, "Volume -> %d%%", vol);
}

void handle_proto_event(const Event &ev) {
    switch (ev.type) {
    case EvType::Connected:
        if (proto().HandshakeDone()) break;  // stale connected before queued hello
        s_connect_pending = true;
        s_connect_started_ms = now_ms();
        ESP_LOGI(TAG, "Socket connected, sending session.update");
        xiaozhi::ui_set_status_text("正在建立连接");
        if (!proto().ConfigureSession()) {
            s_connect_pending = false;
            s_connect_failures++;
            proto().Stop();
            s_retry_reason = "会话配置失败";
            xiaozhi::ui_set_status_text(s_retry_reason);
            schedule_reconnect(kReconnectDelayMs);
        }
        break;

    case EvType::HelloOk:
        if (!proto().HandshakeDone() || !proto().IsConnected()) break;
        s_connect_pending = false;
        s_reconnect_at_ms = 0;
        s_connect_failures = 0;
        // 播放采样率跟随服务端（官方实测下发 24000，本机默认 16000）
        if (proto().ServerSampleRate() > 0) {
            xiaozhi::AudioService::GetInstance().SetPlaybackSampleRate(proto().ServerSampleRate());
        }
        if (!xiaozhi::AudioService::GetInstance().PrepareSessionResources()) {
            ESP_LOGE(TAG, "Server hello succeeded but post-hello audio reservation failed");
            s_session_ready = false;
            s_audio_recovery_required = true;
            s_retry_reason = "音频内存不足，自动重试";
            s_conversation_active = false;
            proto().Stop();
            schedule_reconnect(kReconnectDelayMs);
            xiaozhi::ui_set_status_text(s_retry_reason);
            break;
        }
        s_session_ready = true;
        s_reconnect_delay_ms = kReconnectDelayMs;
        s_retry_reason.clear();
        xiaozhi::ui_set_status_text("准备就绪");
        if (s_listen_after_connect) {
            s_listen_after_connect = false;
            do_start_listen();
        }
        break;

    case EvType::StateChanged:
        on_state_changed(ev.state);
        break;

    case EvType::Disconnected:
        s_retry_reason = "网络断开";
        xiaozhi::ui_set_status_text(s_retry_reason);
        if (s_connect_pending) s_connect_failures++;
        s_connect_pending = false;
        clear_timers();
        s_conversation_active = false;
        s_session_ready = false;
        // 断线时把音频收干净，避免残留任务抱着 codec
        xiaozhi::AudioService::GetInstance().StopRecording();
        xiaozhi::AudioService::GetInstance().AbortPlayback();
        // 播放资源在一段 websocket 会话内常驻，但重连的 TLS 握手
        // 需要这块短期连续内存。只在断线边界释放，不影响多轮对话。
        if (!xiaozhi::AudioService::GetInstance().ReleaseSessionResources()) {
            s_audio_recovery_required = true;
        }
        schedule_reconnect(kReconnectDelayMs);
        break;

    case EvType::SocketError:
        s_connect_failures++;
        s_connect_pending = false;
        clear_timers();
        s_conversation_active = false;
        s_session_ready = false;
        xiaozhi::AudioService::GetInstance().StopRecording();
        xiaozhi::AudioService::GetInstance().AbortPlayback();
        if (!xiaozhi::AudioService::GetInstance().ReleaseSessionResources()) {
            s_audio_recovery_required = true;
        }
        s_retry_reason = "无法连接服务器";
        xiaozhi::ui_set_status_text(s_retry_reason);
        schedule_reconnect(kReconnectDelayMs);
        break;

    case EvType::UserText:
        xiaozhi::ui_set_user_text(ev.text);
        break;

    case EvType::AiSentence:
        xiaozhi::ui_set_ai_text(ev.text);
        break;

    case EvType::AiDelta:
        xiaozhi::ui_append_ai_text(ev.text);
        break;

    case EvType::LlmEmotion:
        xiaozhi::ui_set_emotion(ev.text);
        break;

    case EvType::Alert:
        ESP_LOGW(TAG, "Server alert: %s", ev.text);
        xiaozhi::ui_set_status_text(ev.text);
        break;

    case EvType::SessionInfo:
    case EvType::HandshakeTimeout:
        break;
    }
}

void handle_cmd(Cmd c) {
    switch (c) {
    case Cmd::Connect:
        s_reconnect_at_ms = 0;
        do_connect();
        break;
    case Cmd::StartListen: do_start_listen(); break;
    case Cmd::StopListen:  do_stop_listen();  break;
    case Cmd::Abort:       do_abort();        break;
    case Cmd::VolUp:       set_volume(+10);   break;
    case Cmd::VolDown:     set_volume(-10);   break;
    case Cmd::ToggleListen:
        // The input task never dereferences a client that the controller may
        // concurrently stop/destroy. Resolve the button against live state here.
        if (proto().NeedsRestart() || !proto().IsConnected() ||
            !proto().HandshakeDone() || !s_session_ready) {
            s_listen_after_connect = true;
            do_connect();
        }
        else if (proto().GetState() == State::Speaking) do_abort();
        else if (proto().GetState() == State::Listening) do_stop_listen();
        else if (proto().GetState() != State::Thinking) do_start_listen();
        break;
    case Cmd::Stop:        do_stop_app();     break;
    }
}

void process_conversation_request() {
    if (!s_conversation_requested.exchange(false) || s_stop_requested || !s_app_active) return;
    if (proto().NeedsRestart() || !proto().IsConnected() ||
        !proto().HandshakeDone() || !s_session_ready) {
        s_listen_after_connect = true;
        do_connect();
        return;
    }
    // A second double-click is an explicit start request, not ToggleListen:
    // never stop a recording or interrupt an ongoing answer/thinking turn.
    if (proto().GetState() == State::Idle) do_start_listen();
}

void tick() {
    const int64_t t = now_ms();

    // 1. 连上 Wi-Fi / 到达重连时间 → 发起连接
    if (!proto().IsConnected() && s_reconnect_at_ms != 0 && t >= s_reconnect_at_ms) {
        s_reconnect_at_ms = 0;
        post_cmd(Cmd::Connect);
    }
    if (s_next_connect_attempt_ms != 0 && t >= s_next_connect_attempt_ms) {
        s_next_connect_attempt_ms = 0;
        post_cmd(Cmd::Connect);
    }

    // 2. 握手超时：连上了但服务端一直不回 hello
    if (s_connect_pending && t - s_connect_started_ms > kConnectTimeoutMs) {
        ESP_LOGW(TAG, "Server hello timeout, retrying");
        s_connect_pending = false;
        s_connect_failures++;
        proto().Stop();
        s_retry_reason = "服务器无响应";
        xiaozhi::ui_set_status_text(s_retry_reason);
        schedule_reconnect(kReconnectDelayMs);
    }

    if (proto().GetState() == State::Listening && s_listening_since_ms != 0 &&
        t - s_listening_since_ms >= kListeningTimeoutMs) {
        s_listening_since_ms = 0;
        s_conversation_active = false;
        xiaozhi::AudioService::GetInstance().StopRecording();
        proto().TriggerAbort();
        xiaozhi::ui_set_ai_text("录音已停止，请按确定再说一次。");
    }

    // 3. 思考态无响应 → 主动 abort，避免 UI 永久卡在"思考中"按 OK 没反应
    if (proto().GetState() == State::Thinking && s_thinking_since_ms != 0 &&
        t - s_thinking_since_ms > kThinkingTimeoutMs) {
        ESP_LOGW(TAG, "Thinking timeout, aborting this turn");
        s_thinking_since_ms = 0;
        s_conversation_active = false;
        proto().TriggerAbort();
        xiaozhi::ui_set_ai_text("超时未收到回复，请再说一次。");
    }

    // Continuous stories/music may last arbitrarily long. Only lack of both
    // accepted network audio and successful PCM writes counts as a stall.
    if (proto().GetState() == State::Speaking) {
        const uint32_t progress = xiaozhi::AudioService::GetInstance().PlaybackProgress();
        if (progress != s_playback_progress) {
            s_playback_progress = progress;
            s_speaking_since_ms = t;
        }
        if (s_speaking_since_ms != 0 && t - s_speaking_since_ms > kSpeakingStallTimeoutMs) {
            ESP_LOGW(TAG, "Playback stalled without audio/PCM progress; aborting");
            do_abort();
        }
    }

    // 5. 兜底：云端音频已经在环形缓冲里、但播放任务还没起来（例如服务端没发
    //    tts/start 就直接推音频）。正常情况下 tts start 会先到，这里只是保险。
    if (s_session_ready && proto().GetState() == State::Speaking &&
        !xiaozhi::AudioService::GetInstance().IsPlaying() &&
        xiaozhi::AudioService::GetInstance().HasPendingPlayback()) {
        ESP_LOGI(TAG, "Audio queued without playback task; starting playback");
        xiaozhi::AudioService::GetInstance().StartPlayback();
    }
}

void control_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Control task started (stack=%u, free=%u largest=%u)",
             (unsigned)kControlTaskStack, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // 音频子系统不在这里初始化：要等 OTA 的临时任务把 12KB 栈还回来、堆最完整的
    // 时候再创建常驻录音任务（见 do_connect）。
    proto().SetEventQueue(s_proto_q);
    s_protocol_ready = true;   // 之后按键任务才允许访问协议单例
    xiaozhi::ui_init();
    xiaozhi::ui_show();

    post_cmd(Cmd::Connect);

    Event pev;
    Cmd cmd;

    while (true) {
        bool did_work = false;

        while (proto().PopCriticalEvent(&pev)) {
            handle_proto_event(pev);
            did_work = true;
        }
        if (proto().GetState() != s_cur_state) {
            on_state_changed(proto().GetState());
            did_work = true;
        }

        while (xQueueReceive(s_proto_q, &pev, 0) == pdTRUE) {
            handle_proto_event(pev);
            did_work = true;
        }

        bool stop_requested = false;
        // A4 兜底:Stop 命令若因队列满被丢,标志位仍在,这里会补一次退出。
        if (s_stop_requested) stop_requested = true;
        while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
            if (cmd == Cmd::Stop) {
                stop_requested = true;
                break;
            }
            handle_cmd(cmd);
            did_work = true;
        }

        if (stop_requested) {
            do_stop_app();
            break;
        }

        process_conversation_request();

        // 录音帧的发送在这里完成（不是录音任务），见 drain_audio_tx 的说明
        if (drain_audio_tx() > 0) did_work = true;

        tick();

        // 有活干就立刻再转一圈，空闲时 20ms 一跳（对按键响应而言无感）
        if (!did_work) vTaskDelay(pdMS_TO_TICKS(kLoopTickMs));
    }

    s_app_active = false;
    s_ctl_task = nullptr;
    if (s_stopped_sem) xSemaphoreGive(s_stopped_sem);
    ESP_LOGI(TAG, "Control task exiting");
    s_control_running.store(false);
    vTaskDelete(NULL);
}

} // namespace

// ---------------------------------------------------------------------------
// passport_app_t 接口
// ---------------------------------------------------------------------------

void xiaozhi_app_init(void) {
    // 重的初始化（音频缓冲/codec）放在控制任务里做，见 control_task()。
    ESP_LOGI(TAG, "Xiaozhi app init (deferred to control task)");
}

void xiaozhi_app_start(void) {
    s_start_failed = false;
    // 2026-09-13 修复(A4):旧逻辑只判 s_app_active。若上一次 stop() 没能收掉
    // 控制任务(Stop 被丢 / 卡在连接里),这里会再建一个 xzg_ctl —— 两个任务
    // 同时操作 proto() 与音频服务,必然踩文件头记录过的 use-after-free。
    // 现在:只要旧任务句柄还在,就直接拒绝启动。
    if (s_ctl_task != nullptr) {
        ESP_LOGE(TAG, "Previous control task still alive; refusing to start a second one");
        return;
    }
    if (s_app_active) {
        ESP_LOGW(TAG, "Xiaozhi app already active");
        return;
    }

    // A previous bounded stop may have retained a slow worker to avoid UAF.
    // Give it one more cleanup opportunity before creating a new owner.
    if (s_audio_ready) {
        if (!xiaozhi::AudioService::GetInstance().Deinit()) {
            ESP_LOGE(TAG, "Previous audio worker is still alive; refusing a second owner");
            return;
        }
        s_audio_ready = false;
    }

    if (s_proto_q == nullptr) s_proto_q = xQueueCreateStatic(kProtoQueueDepth, sizeof(Event), s_proto_items, &s_proto_queue_storage);
    if (s_cmd_q == nullptr) s_cmd_q = xQueueCreateStatic(kCmdQueueDepth, sizeof(Cmd), s_cmd_items, &s_cmd_queue_storage);
    if (s_stopped_sem == nullptr) s_stopped_sem = xSemaphoreCreateBinaryStatic(&s_stopped_storage);

    if (s_proto_q == nullptr || s_cmd_q == nullptr || s_stopped_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create queues/semaphore (free=%u)",
                 (unsigned)esp_get_free_heap_size());
        s_start_failed = true;
        xiaozhi::ui_init();
        xiaozhi::ui_show();
        xiaozhi::ui_set_status_text("启动失败，请重试");
        xiaozhi::ui_set_ai_text("内存暂时不足。确定重试，或长按确定返回。");
        return;
    }

    // 清空可能残留的事件/命令
    xQueueReset(s_proto_q);
    xQueueReset(s_cmd_q);
    while (xSemaphoreTake(s_stopped_sem, 0) == pdTRUE) { }

    s_app_active = true;
    s_stop_requested = false;
    s_conversation_requested = false;
    s_cur_state = State::Disconnected;
    s_listen_after_connect = false;
    clear_timers();
    s_reconnect_at_ms = 0;
    s_next_connect_attempt_ms = 0;
    s_conversation_active = false;
    s_session_ready = false;
    s_audio_recovery_required = false;
    s_retry_reason.clear();
    s_connect_failures = 0;

    ESP_LOGI(TAG, "Starting Xiaozhi app...");
    s_control_running.store(true);
    BaseType_t ok = xTaskCreate(control_task, "xzg_ctl", kControlTaskStack, nullptr, 5, &s_ctl_task);
    if (ok != pdPASS) {
        s_control_running.store(false);
        ESP_LOGE(TAG, "Failed to create control task (free=%u largest=%u)",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_app_active = false;
        s_start_failed = true;
        xiaozhi::ui_init();
        xiaozhi::ui_show();
        xiaozhi::ui_set_status_text("启动失败，请重试");
        xiaozhi::ui_set_ai_text("内存暂时不足。确定重试，或长按确定返回。");
        return;
    }
}

void xiaozhi_app_request_conversation(void) {
    if (s_app_active && !s_stop_requested) s_conversation_requested = true;
}

bool xiaozhi_app_prepare_stop(void) {
    s_conversation_requested = false;
    if (s_app_active.exchange(false)) post_cmd(Cmd::Stop);
    if (!s_control_running.load() && s_audio_ready) {
        if (xiaozhi::AudioService::GetInstance().Deinit()) s_audio_ready = false;
    }
    return !s_control_running.load() && !s_audio_ready;
}

void xiaozhi_app_stop(void) {
    ESP_LOGI(TAG, "Requesting Xiaozhi app stop");
    s_app_active = false;

    if (s_ctl_task == nullptr) {
        // 控制任务已经退出：这里仍然把协议栈和音频收干净，保证幂等
        do_stop_app();
        xiaozhi::ui_hide();
        return;
    }

    // 请求控制任务收尾，并等它真正结束。绝不 vTaskDelete 一个正在跑的任务
    // （旧实现会删掉可能正卡在 connect_xiaozhi() 里的 wifi 任务，属于典型的
    //  "删任务时它正持有堆锁/中间态" → 堆损坏 → 随机重启）。
    post_cmd(Cmd::Stop);
    if (xSemaphoreTake(s_stopped_sem, pdMS_TO_TICKS(6000)) != pdTRUE) {
        ESP_LOGE(TAG, "Control task did not stop in time; leaving it to self-exit");
    } else {
        xiaozhi::ui_hide();
    }
}

void xiaozhi_app_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (s_start_failed && btn == BSP_BTN_OK && event == BTN_EVT_CLICK) {
        xiaozhi_app_start();
        return;
    }
    if (!s_app_active) return;
    // 协议单例还没构造好之前不碰它：按键任务只有 3072 字节栈，而构造 Protocol
    // 会走 NVS 写入（SPI-Flash 擦写）这条很深的调用链。
    if (!s_protocol_ready) return;
    if (event != BTN_EVT_CLICK) return;

    if (btn == BSP_BTN_OK) {
        post_cmd(Cmd::ToggleListen);
    } else if (btn == BSP_BTN_UP) {
        post_cmd(Cmd::VolUp);
    } else if (btn == BSP_BTN_DOWN) {
        post_cmd(Cmd::VolDown);
    }
}

const passport_app_t g_xiaozhi_app = {
    .id = APP_ID_XIAOZHI,
    .name = "Grok",
    .en_name = "GROK",
    .desc = "按确定说话\n再按确定等待回复",
    .tag = "直连",
    .theme_color = 0x38BDF8,
    .init = xiaozhi_app_init,
    .start = xiaozhi_app_start,
    .stop = xiaozhi_app_stop,
    .on_key = xiaozhi_app_on_key,
    .prepare_stop = xiaozhi_app_prepare_stop,
};
