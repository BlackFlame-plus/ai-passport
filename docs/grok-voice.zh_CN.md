<p align="right">
  <strong>简体中文</strong> · <a href="grok-voice.md">English</a>
</p>

# Grok 语音

设备上的语音对话应用走 xAI 官方 Speech-to-Speech 接口，不再使用小智后台、OTA 或设备绑定码。

## 准备

1. 在 [xAI Console](https://console.x.ai/) 申请自己的 API Key。
2. 手机连接设备热点 `Passport-WiFi-xxxx`（无密码），浏览器打开 `http://192.168.4.1`。
3. 在 **Grok 语音** 一栏填入 API Key 并保存。页面之后只显示「已保存」，不会把密钥再读出来。
4. 设备连上家庭 Wi-Fi 后，打开 **Grok**。按确定开始听，再按确定结束这句话并等待回复。回复过程中按确定会打断。音量键调节音量，长按确定回到启动器。

密钥只写在设备 NVS 命名空间 `grok` 的 `api_key` 里，不会写进固件、日志或这个仓库。

如果所在网络访问不了 `api.x.ai`，同一栏可以填电脑上 HTTP 代理的局域网 IPv4 和端口。设备会先 `CONNECT api.x.ai:443`，再在这条隧道里做原来的那一次 TLS。留空则直连。

官方语音按用量计费。按当前 xAI 文档，价格大约是每分钟 0.08 美元。以 [Speech to Speech](https://docs.x.ai/developers/model-capabilities/audio/speech-to-speech) 页面为准。

## 链路上的选择

- WebSocket：`wss://api.x.ai/v1/realtime?model=grok-voice-latest`
- 鉴权：`Authorization: Bearer <key>`
- 连上后由控制任务发送 `session.update`：中文 `instructions`、声音 `eve`、`turn_detection: null`（手动轮次）、输入转写 `language_hint=zh`
- 上下行都是 `audio/opus`、24 kHz、单声道。每个二进制 WebSocket 帧是一个原始 Opus 包，不再包小智的 v3 帧头，也不做 base64
- 松手（再次按确定）时发送 `input_audio_buffer.commit` 和 `response.create`
- 打断时发送 `response.cancel`
- 屏幕文字来自 `conversation.item.input_audio_transcription.*` 和 `response.output_audio_transcript.*`

音频线程模型没有改：一个控制任务、常驻 20 KB 音频任务在进应用时创建、常驻 Opus 编码器、socket 回调只入队。细节仍见 [xiaozhi-app.md](xiaozhi-app.md) / [xiaozhi-app.zh_CN.md](xiaozhi-app.zh_CN.md)。

## 编译完整镜像

需要 ESP-IDF 5.5.3。在已安装 IDF 的环境里：

```bash
bash build.sh release
```

脚本会在 `build-public/` 里生成合并镜像 `FoloToy-AI-Passport-full.bin`。整片写入地址是 `0x0`。这只更新应用分区时不要用 `erase_flash`，也不要改分区表、`cardid` 或 `0x700000` 的 Recovery。
