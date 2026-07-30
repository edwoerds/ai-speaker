# AI-Speaker 蓝牙AI音箱

> 嵌入式 Linux 项目 | 纯 C 语言 | 香橙派 Zero 3

## 概述

蓝牙 AI 音箱，基于 Orange Pi Zero 3（ARM64 Linux）开发。
通过手机蓝牙 SPP 发送文字，调用 AI API 生成回复并语音播报。
支持 A2DP 蓝牙音乐播放、离线唤醒触发、TTS ducking 等完整音频功能。

**项目规模：** ~6000 行 C 代码，25 个单元测试，Valgrind 零泄漏。

---

## 硬件要求

- Orange Pi Zero 3（或任意 ARM64 Linux 开发板）
- USB 声卡（播放+录音，如 BH900 PRO）
- 麦克风（USB 声卡自带或外接）
- 手机（用于蓝牙 SPP 通信）

## 依赖安装

```bash
# Debian/Ubuntu
sudo apt install build-essential libasound2-dev libcurl4-openssl-dev \
                 libbluetooth-dev libsystemd-dev libssl-dev
```

## 构建

```bash
make clean && make
```

产物：`output/speaker`

## 运行测试

```bash
make test
```

运行 27 个单元测试，覆盖事件总线、状态机、线程池。

## 配置文件

配置文件支持通过 `-c` 参数指定，默认路径 `/etc/speaker.conf`。

```bash
cp speaker.conf.example /etc/speaker.conf
vi /etc/speaker.conf
```

### 配置项

| 段 | 字段 | 说明 |
|:---|:---|:---|
| `[ai]` | `api_key` | DeepSeek API Key |
| `[ai]` | `model` | 模型名（如 deepseek-v4-flash） |
| `[tts]` | `secret_id` | 腾讯云 SecretId |
| `[tts]` | `secret_key` | 腾讯云 SecretKey |
| `[tts]` | `voice_type` | 音色编号（0=晓薇） |
| `[stt]` | `api_key` | 百度语音识别 API Key（v2.0） |
| `[stt]` | `secret_key` | 百度语音识别 Secret Key（v2.0） |
| `[bluetooth]` | `device_name` | 蓝牙广播名称 |
| `[audio]` | `device` | ALSA PCM 设备名 |
| `[wake_word]` | — | 预留 Porcupine 配置段 |

## 运行

```bash
./output/speaker -c /etc/speaker.conf
```

运行后：
1. 手机连蓝牙 "AI-Speaker"
2. 通过蓝牙串口 App（如 Serial Bluetooth Terminal）发送文字
3. 音箱 AI 回复并语音播报
4. 手机可播放 A2DP 蓝牙音乐，TTS 播报时自动 ducking

## 语音唤醒

音箱支持两种唤醒方式：

**① 语音触发（v2.0 STT）**
拍手/敲桌子 → 能量检测触发 → 录音 2.5 秒 → 百度语音识别（STT） → AI 对话 → TTS 播报
配置百度 API Key 后在 `speaker.conf` 的 `[stt]` 段填写。

**② 蓝牙文字输入**
手机蓝牙串口 App 发送文字 → AI 对话 → TTS 播报

**③ 离线唤醒词（预留）**
Porcupine 引擎接口已封装在 `wake_word.c`，需 Picovoice AccessKey。
试用 Key 已验证全链路可用。详见 `inc/wake_word.h`。

## 断线重连（v2.0）

蓝牙 SPP 断开后自动重连，策略：2s → 4s → 8s → 16s → 30s（指数退避）。
在 `speaker.conf` 的 `[bluetooth]` 段配置 `auto_reconnect = true`。

## 项目结构

```
├── ARCHITECTURE.md       # 架构文档
├── Makefile               # 构建
├── speaker.conf.example   # 配置示例
├── inc/                   # 头文件
│   ├── common.h          # 公共类型、事件ID
│   ├── event_bus.h       # 事件总线
│   ├── state_machine.h   # 状态机
│   ├── thread_pool.h     # 线程池
│   ├── module.h          # 模块注册框架
│   ├── config.h          # 配置读取
│   ├── logger.h          # 日志
│   ├── bt_manager.h      # 蓝牙 SPP
│   ├── bt_a2dp.h         # 蓝牙 A2DP 音乐
│   ├── audio_player.h    # 音频播放
│   ├── alsa_capture.h    # 录音
│   ├── audio_mixer.h     # 软件混音
│   ├── audio_pipeline.h  # 音频管道
│   ├── ai_client.h       # AI 客户端 + TTS 配置
│   ├── ai_stt.h          # v2.0：语音识别
│   ├── voice_agent.h     # 主控状态机
│   └── wake_word.h       # 唤醒词引擎接口
├── src/                   # 源码
│   ├── main.c
│   ├── core/
│   ├── bluetooth/
│   ├── ai/
│   ├── audio/
│   └── utils/
└── tests/                 # 单元测试
    ├── test_event_bus.c
    ├── test_state_machine.c
    └── test_thread_pool.c
```

## 技术栈

| 层面 | 技术 |
|:---|:---|
| 系统 | Linux (Armbian Debian) |
| 语言 | C11 |
| 音频 | ALSA (`libasound2`) |
| HTTP | libcurl |
| AI | DeepSeek API (同步 HTTP) |
| TTS | 腾讯云语音合成 (HTTP API) |
| 蓝牙 | BlueZ D-Bus (SPP) / bluez-alsa (A2DP) |
| 加密 | OpenSSL (TC3-HMAC-SHA256 签名) |
| 线程 | POSIX threads |
| 内存检测 | Valgrind |
