#include "mock_platform.h"
#define private public
#include "apps/xiaozhi/xiaozhi_audio.h"
#include "apps/xiaozhi/grok_protocol.h"
#undef private
#include "../main/apps/xiaozhi/app_xiaozhi.cc"

const char *const kNsXiaozhi="xiaozhi", *const kNsWebsocket="websocket";
std::string store_get_string(const char *,const char *,const char *v) {return v;}
int store_get_int(const char *,const char *,int v) {return v;}
bool store_set_string(const char *,const char *,const std::string &) {return true;}
bool store_set_int(const char *,const char *,int) {return true;}
bool store_erase(const char *,const char *) {return true;}
static int aborts=0,listen_starts=0,record_starts=0,commits=0;
static bool key_present=true;
static int connection_reads=0;
static bool socket_alive=true;
static int connection_starts=0;
static bool session_allocation_ok=true;
static bool audio_init_ok=true;
static int stop_requests=0;
static bool idle_ui_saw_playback=false;
static std::string status;
namespace xiaozhi {
Protocol::Protocol() {}
Protocol::~Protocol() {}
bool Protocol::IsConnected() const {++connection_reads;return client_!=nullptr && socket_alive;}
bool Protocol::Start(const std::string &,const char *,uint16_t) {++connection_starts;client_=reinterpret_cast<void *>(1);needs_restart_=false;hello_ok_=false;state_=State::Connecting;return true;}
void Protocol::Stop() {client_=nullptr;hello_ok_=false;state_=State::Disconnected;needs_restart_=true;}
void Protocol::SetState(State s) {state_=s;}
bool Protocol::PopCriticalEvent(Event *) {return false;}
bool Protocol::ConfigureSession() {return true;}
bool Protocol::TriggerStartListen() {++listen_starts;state_=State::Listening;return true;}
bool Protocol::TriggerStopListen() {++commits;state_=State::Thinking;return true;}
bool Protocol::TriggerAbort() {++aborts;state_=State::Idle;return true;}
bool Protocol::SendAudio(const uint8_t *,size_t) {return true;}
AudioService::AudioService() {}
AudioService::~AudioService() {}
AudioService &AudioService::GetInstance() {static AudioService a;return a;}
void AudioService::RequestStop() {++stop_requests;}
esp_err_t AudioService::Init() {return audio_init_ok?ESP_OK:ESP_ERR_NO_MEM;}
bool AudioService::Deinit() {return true;}
bool AudioService::PrepareSessionResources() {return session_allocation_ok;}
bool AudioService::ReleaseSessionResources() {return true;}
bool AudioService::StartRecording() {++record_starts;return true;}
void AudioService::StopRecording() {}
bool AudioService::StartPlayback() {is_playing_=true;return true;}
void AudioService::StopPlayback() {is_playing_=false;}
void AudioService::AbortPlayback() {is_playing_=false;}
bool AudioService::PopEncodedFrame(uint8_t *,size_t,size_t *) {return false;}
bool AudioService::HasPendingPlayback() const {return false;}
void AudioService::SetPlaybackSampleRate(int) {}
void ui_init() {} void ui_show() {} void ui_hide() {}
void ui_set_state(State state) {if(state==State::Idle)idle_ui_saw_playback=AudioService::GetInstance().IsPlaying();}
void ui_set_emotion(const char *) {} void ui_set_status_text(const std::string &s) {status=s;}
void ui_set_user_text(const std::string &) {} void ui_set_ai_text(const std::string &) {}
void ui_append_ai_text(const std::string &) {}
void ui_set_hint(const std::string &) {} void ui_clear_text() {} void ui_set_volume(unsigned) {}
}
bool grok_copy_key(char *out, size_t cap) {
    const char *key="xai-test-key-0123456789";
    if (!key_present || cap<=std::strlen(key)) return false;
    std::memcpy(out, key, std::strlen(key)+1);
    return true;
}
void grok_proxy_get(char *host, size_t cap, uint16_t *port) {
    if (host && cap) host[0]='\0';
    if (port) *port=0;
}

static void connected(bool ready) {
    proto().client_=reinterpret_cast<void *>(1);proto().hello_ok_=true;
    proto().needs_restart_=false;socket_alive=true;s_listen_after_connect=false;
    s_session_ready=ready;s_audio_ready=true;s_retry_reason.clear();
    s_reconnect_at_ms=0;s_next_connect_attempt_ms=0;s_reconnect_delay_ms=kReconnectDelayMs;
}
int main() {
    s_cmd_q=xQueueCreateStatic(kCmdQueueDepth,sizeof(Cmd),s_cmd_items,&s_cmd_queue_storage);
    s_app_active=true;s_stop_requested=false;
    s_cur_state=State::Speaking;xiaozhi::AudioService::GetInstance().is_playing_=true;
    on_state_changed(State::Idle);assert(!idle_ui_saw_playback);
    connected(true);proto().SetState(State::Thinking);on_state_changed(State::Thinking);
    mock_time+=1000;proto().SetState(State::Speaking);on_state_changed(State::Speaking);
    assert(s_thinking_since_ms==0);
    // Ten minutes of actual controller tick calls with audio progress must not
    // hit the old 25-second or 90-second absolute reply deadline.
    for(int i=0;i<30;++i) {
        mock_time+=20000;xiaozhi::AudioService::GetInstance().playback_progress_.fetch_add(1);tick();
        assert(aborts==0);
    }
    mock_time+=30001;tick();assert(aborts==1);
    connected(true);proto().SetState(State::Thinking);on_state_changed(State::Thinking);
    mock_time+=25001;tick();assert(aborts==1);
    mock_time+=25000;tick();assert(aborts==2);
    // A stale thinking timestamp cannot abort an already-Speaking protocol,
    // even before the controller has processed the state transition.
    connected(true);proto().SetState(State::Speaking);s_thinking_since_ms=mock_time-30000;
    s_speaking_since_ms=mock_time;tick();assert(aborts==2);

    connected(false);do_start_listen();assert(record_starts==0 && listen_starts==0);
    connected(true);proto().hello_ok_=false;do_start_listen();assert(record_starts==0);
    connected(true);proto().SetState(State::Idle);do_start_listen();assert(record_starts==1);
    const Event hello{EvType::HelloOk,State::Idle,{}};
    session_allocation_ok=false;handle_proto_event(hello);
    assert(!s_session_ready && s_audio_recovery_required && s_reconnect_at_ms>mock_time);
    on_state_changed(proto().GetState());assert(status=="音频内存不足，自动重试");
    do_start_listen();assert(record_starts==1);
    session_allocation_ok=true;connected(false);handle_proto_event(hello);
    assert(s_session_ready);
    xQueueReset(s_cmd_q);s_app_active=true;s_protocol_ready=true;
    const int input_reads=connection_reads;
    xiaozhi_app_on_key(BSP_BTN_OK,BTN_EVT_CLICK,0);
    assert(connection_reads==input_reads); // input never reads a mutable WS handle
    Cmd button;assert(xQueueReceive(s_cmd_q,&button,0));assert(button==Cmd::ToggleListen);
    post_cmd(Cmd::Stop);assert(s_stop_requested && stop_requests==1);
    xQueueReset(s_cmd_q);s_stop_requested=false;

    // Exercise the first do_connect allocation failure, the scheduled retry,
    // and eventual success through the production command handler.
    proto().Stop();s_audio_ready=false;s_session_ready=false;
    s_audio_recovery_required=false;s_connect_failures=0;
    audio_init_ok=false;do_connect();
    assert(!s_audio_ready && !s_session_ready);
    assert(status=="音频启动失败，自动重试" && s_reconnect_at_ms>mock_time);
    audio_init_ok=true;mock_time=s_reconnect_at_ms+1;tick();
    Cmd retry;assert(xQueueReceive(s_cmd_q,&retry,0));assert(retry==Cmd::Connect);
    handle_cmd(retry);assert(s_audio_ready && s_connect_pending && proto().IsConnected());
    proto().hello_ok_=true;handle_proto_event(hello);assert(s_session_ready);

    // Server idle shutdown leaves a handle but no running socket. User OK
    // must rebuild immediately, then start recording once the new hello and
    // audio resources are ready, without leaving/reopening the application.
    connected(true);proto().needs_restart_=true;proto().hello_ok_=false;
    proto().SetState(State::Disconnected);socket_alive=false;s_connect_failures=0;
    const int starts_before=connection_starts, recordings_before=record_starts;
    handle_proto_event(Event{EvType::Disconnected,State::Disconnected,{}});
    handle_cmd(Cmd::ToggleListen);
    assert(connection_starts==starts_before+1 && s_listen_after_connect);
    assert(record_starts==recordings_before && !s_session_ready);
    // Repeated presses while the SDK is reconnecting retain one wake request.
    handle_cmd(Cmd::ToggleListen);assert(connection_starts==starts_before+1);
    socket_alive=true;proto().hello_ok_=true;proto().SetState(State::Idle);
    handle_proto_event(hello);
    assert(record_starts==recordings_before+1 && !s_listen_after_connect);
    handle_proto_event(hello);assert(record_starts==recordings_before+1);

    // Background reconnection must stay silent until the user asks to talk.
    connected(true);proto().needs_restart_=true;proto().hello_ok_=false;socket_alive=false;
    handle_proto_event(Event{EvType::Disconnected,State::Disconnected,{}});
    do_connect();socket_alive=true;proto().hello_ok_=true;proto().SetState(State::Idle);
    handle_proto_event(hello);assert(record_starts==recordings_before+1);
    // goodbye can arrive before TCP closes; a live socket must still be retired.
    connected(true);proto().needs_restart_=true;proto().hello_ok_=false;
    const int before_goodbye=connection_starts;
    handle_cmd(Cmd::ToggleListen);assert(connection_starts==before_goodbye+1);
    assert(s_listen_after_connect && record_starts==recordings_before+1);
    // A resource failure after hello cannot start the microphone.
    session_allocation_ok=false;proto().hello_ok_=true;proto().SetState(State::Idle);
    handle_proto_event(hello);assert(record_starts==recordings_before+1);
    session_allocation_ok=true;
    do_stop_app();assert(!s_listen_after_connect);

    connected(true);s_reconnect_delay_ms=6000;
    for (int delay : {6000,12000,24000,48000,60000,60000}) {
        s_reconnect_at_ms=0;schedule_reconnect(kReconnectDelayMs);
        assert(s_reconnect_at_ms==mock_time+delay);
        schedule_reconnect(kReconnectDelayMs);assert(s_reconnect_at_ms==mock_time+delay);
    }
    handle_proto_event(hello);assert(s_reconnect_delay_ms==6000);
    proto().SetState(State::Listening);on_state_changed(State::Listening);
    const int before_listen_timeout=aborts;
    mock_time+=30000;tick();assert(aborts==before_listen_timeout+1);
    assert(!s_conversation_active && s_listening_since_ms==0);

    // Explicit double-click intent bypasses a full ordinary command queue,
    // waits for Hello/audio, and is idempotent throughout an active turn.
    xQueueReset(s_cmd_q);
    for(int i=0;i<kCmdQueueDepth;++i) post_cmd(Cmd::Connect);
    connected(false);proto().Stop();s_connect_failures=0;s_audio_recovery_required=false;
    const int before_auto=record_starts,before_connect=connection_starts;
    xiaozhi_app_request_conversation();process_conversation_request();
    assert(connection_starts==before_connect+1&&s_listen_after_connect);
    assert(record_starts==before_auto&&!s_conversation_requested);
    xiaozhi_app_request_conversation();process_conversation_request();
    assert(connection_starts==before_connect+1&&record_starts==before_auto);
    session_allocation_ok=false;proto().hello_ok_=true;proto().SetState(State::Idle);
    handle_proto_event(hello);assert(record_starts==before_auto&&s_listen_after_connect);
    session_allocation_ok=true;do_connect();proto().hello_ok_=true;proto().SetState(State::Idle);
    handle_proto_event(hello);assert(record_starts==before_auto+1&&!s_listen_after_connect);
    const int before_repeat_abort=aborts;
    for(State st:{State::Listening,State::Thinking,State::Speaking}){
        proto().SetState(st);xiaozhi_app_request_conversation();process_conversation_request();
        assert(record_starts==before_auto+1&&aborts==before_repeat_abort);
    }
    proto().SetState(State::Idle);xiaozhi_app_request_conversation();process_conversation_request();
    assert(record_starts==before_auto+2);
    xiaozhi_app_request_conversation();s_control_running=true;
    xiaozhi_app_prepare_stop();assert(!s_conversation_requested&&!s_app_active);
    s_listen_after_connect=true;proto().SetState(State::Idle);handle_proto_event(hello);
    xiaozhi_app_request_conversation();process_conversation_request();
    assert(record_starts==before_auto+2&&!s_conversation_requested);
    do_stop_app();assert(!s_listen_after_connect);
    // A missing key stays on the setup hint and never opens a socket.
    key_present=false;proto().Stop();s_audio_ready=false;s_session_ready=false;
    s_stop_requested=false;s_connect_failures=0;s_reconnect_at_ms=0;
    const int before_missing=connection_starts;
    do_connect();
    assert(connection_starts==before_missing && !s_audio_ready && status.find("密钥")!=std::string::npos);
    key_present=true;s_reconnect_at_ms=0;do_connect();
    assert(connection_starts==before_missing+1 && s_audio_ready);
    connected(true);proto().SetState(State::Listening);s_cur_state=State::Listening;
    const int aborts_before_commit=aborts;
    do_stop_listen();
    assert(commits==1 && aborts==aborts_before_commit && proto().GetState()==State::Thinking);
    do_stop_app();
    s_control_running=false;s_app_active=true;xQueueReset(s_cmd_q);
    s_app_active=true;s_control_running=true;s_audio_ready=false;
    assert(!xiaozhi_app_prepare_stop());assert(s_stop_requested);
    s_control_running=false;
    assert(xiaozhi_app_prepare_stop());
    // Failed startup remains a usable retry page, rather than a frozen old UI.
    s_ctl_task=nullptr;s_app_active=false;mock_task_fail=true;
    xiaozhi_app_start();assert(s_start_failed&&!s_control_running&&status=="启动失败，请重试");
    mock_task_fail=false;xiaozhi_app_on_key(BSP_BTN_OK,BTN_EVT_CLICK,0);
    assert(!s_start_failed&&s_control_running&&s_app_active);
    s_control_running=false;s_app_active=false;s_ctl_task=nullptr;mock_pending_task=nullptr;
    assert(s_proto_q==&s_proto_queue_storage && s_cmd_q==&s_cmd_queue_storage);
    assert(s_stopped_sem==&s_stopped_storage);
    xQueueReset(s_proto_q);xQueueReset(s_cmd_q);
    std::puts("Grok controller: long playback, readiness, missing key, push-to-talk PASS");
}
