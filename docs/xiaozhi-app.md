<p align="right">
  <a href="xiaozhi-app.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Voice chat audio architecture notes

This page keeps the ESP32-C3 half-duplex Opus memory and threading constraints that still apply.

The current voice chat app is **Grok**. Protocol and setup live in [grok-voice.md](grok-voice.md). The historical Chinese write-up that used the name 「小智」 during the original port is at [xiaozhi-app.zh_CN.md](xiaozhi-app.zh_CN.md). On-device UI no longer uses that name.

## What still matters

- One control task owns protocol Start/Stop, session updates, and audio Start/Stop.
- WebSocket callbacks only post events or enqueue Opus frames.
- A resident ~20 KB audio worker encodes and decodes half-duplex Opus.
- The Opus encoder is kept resident because the heap fragments after TLS.
- Leaving the app must `Deinit()` audio so other apps recover contiguous heap.
- Capture and playback are 24 kHz mono Opus.

## Current sources

| File | Role |
| --- | --- |
| `app_xiaozhi.cc` | Control task, state machine, keys (registry symbols stay `xiaozhi_*`) |
| `grok_protocol.{h,cc}` | xAI realtime WebSocket, session.update, commit/cancel |
| `grok_config.{h,cc}` | NVS namespace `grok` for API key and optional proxy |
| `grok_tunnel.{h,cc}` | Optional HTTP CONNECT proxy before TLS |
| `xiaozhi_audio.{h,cc}` | ES8311 half-duplex Opus worker |
| `xiaozhi_ui.{h,cc}` | LVGL UI labeled **Grok** |

OTA (`xiaozhi_ota.*`) and the old XiaoZhi WebSocket protocol (`xiaozhi_protocol.*`) are removed.
