#include "mock_platform.h"
#define private public
#include "apps/xiaozhi/grok_protocol.h"
#include "apps/xiaozhi/grok_config.h"
#include "apps/xiaozhi/xiaozhi_audio.h"
#undef private
#include "../main/apps/xiaozhi/grok_protocol.cc"

static int accepted_audio=0;
namespace device_control {void cancel_session(){} void response_sent(uint64_t,bool){} }
namespace xiaozhi {
AudioService::AudioService() {} AudioService::~AudioService() {}
AudioService &AudioService::GetInstance() {static AudioService a;return a;}
void AudioService::PushIncomingAudio(const uint8_t *,size_t) {++accepted_audio;}
}

static esp_websocket_event_data_t text_frame(const char *body) {
    const int n = int(std::strlen(body));
    return {body, n, 0, n, 1, {0}};
}

int main() {
    using namespace xiaozhi;
    assert(!grok_key_acceptable("short"));
    assert(!grok_key_acceptable("has space-in-token!!"));
    assert(!grok_key_acceptable("quote\"in-the-token!!"));
    assert(grok_key_acceptable("xai-test-key-0123456789"));
    assert(grok_proxy_acceptable("", 0));
    assert(!grok_proxy_acceptable("", 80));
    assert(grok_proxy_acceptable("192.168.1.20", 7897));
    assert(!grok_proxy_acceptable("127.0.0.1", 7897));
    assert(!grok_proxy_acceptable("10.0.0.255", 7897));
    assert(!grok_proxy_acceptable("api.x.ai", 443));

    Protocol p;
    auto q=xQueueCreate(4, sizeof(Event));
    p.SetEventQueue(q);
    p.Publish(EvType::AiSentence, "fills one slot");
    p.Publish(EvType::Connected, nullptr);
    p.Publish(EvType::HelloOk, nullptr);
    p.Publish(EvType::Disconnected, nullptr);
    p.Publish(EvType::SocketError, nullptr);
    Event event;
    const EvType order[]={EvType::Disconnected, EvType::SocketError, EvType::Connected, EvType::HelloOk};
    for (auto expected : order) { assert(p.PopCriticalEvent(&event)); assert(event.type==expected); }
    assert(!p.PopCriticalEvent(&event));

    assert(!p.Start("too-short", "", 0));
    assert(!p.client_ && !p.rx_text_ && p.headers_[0]=='\0');
    const char *key="xai-test-key-0123456789";
    assert(p.Start(key, "", 0));
    assert(mock_ws_uri.find("wss://api.x.ai/v1/realtime?model=grok-voice-latest")!=std::string::npos);
    assert(mock_ws_headers==std::string("Authorization: Bearer ")+key+"\r\n");
    assert(mock_ws_headers.find("Protocol-Version")==std::string::npos);
    assert(mock_ws_headers.find("Device-Id")==std::string::npos);
    assert(p.headers_[0]=='\0'); // local copy wiped after the client duplicates it
    assert(p.ConfigureSession());
    assert(mock_sent_text.size()==1);
    const std::string &update=mock_sent_text.back();
    assert(update.find("\"type\":\"session.update\"")!=std::string::npos);
    assert(update.find("\"voice\":\"eve\"")!=std::string::npos);
    assert(update.find("\"turn_detection\":null")!=std::string::npos);
    assert(update.find("\"language_hint\":\"zh\"")!=std::string::npos);
    assert(update.find("\"type\":\"audio/opus\"")!=std::string::npos);
    assert(update.find("\"transport\":\"binary\"")!=std::string::npos);
    assert(update.find("小屏幕")!=std::string::npos);
    assert(!p.TriggerStartListen()); // session not ready
    const char updated[]="{\"type\":\"session.updated\",\"session\":{\"voice\":\"eve\"}}";
    esp_websocket_event_data_t hello=text_frame(updated);
    p.HandleData(&hello);
    assert(p.HandshakeDone() && p.GetState()==State::Idle);
    assert(p.PopCriticalEvent(&event) && event.type==EvType::HelloOk);
    assert(p.TriggerStartListen() && mock_sent_text.size()==1 && p.GetState()==State::Listening);
    const uint8_t packet[]={0x01, 0x02, 0x03, 0x04};
    assert(p.SendAudio(packet, sizeof(packet)));
    assert(mock_sent_bin.size()==1 && mock_sent_bin.back().size()==sizeof(packet));
    assert(std::memcmp(mock_sent_bin.back().data(), packet, sizeof(packet))==0);
    p.SetState(State::Speaking);
    esp_websocket_event_data_t bin{reinterpret_cast<const char *>(packet), 4, 0, 4, 2, {0}};
    p.HandleData(&bin);
    assert(accepted_audio==1);
    p.hello_ok_=false;
    p.HandleData(&bin);
    assert(accepted_audio==1);
    p.hello_ok_=true;
    p.SetState(State::Listening);
    assert(p.TriggerStopListen());
    assert(mock_sent_text[mock_sent_text.size()-2].find("input_audio_buffer.commit")!=std::string::npos);
    assert(mock_sent_text.back().find("response.create")!=std::string::npos);
    assert(p.GetState()==State::Thinking);
    const char user[]="{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\"\\u4f60\\u597d\"}";
    esp_websocket_event_data_t user_ev=text_frame(user);
    p.HandleData(&user_ev);
    assert(xQueueReceive(q, &event, 0)==pdTRUE && event.type==EvType::AiSentence);
    assert(xQueueReceive(q, &event, 0)==pdTRUE && event.type==EvType::UserText);
    assert(std::strcmp(event.text, "你好")==0);
    const char delta[]="{\"type\":\"response.output_audio_transcript.delta\",\"delta\":\"早\"}";
    esp_websocket_event_data_t delta_ev=text_frame(delta);
    p.HandleData(&delta_ev);
    assert(xQueueReceive(q, &event, 0)==pdTRUE && event.type==EvType::AiDelta && std::strcmp(event.text, "早")==0);
    p.SetState(State::Speaking);
    const char done[]="{\"type\":\"response.done\"}";
    esp_websocket_event_data_t done_ev=text_frame(done);
    p.HandleData(&done_ev);
    assert(p.GetState()==State::Idle);
    const unsigned stops=mock_ws_stops;
    const char err[]="{\"type\":\"error\",\"error\":{\"message\":\"rate limit\"}}";
    esp_websocket_event_data_t err_ev=text_frame(err);
    p.HandleData(&err_ev);
    assert(mock_ws_stops==stops);
    assert(xQueueReceive(q, &event, 0)==pdTRUE && event.type==EvType::Alert);
    assert(std::strstr(event.text, "rate limit"));
    p.SetState(State::Speaking);
    assert(p.TriggerAbort());
    assert(mock_sent_text.back().find("response.cancel")!=std::string::npos);
    assert(p.GetState()==State::Idle);
    std::string huge(kMaxTextMessage, ' ');
    huge.replace(0, 15, "{\"type\":\"error\"");
    esp_websocket_event_data_t huge_ev{huge.data(), int(huge.size()), 0, int(huge.size()), 1, {0}};
    p.HandleData(&huge_ev);
    assert(p.GetState()==State::Idle);
    p.WebsocketEventHandler(&p, nullptr, WEBSOCKET_EVENT_DISCONNECTED, nullptr);
    assert(p.NeedsRestart() && !p.HandshakeDone());
    assert(p.PopCriticalEvent(&event) && event.type==EvType::Disconnected);
    p.WebsocketEventHandler(&p, nullptr, WEBSOCKET_EVENT_CLOSED, nullptr);
    p.WebsocketEventHandler(&p, nullptr, WEBSOCKET_EVENT_FINISH, nullptr);
    assert(!p.PopCriticalEvent(&event));
    assert(mock_ws_stops==stops);
    p.Stop();
    assert(p.headers_[0]=='\0' && !p.rx_text_ && !p.client_);
    mock_ws_connected=true;
    assert(p.Start(key, "", 0));
    mock_ws_lifecycle.clear();
    p.Stop();
    assert(mock_ws_lifecycle.size()>=3);
    assert(mock_ws_lifecycle[0]=="ping" && mock_ws_lifecycle[1]=="stop" && mock_ws_lifecycle[2]=="destroy");
    delete q;
    std::puts("Grok protocol: session, opus binary, transcripts, cancel, reconnect PASS");
}
