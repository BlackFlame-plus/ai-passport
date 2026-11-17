<p align="right">
  <a href="grok-voice.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Grok voice

The on-device voice chat app uses xAI's official Speech-to-Speech API. It no longer uses the XiaoZhi backend, OTA config fetch, or device binding codes.

## Setup

1. Create an API key in the [xAI Console](https://console.x.ai/).
2. Connect your phone to the device hotspot `Passport-WiFi-xxxx` (no password) and open `http://192.168.4.1`.
3. In the **Grok voice** section, enter the API key and save. The page then only shows that a key is saved; it never reads the key back.
4. After the device joins home Wi-Fi, open **Grok**. Press OK to start listening, press OK again to end the utterance and wait for a reply. Press OK during a reply to interrupt. Volume keys change volume; long-press OK returns to the launcher.

The key is stored only in NVS namespace `grok` under `api_key`. It is not baked into firmware, logs, or this repository.

If the network cannot reach `api.x.ai`, the same section accepts a LAN IPv4 and port for an HTTP proxy on a computer. The device issues `CONNECT api.x.ai:443`, then runs the usual TLS session inside that tunnel. Leave the fields empty for a direct connection.

Official voice usage is billed by the minute. Per current xAI docs, pricing is about USD 0.08 per minute. Treat the [Speech to Speech](https://docs.x.ai/developers/model-capabilities/audio/speech-to-speech) page as authoritative.

## Link choices

- WebSocket: `wss://api.x.ai/v1/realtime?model=grok-voice-latest`
- Auth: `Authorization: Bearer <key>`
- After connect, the control task sends `session.update`: Chinese `instructions`, voice `eve`, `turn_detection: null` (manual turns), input transcription `language_hint=zh`
- Both directions use `audio/opus`, 24 kHz, mono. Each binary WebSocket frame is one raw Opus packet. There is no XiaoZhi v3 frame header and no base64
- On release (OK again), the client sends `input_audio_buffer.commit` and `response.create`
- Interrupt sends `response.cancel`
- On-screen text comes from `conversation.item.input_audio_transcription.*` and `response.output_audio_transcript.*`

The audio thread model is unchanged: one control task, a resident 20 KB audio task created when the app starts, a resident Opus encoder, and socket callbacks that only enqueue. Details remain in [xiaozhi-app.md](xiaozhi-app.md).

## Build a full image

Requires ESP-IDF 5.5.3. In an activated IDF environment:

```bash
bash build.sh release
```

The script writes a merged image `FoloToy-AI-Passport-full.bin` under `build-public/`. Full-chip write address is `0x0`. When updating only the application partition, do not use `erase_flash`, and do not change the partition table, `cardid`, or Recovery at `0x700000`.
