#pragma once

// Grok Speech-to-Speech client.
//
// WebSocket callbacks only post events or enqueue Opus. They do not decode,
// play, send the session update, or stop the client. The control task owns
// Start/Stop, session.update, commit/response.create, and response.cancel.
// This object is a function-local static in the app and is not destroyed
// while a callback can still run.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace xiaozhi {

enum class State {
    Disconnected,
    Connecting,
    Idle,
    Listening,
    Thinking,
    Speaking,
    Error,
};

enum class EvType : uint8_t {
    Connected,
    Disconnected,
    SocketError,
    HelloOk,       // session.updated accepted
    SessionInfo,
    StateChanged,
    UserText,      // input transcript, replace
    AiSentence,    // assistant transcript, replace
    AiDelta,       // assistant transcript, append
    LlmEmotion,
    Alert,
    HandshakeTimeout,
};

constexpr size_t kEventTextCap = 256;
constexpr size_t kMaxTextMessage = 3072;
constexpr size_t kMaxBinaryMessage = 512;

struct Event {
    EvType type;
    State state;
    char text[kEventTextCap];
};

using EventQueue = QueueHandle_t;

class Protocol {
public:
    Protocol();
    ~Protocol();

    // key is the xAI API key. proxy_host empty means connect directly.
    // The key is copied into the socket config and then wiped from this object
    // after the client has its own copy, except while a proxy transport still
    // needs the header pointer.
    bool Start(const std::string &key, const char *proxy_host, uint16_t proxy_port);
    void Stop();

    bool IsConnected() const;
    bool HasClient() const { return client_ != nullptr; }
    bool NeedsRestart() const { return needs_restart_.load(); }
    State GetState() const { return state_.load(); }

    void SetEventQueue(EventQueue q) { event_q_ = q; }
    void SetState(State new_state);
    bool PopCriticalEvent(Event *out);

    // Sent from the control task after Connected. Not from the socket callback.
    bool ConfigureSession();
    bool TriggerStartListen();
    bool TriggerStopListen();
    bool TriggerAbort();
    bool SendAudio(const uint8_t *opus_data, size_t len);

    bool HandshakeDone() const { return hello_ok_.load(); }
    int ServerSampleRate() const { return 24000; }

    static const char *StateName(State s);

private:
    static void WebsocketEventHandler(void *handler_args, esp_event_base_t base,
                                      int32_t event_id, void *event_data);
    void OnConnected();
    void OnDisconnected();
    void OnSessionEnded();
    void HandleData(const esp_websocket_event_data_t *data);
    void DispatchText();
    void Publish(EvType type, const char *text, State st = State::Disconnected);
    bool SendJson(const char *json);
    void ResetRx();
    void ReleaseTunnel();

    esp_websocket_client_handle_t client_{nullptr};
    std::atomic<State> state_{State::Disconnected};
    std::atomic<uint32_t> critical_events_{0};
    EventQueue event_q_{nullptr};
    std::atomic<bool> hello_ok_{false};
    std::atomic<bool> needs_restart_{false};
    bool session_sent_{false};

    char *rx_text_{nullptr};
    size_t rx_text_len_{0};
    uint8_t rx_bin_[kMaxBinaryMessage];
    size_t bin_len_{0};

    // Lives until Stop when a proxy transport holds the pointer. Wiped, never logged.
    char headers_[320]{};
#if defined(ESP_PLATFORM)
    struct Tunnel {
        esp_transport_handle_t stream;
        esp_transport_handle_t ws;
    } tunnel_{};
#endif
};

} // namespace xiaozhi
