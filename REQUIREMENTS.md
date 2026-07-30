# 蓝牙AI智能音箱 — 需求规格说明书

> 平台：香橙派 Zero 3（ARM64） | 语言：C | 版本：v2.0

---

## 1. 产品定位

一款基于 ARM Linux 的蓝牙 AI 智能音箱，支持手机蓝牙音乐播放、AI 文字对话、语音唤醒交互三大核心功能。

| 场景 | 描述 | 优先级 | 状态 |
|------|------|:------:|:----:|
| 蓝牙音响 | 手机连接音箱播放音乐（A2DP Sink） | P0 | ✅ |
| AI 文字助手 | 蓝牙串口发送文字 → AI 回复 → TTS 播报 | P0 | ✅ |
| AI 语音聊天 | 说话触发唤醒 → 录音 → STT → AI → TTS 播报 | P0 | ✅ |
| 断线重连 | 蓝牙断开后自动重连，指数退避 | P1 | ✅ |
| 手机语音输入 | 蓝牙连接后手机麦克风收音 → AI → 播报 | P1 | ❌ |

---

## 2. 硬件平台

| 组件 | 型号 | 状态 |
|------|------|:----:|
| SoC | 全志 H618 4×Cortex-A53 @1.5GHz | ✅ |
| 内存 | 1GB DDR3 | ✅ |
| 存储 | TF 卡 32GB | ✅ |
| 蓝牙/WiFi | 板载 RTL8821CS | ✅ |
| 音频 | USB 声卡（BH900 PRO） | ✅ |
| 麦克风 | USB 声卡内置 mic | ✅ |

---

## 3. 功能需求

### FR-01：蓝牙 SPP 通信

BlueZ D-Bus Profile1 vtable 注册 SPP 串口服务（UUID 00001101-...），手机通过蓝牙串口 App 发送文本命令。

- 蓝牙适配器初始化：打开、设名称 AI-Speaker、可发现 ✅
- SPP Profile 注册（Channel 1, AutoConnect） ✅
- NewConnection 回调接受连接，开线程读取 ✅
- 行缓冲处理蓝牙分包，按换行符切分 ✅
- 断开时发布 EV_BT_DEVICE_DISCONN ✅

### FR-02：蓝牙 A2DP 音乐播放

bluez-alsa 接收蓝牙音频流，PCM 环形缓冲解耦读写线程。

- 环形缓冲 16384 帧，互斥锁保护 ✅
- pcm_reader_thread 从 bluealsa 读取 PCM ✅
- playback_thread → audio_player_stream_write ✅
- TTS 播报时软件 ducking（PCM × 0.3） ✅
- TTS 时暂停 A2DP 流，播完恢复 ✅

### FR-03：ALSA 音频播放

WAV 文件播放 + 流式播放（A2DP/TTS 共用）。

- WAV 解析 → snd_pcm_writei ✅
- 异步播放线程，播完发 EV_AUDIO_PLAY_DONE ✅
- XRUN 自动恢复 ✅

### FR-04：录音与唤醒检测

USB 声卡录音，实时能量检测触发唤醒。

- capture 线程 snd_pcm_readi 循环（512 帧/次） ✅
- PCM 数据通过 EV_AUDIO_CAPTURE_DATA 发布 ✅
- 能量检测：abs 平均值超阈值（~3000）发 EV_WAKEUP_DETECTED ✅
- TTS 播报期间静音唤醒 ✅

### FR-05：事件总线

模块间 publish/subscribe 解耦通信。

- 环形缓冲队列（容量 64） ✅
- 订阅/退订，分发时拷贝列表 ✅
- 13 种事件类型，线程安全 ✅

### FR-06：表驱动状态机

| 状态 | 说明 |
|:----|:----|
| ST_IDLE | 空闲等待事件 |
| ST_MUSIC | A2DP 音乐播放中 |
| ST_PROCESSING | AI 请求处理中 |
| ST_SPEAKING | TTS 语音播报中 |
| **ST_LISTENING** | **v2.0：唤醒后录音中** |
| ST_EXITING | 关闭中 |

14 条转移规则覆盖所有路径。

### FR-07：AI 对话

DeepSeek API 同步调用，多轮历史管理。

- libcurl POST ✅
- 多轮对话：user/assistant 历史，保留 system prompt ✅
- Token 裁剪：超限时丢弃最旧轮次 ✅
- JSON 转义特殊字符 ✅

### FR-08：TTS 语音合成

腾讯云 TextToVoice API，TC3-HMAC-SHA256 签名。

- TC3-HMAC-SHA256 认证 ✅
- 腾讯云 API 调用 ✅
- base64 解码 → WAV 文件 ✅
- 异步播放 → 播完回调 ✅
- 文本截断：超 900 字节时安全截断，不切碎 UTF-8 ✅

### FR-09：STT 语音识别（v2.0 新增）

唤醒后录音 2.5 秒 → 百度 STT → DeepSeek AI → TTS。

- 录音缓冲区最大 3 秒（48000 帧） ✅
- 唤醒后自动录音，2.5 秒超时提交 ✅
- 百度 OAuth2.0 access_token 获取 ✅
- 百度 server_api（PCM 16kHz/16bit/mono base64） ✅
- 识别结果 → AI → TTS 全链路 ✅
- 未配置 Key 时降级"你好" ✅
- 自实现 base64 编码器 ✅

### FR-10：断线重连（v2.0 新增）

蓝牙 SPP 断开后 D-Bus Device1.Connect() 自动重连。

- 保存设备 BD 地址和 D-Bus 路径 ✅
- 指数退避 2s → 4s → 8s → 16s → 30s ✅
- 新连接到来时取消重连 ✅
- auto_reconnect 配置开关 ✅

---

## 4. 非功能需求

| 要求 | 指标 | 状态 |
|------|------|:----:|
| 编译警告 | 0（-Wall -Wextra -Werror） | ✅ |
| 内存泄漏 | Valgrind 零泄漏 | ✅ |
| 单元测试 | 25 个全部通过 | ✅ |
| 连续运行 | 7×24h 无崩溃 | ✅ |
| AI 应答延迟 | SPP 文字 ~2-3s / 语音唤醒 ~5-6s | ✅ |
| 启动时间 | ~2 秒 | ✅ |

---

## 5. 验收状态

| 验收项 | 判定标准 | 状态 |
|:------|:--------|:----:|
| SPP 通信 | 蓝牙串口发"你好"，音箱回复播报 | ✅ |
| A2DP 音乐 | 手机播放音乐，音箱出声 | ✅ |
| TTS 播报 | AI 回复完整播报 | ✅ |
| 唤醒 + STT | 拍手唤醒 → 说话 → 识别 → AI → 播报 | ✅ |
| 断线重连 | 关蓝牙再开，自动重连 | ✅ |
| 持续运行 | 24h 无崩溃 | ✅ |
| Valgrind | 零泄漏 | ✅ |

---

## 6. 技术栈

| 层面 | 技术 |
|:----|:----|
| 系统 | Linux (Armbian Debian) |
| 语言 | C11 |
| 音频 | ALSA (libasound2) |
| HTTP | libcurl |
| AI | DeepSeek API（同步 HTTP） |
| TTS | 腾讯云语音合成 |
| STT | 百度短语音识别 |
| 蓝牙 | BlueZ D-Bus (SPP) / bluez-alsa (A2DP) |
| 加密 | OpenSSL（TC3-HMAC-SHA256 签名） |
| 线程 | POSIX threads |
| 检测 | Valgrind |

---

## 7. 未实现说明

| 功能 | 原因 | 面试话术 |
|:----|:----|:--------|
| 流式 SSE | DeepSeek v4 流式有 reasoning_content 干扰 | 改用同步 API，架构已预留流式事件 |
| Porcupine 唤醒词 | Picovoice 试用 Key 过期 | 接口已封装在 wake_word.c |
| A2DP D-Bus vtable | H618 BlueZ 5.66 兼容性 bug | 改用 bluez-alsa |
