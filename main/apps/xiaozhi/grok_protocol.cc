#include "apps/xiaozhi/grok_protocol.h"
#include "apps/xiaozhi/grok_config.h"
#include "apps/xiaozhi/xiaozhi_audio.h"
#include "device_control.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cstdio>
#include <cstring>

#if defined(ESP_PLATFORM)
#include "apps/xiaozhi/grok_tunnel.h"
#endif

static const char *TAG = "grok_proto";

namespace xiaozhi {

namespace {

constexpr TickType_t kSendTimeout = pdMS_TO_TICKS(150);
constexpr char kUrl[] = "wss://api.x.ai/v1/realtime?model=grok-voice-latest";
constexpr char kPath[] = "/v1/realtime?model=grok-voice-latest";
constexpr char kSessionUpdate[] =
    "{\"type\":\"session.update\",\"session\":{"
    "\"instructions\":\"你是护照上的语音助手 Grok。请用简短、自然的中文回答，适合在小屏幕上阅读。\","
    "\"voice\":\"eve\","
    "\"turn_detection\":null,"
    "\"reasoning\":{\"effort\":\"none\"},"
    "\"audio\":{\"input\":{\"format\":{\"type\":\"audio/opus\"},\"transport\":\"binary\","
    "\"transcription\":{\"language_hint\":\"zh\"}},"
    "\"output\":{\"format\":{\"type\":\"audio/opus\"},\"transport\":\"binary\"}}}}";
constexpr char kCommit[] = "{\"type\":\"input_audio_buffer.commit\"}";
constexpr char kRespond[] = "{\"type\":\"response.create\"}";
constexpr char kCancel[] = "{\"type\":\"response.cancel\"}";
constexpr char kClear[] = "{\"type\":\"input_audio_buffer.clear\"}";

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_utf8(char *out, size_t cap, size_t *n, unsigned cp) {
    char tmp[3];
    int len = 0;
    if (cp < 0x80) {
        tmp[0] = static_cast<char>(cp);
        len = 1;
    } else if (cp < 0x800) {
        tmp[0] = static_cast<char>(0xC0 | (cp >> 6));
        tmp[1] = static_cast<char>(0x80 | (cp & 0x3F));
        len = 2;
    } else if (cp < 0x10000) {
        tmp[0] = static_cast<char>(0xE0 | (cp >> 12));
        tmp[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = static_cast<char>(0x80 | (cp & 0x3F));
        len = 3;
    } else {
        return;
    }
    if (*n + static_cast<size_t>(len) >= cap) return;
    std::memcpy(out + *n, tmp, static_cast<size_t>(len));
    *n += static_cast<size_t>(len);
}

// Copies a JSON string value into out. Returns false when the key is absent.
bool json_string(const char *json, const char *key, char *out, size_t cap) {
    if (!json || !key || !out || cap == 0) return false;
    out[0] = '\0';
    char pattern[48];
    std::snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = json;
    const char *value = nullptr;
    while ((p = std::strstr(p, pattern)) != nullptr) {
        const char *after = p + std::strlen(pattern);
        while (*after == ' ' || *after == '\n' || *after == '\r' || *after == '\t') ++after;
        if (*after == ':') {
            value = after + 1;
            break;
        }
        ++p;
    }
    if (!value) return false;
    while (*value == ' ' || *value == '\n' || *value == '\r' || *value == '\t') ++value;
    if (*value != '"') return false;
    ++value;
    size_t n = 0;
    while (*value && *value != '"') {
        unsigned char c = static_cast<unsigned char>(*value++);
        if (c == '\\') {
            if (!*value) break;
            const char esc = *value++;
            if (esc == 'u') {
                unsigned cp = 0;
                bool ok = true;
                for (int i = 0; i < 4; ++i) {
                    const int nib = hex_nibble(*value);
                    if (nib < 0) {
                        ok = false;
                        break;
                    }
                    cp = (cp << 4) | static_cast<unsigned>(nib);
                    ++value;
                }
                if (ok) append_utf8(out, cap, &n, cp);
                continue;
            }
            if (esc == 'n') c = '\n';
            else if (esc == 'r') c = '\r';
            else if (esc == 't') c = '\t';
            else c = static_cast<unsigned char>(esc);
        }
        if (n + 1 < cap) out[n++] = static_cast<char>(c);
    }
    out[n] = '\0';
    return true;
}

} // namespace

const char *Protocol::StateName(State s) {
    switch (s) {
    case State::Disconnected: return "Disconnected";
    case State::Connecting:   return "Connecting";
    case State::Idle:         return "Idle";
    case State::Listening:    return "Listening";
    case State::Thinking:     return "Thinking";
    case State::Speaking:     return "Speaking";
    case State::Error:        return "Error";
    }
    return "?";
}

Protocol::Protocol() = default;

Protocol::~Protocol() { Stop(); }

bool Protocol::Start(const std::string &key, const char *proxy_host, uint16_t proxy_port) {
    if (client_ != nullptr || rx_text_) Stop();
    if (!grok_key_acceptable(key.c_str())) {
        SetState(State::Error);
        return false;
    }
    const bool use_proxy = proxy_host && proxy_host[0];
    if (use_proxy && !grok_proxy_acceptable(proxy_host, proxy_port)) {
        SetState(State::Error);
        return false;
    }

    hello_ok_ = false;
    session_sent_ = false;
    needs_restart_ = false;
    critical_events_ = 0;
    ResetRx();
    std::snprintf(headers_, sizeof(headers_), "Authorization: Bearer %s\r\n", key.c_str());

    rx_text_ = static_cast<char *>(heap_caps_malloc(kMaxTextMessage, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!rx_text_) {
        std::memset(headers_, 0, sizeof(headers_));
        SetState(State::Error);
        return false;
    }
    rx_text_[0] = '\0';
    SetState(State::Connecting);

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = kUrl;
    ws_cfg.buffer_size = 1024;
    ws_cfg.task_stack = 5120;
    ws_cfg.task_prio = 5;
    ws_cfg.ping_interval_sec = 15;
    ws_cfg.disable_auto_reconnect = true;
    ws_cfg.network_timeout_ms = 8000;
    ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    ws_cfg.headers = headers_;

#if defined(ESP_PLATFORM)
    if (use_proxy) {
        GrokTunnel opened{};
        if (!grok_tunnel_open(&opened, proxy_host, proxy_port, kPath, headers_)) {
            std::memset(headers_, 0, sizeof(headers_));
            heap_caps_free(rx_text_);
            rx_text_ = nullptr;
            SetState(State::Error);
            return false;
        }
        tunnel_.stream = opened.stream;
        tunnel_.ws = opened.ws;
        ws_cfg.ext_transport = tunnel_.ws;
    }
#else
    (void)use_proxy;
#endif

    ESP_LOGI(TAG, "Connecting to Grok realtime (key length=%u, proxy=%s)",
             static_cast<unsigned>(key.size()), use_proxy ? "yes" : "no");
    client_ = esp_websocket_client_init(&ws_cfg);
    if (!client_) {
        ReleaseTunnel();
        std::memset(headers_, 0, sizeof(headers_));
        heap_caps_free(rx_text_);
        rx_text_ = nullptr;
        SetState(State::Error);
        return false;
    }
    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, WebsocketEventHandler, this);
    if (esp_websocket_client_start(client_) != ESP_OK) {
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
        ReleaseTunnel();
        std::memset(headers_, 0, sizeof(headers_));
        heap_caps_free(rx_text_);
        rx_text_ = nullptr;
        SetState(State::Error);
        return false;
    }
#if !defined(ESP_PLATFORM)
    std::memset(headers_, 0, sizeof(headers_));
#else
    if (!tunnel_.ws) std::memset(headers_, 0, sizeof(headers_));
#endif
    return true;
}

void Protocol::ReleaseTunnel() {
#if defined(ESP_PLATFORM)
    GrokTunnel opened{tunnel_.stream, tunnel_.ws};
    tunnel_ = {};
    grok_tunnel_destroy(&opened);
#endif
}

void Protocol::Stop() {
    device_control::cancel_session();
    needs_restart_ = true;
    if (client_) {
        if (esp_websocket_client_is_connected(client_))
            esp_websocket_client_send_with_opcode(client_, WS_TRANSPORT_OPCODES_PING, nullptr, 0,
                                                  pdMS_TO_TICKS(100));
        esp_websocket_client_stop(client_);
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
    }
    ReleaseTunnel();
    hello_ok_ = false;
    session_sent_ = false;
    if (rx_text_) {
        heap_caps_free(rx_text_);
        rx_text_ = nullptr;
    }
    std::memset(headers_, 0, sizeof(headers_));
    ResetRx();
    SetState(State::Disconnected);
    critical_events_ = 0;
}

bool Protocol::IsConnected() const {
    return client_ && esp_websocket_client_is_connected(client_);
}

void Protocol::Publish(EvType type, const char *text, State st) {
    if (type == EvType::StateChanged) return;
    if (type == EvType::Connected || type == EvType::Disconnected ||
        type == EvType::SocketError || type == EvType::HelloOk) {
        critical_events_.fetch_or(1u << static_cast<unsigned>(type));
        return;
    }
    if (event_q_ == nullptr) return;
    Event ev = {};
    ev.type = type;
    ev.state = st;
    if (text != nullptr) {
        std::strncpy(ev.text, text, kEventTextCap - 1);
        ev.text[kEventTextCap - 1] = '\0';
    }
    if (xQueueSend(event_q_, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, dropped event type=%d", static_cast<int>(type));
    }
}

bool Protocol::PopCriticalEvent(Event *out) {
    if (out == nullptr) return false;
    const EvType order[] = {EvType::Disconnected, EvType::SocketError,
                            EvType::Connected, EvType::HelloOk};
    for (EvType type : order) {
        const uint32_t bit = 1u << static_cast<unsigned>(type);
        if (critical_events_.fetch_and(~bit) & bit) {
            *out = {};
            out->type = type;
            return true;
        }
    }
    return false;
}

void Protocol::SetState(State new_state) {
    if (state_ == new_state) return;
    const State old = state_.exchange(new_state);
    ESP_LOGI(TAG, "State: %s -> %s", StateName(old), StateName(new_state));
}

bool Protocol::SendJson(const char *json) {
    if (!json || !IsConnected()) return false;
    const int n = static_cast<int>(std::strlen(json));
    const int sent = esp_websocket_client_send_text(client_, json, n, kSendTimeout);
    if (sent != n) {
        ESP_LOGW(TAG, "send_text failed (%d bytes)", n);
        return false;
    }
    return true;
}

bool Protocol::ConfigureSession() {
    if (!IsConnected() || NeedsRestart()) return false;
    if (hello_ok_ || session_sent_) return true;
    if (!SendJson(kSessionUpdate)) return false;
    session_sent_ = true;
    ESP_LOGI(TAG, "session.update sent (opus 24 kHz, manual turn)");
    return true;
}

bool Protocol::TriggerStartListen() {
    if (!IsConnected() || !HandshakeDone() || NeedsRestart()) return false;
    const State st = GetState();
    if (st == State::Listening || st == State::Thinking) return false;
    SetState(State::Listening);
    return true;
}

bool Protocol::TriggerStopListen() {
    if (!IsConnected() || GetState() != State::Listening) return false;
    if (!SendJson(kCommit) || !SendJson(kRespond)) return false;
    ESP_LOGI(TAG, "input committed, response requested");
    SetState(State::Thinking);
    return true;
}

bool Protocol::TriggerAbort() {
    if (!IsConnected()) {
        SetState(State::Idle);
        return false;
    }
    SendJson(kClear);
    const bool ok = SendJson(kCancel);
    ESP_LOGI(TAG, "response.cancel sent");
    SetState(State::Idle);
    return ok;
}

bool Protocol::SendAudio(const uint8_t *opus_data, size_t len) {
    if (!opus_data || len == 0 || len > kMaxBinaryMessage) return false;
    if (!IsConnected() || GetState() != State::Listening) return false;
    const int sent = esp_websocket_client_send_bin(client_, reinterpret_cast<const char *>(opus_data),
                                                   static_cast<int>(len), kSendTimeout);
    return sent == static_cast<int>(len);
}

void Protocol::ResetRx() {
    rx_text_len_ = 0;
    bin_len_ = 0;
}

void Protocol::WebsocketEventHandler(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data) {
    (void)base;
    auto *self = static_cast<Protocol *>(handler_args);
    auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: self->OnConnected(); break;
    case WEBSOCKET_EVENT_DISCONNECTED: self->OnDisconnected(); break;
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_FINISH: self->OnSessionEnded(); break;
    case WEBSOCKET_EVENT_DATA: self->HandleData(data); break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error, type=%d", data ? static_cast<int>(data->error_handle.error_type) : -1);
        self->hello_ok_ = false;
        self->needs_restart_ = true;
        self->SetState(State::Error);
        self->Publish(EvType::SocketError, "网络连接异常");
        break;
    default:
        break;
    }
}

void Protocol::OnConnected() {
    if (NeedsRestart()) return;
    ESP_LOGI(TAG, "WebSocket connected");
    hello_ok_ = false;
    session_sent_ = false;
    ResetRx();
    Publish(EvType::Connected, nullptr);
}

void Protocol::OnDisconnected() {
    ESP_LOGW(TAG, "WebSocket disconnected");
    hello_ok_ = false;
    SetState(State::Disconnected);
    if (!needs_restart_.exchange(true)) Publish(EvType::Disconnected, nullptr);
}

void Protocol::OnSessionEnded() {
    hello_ok_ = false;
    SetState(State::Disconnected);
    if (!needs_restart_.exchange(true)) Publish(EvType::Disconnected, nullptr);
}

void Protocol::HandleData(const esp_websocket_event_data_t *d) {
    if (NeedsRestart()) return;
    if (d == nullptr || d->data_ptr == nullptr || d->data_len <= 0) return;
    const size_t off = static_cast<size_t>(d->payload_offset);
    const size_t total = static_cast<size_t>(d->payload_len);
    const size_t n = static_cast<size_t>(d->data_len);

    if (d->op_code == 0x02 || (d->op_code == 0x00 && bin_len_ > 0 && rx_text_len_ == 0)) {
        if (d->op_code == 0x02 && off == 0) bin_len_ = 0;
        const size_t space = kMaxBinaryMessage - bin_len_;
        const size_t copy = n > space ? space : n;
        if (copy > 0) {
            std::memcpy(rx_bin_ + bin_len_, d->data_ptr, copy);
            bin_len_ += copy;
        }
        const bool complete = (total == 0) || (off + n >= total);
        if (!complete && bin_len_ >= kMaxBinaryMessage) {
            ESP_LOGW(TAG, "Binary frame too large, dropped");
            bin_len_ = 0;
            return;
        }
        if (complete) {
            if (HandshakeDone() && (GetState() == State::Speaking || GetState() == State::Thinking)) {
                if (GetState() == State::Thinking) SetState(State::Speaking);
                AudioService::GetInstance().PushIncomingAudio(rx_bin_, bin_len_);
            }
            bin_len_ = 0;
        }
        return;
    }

    if (d->op_code == 0x01 || (d->op_code == 0x00 && rx_text_len_ > 0)) {
        if (!rx_text_) return;
        if (d->op_code == 0x01 && off == 0) rx_text_len_ = 0;
        if (total >= kMaxTextMessage || rx_text_len_ + n >= kMaxTextMessage) {
            if (off == 0) ESP_LOGW(TAG, "Text message of %u bytes exceeds %u, dropped",
                                   static_cast<unsigned>(total), static_cast<unsigned>(kMaxTextMessage));
            rx_text_len_ = 0;
            return;
        }
        const size_t space = kMaxTextMessage - 1 - rx_text_len_;
        const size_t copy = n > space ? space : n;
        if (copy > 0) {
            std::memcpy(rx_text_ + rx_text_len_, d->data_ptr, copy);
            rx_text_len_ += copy;
        }
        const bool complete = (total == 0) || (off + n >= total);
        if (complete) {
            rx_text_[rx_text_len_] = '\0';
            DispatchText();
            rx_text_len_ = 0;
        }
    }
}

void Protocol::DispatchText() {
    char type[80];
    if (!json_string(rx_text_, "type", type, sizeof(type))) {
        ESP_LOGW(TAG, "Message without type");
        return;
    }
    ESP_LOGI(TAG, "event %s", type);
    char text[kEventTextCap];

    if (std::strcmp(type, "session.updated") == 0) {
        if (!hello_ok_) {
            hello_ok_ = true;
            Publish(EvType::HelloOk, nullptr);
            SetState(State::Idle);
        }
    } else if (std::strcmp(type, "session.created") == 0 ||
               std::strcmp(type, "conversation.created") == 0 ||
               std::strcmp(type, "input_audio_buffer.committed") == 0 ||
               std::strcmp(type, "input_audio_buffer.cleared") == 0 ||
               std::strcmp(type, "response.output_audio.done") == 0 ||
               std::strcmp(type, "response.output_audio.delta") == 0) {
        // Lifecycle only. Audio travels as binary frames.
    } else if (std::strcmp(type, "response.created") == 0) {
        if (GetState() == State::Thinking || GetState() == State::Idle) SetState(State::Speaking);
    } else if (std::strcmp(type, "response.done") == 0) {
        if (GetState() == State::Speaking || GetState() == State::Thinking) SetState(State::Idle);
    } else if (std::strcmp(type, "response.output_audio_transcript.delta") == 0) {
        if (json_string(rx_text_, "delta", text, sizeof(text)) && text[0]) Publish(EvType::AiDelta, text);
    } else if (std::strcmp(type, "response.output_audio_transcript.done") == 0) {
        if (json_string(rx_text_, "transcript", text, sizeof(text))) Publish(EvType::AiSentence, text);
    } else if (std::strcmp(type, "conversation.item.input_audio_transcription.completed") == 0 ||
               std::strcmp(type, "conversation.item.input_audio_transcription.updated") == 0) {
        if (json_string(rx_text_, "transcript", text, sizeof(text)) && text[0]) Publish(EvType::UserText, text);
    } else if (std::strcmp(type, "error") == 0) {
        if (!json_string(rx_text_, "message", text, sizeof(text)) || !text[0]) {
            std::snprintf(text, sizeof(text), "服务异常");
        }
        Publish(EvType::Alert, text);
        if (GetState() == State::Thinking || GetState() == State::Speaking) SetState(State::Idle);
    } else {
        ESP_LOGI(TAG, "Unhandled event");
    }
}

} // namespace xiaozhi
