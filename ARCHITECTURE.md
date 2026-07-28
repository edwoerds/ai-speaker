# AI 智能音箱 -- 企业级架构设计

> 版本：v3.1 | 日期：2026-07-18

---

## 目录

1. [架构总览](#一架构总览)
2. [模块接口契约](#二模块接口契约)
   - 2.1 voice_agent
   - 2.2 bt_manager
   - 2.3 ai_client
   - 2.4 ai_conv
   - 2.5 ai_tts
   - 2.6 audio_player
   - 2.7 bt_a2dp
   - 2.8 alsa_capture
   - 2.9 audio_mixer
   - 2.10 audio_pipeline
   - 2.11 wake_word
3. [线程模型](#三线程模型)
4. [事件 ID 全集](#四事件-id-全集)
5. [文件组织](#五文件组织)
6. [错误处理标准](#六错误处理标准)
7. [编码规范](#七编码规范)
8. [开发路线图](#八开发路线图)
9. [质量检查项](#九质量检查项)

---

## 一、架构总览

项目采用**四层架构**，模块间通过事件总线解耦，遵守以下核心原则：

- 模块间不直接调用，不 include 对方头文件
- 只通过 `event_bus.subscribe()` / `publish()` 通信
- AI 请求通过 `thread_pool.submit()` 异步执行
- 状态机是唯一编排器，不直调任何服务层模块

```
                              ┌──────────────────────────────────────────────┐
                              │         应用层 Application（PRIO 20）         │
                              │                                              │
                              │  voice_agent.c    主控状态机，项目唯一编排器   │
                              │  职责：状态表驱动、事件路由、任务调度、        │
                              │        超时监控、错误恢复                     │
                              └──────────┬───────────────────────────────────┘
                                         │
                              event_bus publish / subscribe
                                         │
         ┌───────────────────────────────┼───────────────────────────────────┐
         │         服务层 Service（PRIO 10）                                 │
         │                                                                   │
         │  ┌─────────────┐  ┌───────────┐  ┌──────────┐  ┌──────────────┐  │
         │  │  bt_manager  │  │ ai_client  │  │ ai_tts   │  │ audio_player │  │
         │  │ - BlueZ D-Bus│  │ - libcurl  │  │ - 腾讯云  │  │  - ALSA PCM  │  │
         │  │ - SPP vtable │  │ - 同步API   │  │ - TC3-HMAC│  │  - WAV/PCM   │  │
         │  │ - D-Bus 分发  │  │ - 重试+降级 │  │ - base64  │  │  - underrun  │  │
         │  └─────────────┘  └───────────┘  └──────────┘  └──────────────┘  │
         │                                                                   │
         │  ┌─────────────┐  ┌───────────┐  ┌────────────┐  ┌─────────────┐ │
         │  │  bt_a2dp     │  │ ai_conv   │  │alsa_capture│  │audio_mixer  │ │
         │  │ - A2DP Sink  │  │ - 对话历史 │  │ - USB mic  │  │ - 软件混音  │ │
         │  │ - PCM 环形   │  │ - Token裁  │  │ - 16kHz/16 │  │ - 集中duck  │ │
         │  │ - TTS ducking│  │ - JSON构建 │  │ - 能量检测  │  │ - 增益管理  │ │
         │  └─────────────┘  └───────────┘  └────────────┘  └─────────────┘ │
         │  ┌────────────────────────────────────────────────────────────────┐│
         │  │                  audio_pipeline（统一调度入口）                ││
         │  │       播放 / 录音 / A2DP / ducking 协调                       ││
         │  └────────────────────────────────────────────────────────────────┘│
         └───────────────────────────────────────────────────────────────────┘
                                         │
                             唯一通信通道: event_bus
                                         │
         ┌────────────────────────────────────────────────────────────────────┐
         │         事件总线 Event Bus（PRIO 0）                              │
         │                                                                   │
         │  event_bus.c    模块间唯一通信通道                                 │
         │  机制：环形缓冲队列 + 订阅链表 + mutex + cond var                  │
         │  分发：独立线程串行 dispatch，先拷贝订阅列表再分发（防死锁）         │
         └────────────────────────────────────────────────────────────────────┘
                                         │
                             为上层提供能力
                                         │
         ┌────────────────────────────────────────────────────────────────────┐
         │         框架层 Framework（PRIO 0）                                │
         │                                                                   │
         │  state_machine.c   thread_pool.c    module.c       common.h       │
         │  表驱动状态转移     异步任务执行      生命周期管理    公共类型       │
         │  - sm_init/dispatch - submit/worker  - constructor  - err_t       │
         │  - void*data 传递   - 队列深度监控    - 失败回滚    - event_id     │
         └────────────────────────────────────────────────────────────────────┘
```

---

## 二、模块接口契约

### 2.1 voice_agent（应用层，PRIO 20）

**设计说明：** 整个系统的唯一编排器，基于状态表驱动的事件响应。不提供外部 API——全部通过 event_bus 驱动，模块注册由 `MODULE_DEFINE` 完成，`main` 不需要显式调用任何初始化函数。

**状态常量：**

| 状态 | 值 | 说明 |
|------|:--:|------|
| `ST_IDLE` | 0 | 空闲，等待事件 |
| `ST_MUSIC` | 1 | A2DP 音乐播放中 |
| `ST_PROCESSING` | 2 | AI 请求处理中 |
| `ST_SPEAKING` | 3 | TTS 播报中 |
| `ST_EXITING` | 4 | 收到关闭信号 |

**初始化流程（实际顺序）：**

1. 初始化 AI 客户端（`ai_client_init`）—— api_url 不传则默认 `https://api.deepseek.com/chat/completions`
2. 初始化 TTS（`ai_tts_init`）—— 从配置读取 secret_id / secret_key / voice_type
3. 初始化对话管理（`ai_conv_init`）—— 设置 system prompt
4. 初始化音频管道（`audio_pipeline_init`）—— 内部链式 init：mixer → player → a2dp → capture
5. 启动录音（`audio_pipeline_capture_start`）—— 用于唤醒能量检测
6. 初始化状态机（`sm_init`）—— 12 条转移，初始状态 `ST_IDLE`
7. 订阅 11 个事件：`EV_BT_DATA_RECEIVED`、`EV_WAKEUP_DETECTED`、`EV_AI_RESP_READY`、`EV_AUDIO_PLAY_DONE`、`EV_AUDIO_MUSIC_START`、`EV_AUDIO_MUSIC_STOP`、`EV_SYS_SHUTDOWN`、`EV_AI_ERROR`、`EV_AUDIO_ERROR`、`EV_BT_DEVICE_DISCONN`、`EV_SYS_ERROR`

**反初始化：** 按 init 逆序释放：停止录音 → 反初始化音频管道 → 对话管理 → TTS → AI 客户端

**状态转移表（12 条）：**

| 当前状态 | 事件 | 下一状态 | 动作 |
|----------|------|:--------:|------|
| `ST_IDLE` | `EV_BT_DATA_RECEIVED` | `ST_PROCESSING` | `do_ai_request` |
| `ST_IDLE` | `EV_WAKEUP_DETECTED` | `ST_PROCESSING` | `do_wake_response`（发送"你好"） |
| `ST_IDLE` | `EV_AUDIO_MUSIC_START` | `ST_MUSIC` | `on_music_start` |
| `ST_MUSIC` | `EV_AUDIO_MUSIC_STOP` | `ST_IDLE` | `on_music_stop` |
| `ST_MUSIC` | `EV_BT_DATA_RECEIVED` | `ST_PROCESSING` | `do_ai_request`（同时 duck 音乐音量） |
| `ST_PROCESSING` | `EV_AI_RESP_READY` | `ST_SPEAKING` | `do_tts_and_play`（同时 duck 音乐音量） |
| `ST_PROCESSING` | `EV_AI_ERROR` | `ST_IDLE` | `on_error`（恢复音乐播放） |
| `ST_SPEAKING` | `EV_AUDIO_PLAY_DONE` | `ST_IDLE` | `on_play_done`（恢复唤醒检测 + 音乐） |
| `ST_SPEAKING` | `EV_AUDIO_ERROR` | `ST_IDLE` | `on_error` |
| `ST_IDLE` | `EV_BT_DEVICE_DISCONN` | `ST_IDLE` | NULL（不处理） |
| `ST_ANY` | `EV_SYS_SHUTDOWN` | `ST_EXITING` | `on_shutdown` |
| `ST_ANY` | `EV_SYS_ERROR` | `ST_IDLE` | `on_error` |

**动作说明：**

- `do_ai_request` —— 从事件数据中提取文本 → `ai_conv_append("user", text)` → `ai_conv_build_payload` → `thread_pool_submit(ai_request_task, ...)` 异步执行 AI 请求。处期间通过 `alsa_capture_set_wake_muted(true)` 静音唤醒检测。
- `do_wake_response` —— 发送默认文本"你好"发起 AI 请求，同 `do_ai_request` 流程。
- `do_tts_and_play` —— `thread_pool_submit(tts_task, ...)` 异步执行 TTS 合成 → `audio_pipeline_play_tts` 播放。
- `on_play_done` / `on_error` —— 恢复唤醒检测 `alsa_capture_set_wake_muted(false)`。

**状态超时监控：**

- `ST_PROCESSING` 超过 30s —— 自动回退到 `ST_IDLE`，发布 `EV_AI_ERROR`
- `ST_SPEAKING` 超过 60s —— 自动回退到 `ST_IDLE`，发布 `EV_AUDIO_ERROR`

---

### 2.2 bt_manager（服务层，PRIO 10）

**职责：** 蓝牙 SPP 服务管理，通过 BlueZ D-Bus Profile1 vtable 机制实现 SPP 通道，与手机 APP 建立双向数据通道。

**初始化流程：**

1. `sd_bus_open_system` 建立系统 D-Bus 连接
2. 设置 Adapter1 属性：`Powered=1`、`Alias`（设备名）、`Discoverable=1`
3. 注册 `org.bluez.Profile1` vtable（NewConnection / RequestDisconnection / Release 回调），对象路径 `/com/aispeaker/spp`
4. 启动 D-Bus 分发线程（`sd_bus_process` 轮询）
5. 调用 `org.bluez.ProfileManager1.RegisterProfile` 注册 SPP（UUID `00001101-0000-1000-8000-00805F9B34FB`，Channel 1，AutoConnect）

**连接处理（BlueZ 回调驱动，无 accept 循环）：**

BlueZ 检测到手机连接时，通过 D-Bus 调用 `NewConnection` 回调，传入已建立的 socket fd。bt_manager 在该回调中：

- `dup(fd)` 获取独立的文件描述符
- `pthread_create` 创建读线程独立处理
- 立即回复 BlueZ 不阻塞

读线程流程：`read` 逐字节拼接到行缓冲 → 遇到 `\n` 或 `\r` 以行发布 `EV_BT_DATA_RECEIVED` → 断开时关闭 fd 发布 `EV_BT_DEVICE_DISCONN`。

**反初始化：**
设置 `running = false` → `pthread_join` 分发线程 → Adapter1 `Powered=0` → `sd_bus_unref`

**设计说明：**
- 蓝牙硬件不可用时（如无板载蓝牙），bt_manager 返回 `ERR_OK` 并记录 WARN，不阻塞系统启动
- 无指数退避重连逻辑——init 时若 D-Bus 连接失败直接报错，不重试
- 不处理 A2DP，A2DP 由 `bt_a2dp` + `bluez-alsa` 独立处理

**发布事件：**
`EV_BT_DEVICE_CONN`、`EV_BT_DEVICE_DISCONN`、`EV_BT_DATA_RECEIVED`

---

### 2.3 ai_client（服务层，PRIO 10）

**职责：** 封装 libcurl HTTP 请求，向 LLM API（OpenAI 兼容接口）发送对话并获取回复。

**调用方式：** 当前使用**同步（非流式）API** `chat_sync`，原因是 DeepSeek v4-flash 的 `reasoning_content` 字段会干扰 SSE 流式解析，改用同步请求一次性返回完整结果，由 `voice_agent` 通过 `thread_pool` 异步执行以免阻塞主线程。流式 `chat` 接口保留在代码中，供后续兼容的模型使用。

**配置结构（`ai_client_config_t`，定义在 `ai_client.h`）：**

```c
typedef struct {
    const char *api_url;       /* API 端点（NULL 默认 https://api.deepseek.com/chat/completions） */
    const char *api_key;       /* API Key */
    const char *model;         /* 模型名 */
    int         timeout_ms;    /* HTTP 超时（默认 30000） */
    float       temperature;   /* 生成温度（0.0~2.0） */
    int         max_tokens;    /* 最大生成 token 数 */
} ai_client_config_t;
```

**接口说明：**

- `chat_sync(json_payload, &response)` —— **主用接口。** 同步阻塞，libcurl POST 发起请求，等待完整 JSON 响应后解析返回。调用方通过 `thread_pool_submit` 异步执行，不阻塞事件循环。内置重试（最多 3 次，间隔 200ms）。失败返回 `ERR_GENERAL`。
- `chat(json_payload, callback, user_data)` —— **备用接口（当前未用）。** 同步阻塞 + SSE 流式解析。`voice_agent` 当前用同步 API，此接口保留供后续兼容模型切换。

**反初始化：** `curl_global_cleanup()`

---

### 2.4 ai_conv（服务层，PRIO 10）

**职责：** 管理多轮对话历史，自动裁剪 token 长度，构建符合 API 要求的 JSON payload。

| 方法 | 行为 |
|------|------|
| `init(system_prompt)` | 保存系统提示词，初始化 history 环形缓冲区（容量 11 条） |
| `append(role, content)` | 超长内容截断；历史满时删除最旧的一对问答再追加 |
| `build_payload()` | 遍历历史，转义 `"` 和 `\`，返回 malloc 分配的 JSON 字符串 |
| `clear()` | 保留 system prompt，清空 history[1..] |
| `deinit()` | 释放全部动态内存 |

---

### 2.5 ai_tts（服务层，PRIO 10）

**职责：** 调用腾讯云语音合成（TTS）API，将文字合成为 WAV 音频文件。使用 TC3-HMAC-SHA256 签名认证，纯 libcurl + OpenSSL 实现，无外部进程依赖。

**配置结构（`ai_tts_config_t`，定义在 `ai_client.h`）：**

```c
typedef struct {
    const char *secret_id;      /* 腾讯云 SecretId */
    const char *secret_key;     /* 腾讯云 SecretKey */
    int         voice_type;     /* 音色编号（0=晓薇, 1=晓晓, 101001=智逸等） */
    const char *output_dir;     /* WAV 输出目录（默认 /tmp/speaker） */
} ai_tts_config_t;
```

**接口说明：**

| 方法 | 行为 |
|------|------|
| `init(cfg)` | 保存配置，在 output_dir 创建输出目录 |
| `speak(text, &out_path)` | ① 构建 JSON 请求体（含 JSON 转义）→ ② 计算 TC3-HMAC-SHA256 签名（HMAC-SHA256 嵌套三层）→ ③ 组装请求头（Authorization / X-TC-Timestamp / X-TC-Version / X-TC-Region / X-TC-Action）→ ④ libcurl POST 到 `tts.tencentcloudapi.com` → ⑤ 解析 JSON 响应提取 `Audio` base64 字段 → ⑥ base64 解码 → ⑦ 写入 WAV 文件 → `*out_path = strdup(wav_path)`，调用方负责 free。失败返回 `ERR_GENERAL` |
| `deinit()` | 释放资源 |

**endpoint 常量：** `tts.tencentcloudapi.com`，`X-TC-Action: TextToVoice`，`X-TC-Version: 2019-08-23`，`Codec: wav`

---

### 2.6 audio_player（服务层，PRIO 10）

**职责：** ALSA PCM 音频播放，支持 WAV 文件播放和 PCM 流式播放。

**配置结构（`audio_player_config_t`，定义在 `audio_player.h`）：**

```c
typedef struct {
    const char *device;   /* ALSA PCM 设备名（"default", "plughw:0,0" 等） */
} audio_player_config_t;
```

**接口说明：**

| 方法 | 行为 |
|------|------|
| `init(cfg)` | 保存 ALSA 设备名 |
| `play(filepath)` | **阻塞**播放 WAV 文件，播放完才返回（内部 PCM 协商 + `snd_pcm_writei` 循环 + drain） |
| `play_async(filepath)` | **异步**播放，创建独立线程执行 `play`，完成发布 `EV_AUDIO_PLAY_DONE`，错误发布 `EV_AUDIO_ERROR`；正在播放时返回 `ERR_BUSY` |
| `stop()` | `snd_pcm_drop` 踢醒播放线程 |
| `stream_start(rate, channels)` | 开始流式播放模式（供 A2DP 连续 PCM 播放用） |
| `stream_write(data, frames)` | 写入 PCM 帧（16bit 立体声交错），阻塞直到 ALSA 缓冲区可写 |
| `stream_stop()` | 停止流式播放，关闭 ALSA 设备 |
| `deinit()` | 停止播放 → 等待线程退出 → 关闭 PCM 句柄 |

---

### 2.7 bt_a2dp（服务层，PRIO 10）

**职责：** A2DP Sink 实现，通过 bluez-alsa 接收手机蓝牙音乐，PCM 环形缓冲播放到 ALSA 输出。

**配置结构（`bt_a2dp_config_t`，定义在 `bt_a2dp.h`）：**

```c
typedef struct {
    const char *device;           /* ALSA PCM 设备名 */
    int         ringbuf_frames;   /* 环形缓冲容量（帧数，默认 256） */
} bt_a2dp_config_t;
```

**初始化流程：**

1. 注册 BlueZ A2DP Sink Profile（D-Bus MediaEndpoint1）
2. 启动 ALSA 播放线程（从环形缓冲读取数据播放到 audio_player）
3. 自动发现 bluealsa 设备（`bluealsa-aplay -L`）

**环形缓冲设计：**

- 容量：16384 帧（每帧 16bit × 2ch = 4 字节，约 370ms @ 44100Hz）
- 满时：丢弃最老帧（覆盖旧数据），不阻塞 bluealsa 发射端
- 空时：ALSA 播放静音帧（防止 underrun）

**实现说明：** 原计划手写 BlueZ D-Bus A2DP vtable，但 H618 平台存在 PDU malformed 兼容性 bug，改用 `bluez-alsa` 方案。D-Bus vtable 方案留待以后研究。

**Ducking 控制：**

TTS 播报时通过 `bt_a2dp_set_ducking(true)` 触发 PCM 采样值 ×0.3 软件衰减，`bt_a2dp_set_ducking(false)` 恢复 ×1.0。USB 声卡无硬件混音器，不引入 dmix 插件，CPU 增加约 2%。

`bt_a2dp_set_pause(true)` 在 TTS 期间禁止 A2DP 重启流，避免抢声卡。

**发布事件：** `EV_AUDIO_MUSIC_START`、`EV_AUDIO_MUSIC_STOP`

---

### 2.8 alsa_capture（服务层，PRIO 10）

**职责：** USB 声卡录音模块，从麦克风录制 PCM 音频（16kHz / 16bit / mono），录音数据通过事件总线发布，同时内建唤醒能量检测。

**配置结构（`alsa_capture_config_t`，定义在 `alsa_capture.h`）：**

```c
typedef struct {
    const char    *device;    /* ALSA PCM 设备名（"plughw:1,0", "default" 等） */
    unsigned int   rate;      /* 采样率（推荐 16000） */
    int            channels;  /* 声道数（推荐 1, mono） */
} alsa_capture_config_t;
```

**接口说明：**

| 方法 | 行为 |
|------|------|
| `init(cfg)` | 初始化录音模块 |
| `deinit()` | 反初始化 |
| `start()` | 创建独立 capture 线程：`snd_pcm_readi` 读取 PCM → `wake_word_process` 检测唤醒 → 发布 `EV_AUDIO_CAPTURE_DATA` |
| `stop()` | 停止录音线程 |
| `is_running()` | 查询是否在录音 |
| `set_wake_muted(muted)` | TTS 播报时静音唤醒检测（防止自触发） |

**发布事件：** `EV_AUDIO_CAPTURE_DATA`（携带 `capture_data_t` 帧数据）、`EV_AUDIO_CAPTURE_START`、`EV_AUDIO_CAPTURE_STOP`

---

### 2.9 audio_mixer（服务层，PRIO 10）

**职责：** 软件混音器，多路 PCM 混音 + 集中式 Ducking 管理。支持最多 8 路输入流，每路独立增益控制。

**设计说明：** TTS ducking 从各模块分散实现集中到 mixer 统一管理。不直接操作 ALSA，只做 PCM 数据层的混音运算。

**接口说明：**

| 方法 | 行为 |
|------|------|
| `init()` | 初始化混音器 |
| `deinit()` | 反初始化 |
| `stream_register(name, initial_gain)` | 注册一路输入流，返回 stream_id |
| `stream_unregister(stream_id)` | 注销输入流 |
| `stream_set_gain(stream_id, gain)` | 设置流增益（0.0~1.0） |
| `stream_get_gain(stream_id)` | 获取流增益 |
| `set_master_gain(gain)` | 设置主增益 |
| `duck_start(stream_name, duck_gain)` | 开始对某流做 ducking |
| `duck_restore(stream_name)` | 恢复某流原始音量 |
| `mix_2ch(src1, gain1, src2, gain2, dst, frames, channels)` | 将两路 16bit PCM 混音到输出缓冲（算法：sum × gain_master，clamp 防溢出） |

---

### 2.10 audio_pipeline（服务层，PRIO 10）

**职责：** 统一音频调度入口。协调 `audio_player`（播放）、`alsa_capture`（录音）、`audio_mixer`（混音）、`bt_a2dp`（A2DP），提供统一 API。`voice_agent` 只调 pipeline 接口，不直接操作音频子模块。

**配置结构（`audio_pipeline_config_t`，定义在 `audio_pipeline.h`）：**

```c
typedef struct {
    const char    *playback_device;    /* 播放设备名 */
    const char    *capture_device;     /* 录音设备名 */
    unsigned int   capture_rate;       /* 录音采样率（16000） */
    const char    *a2dp_device;        /* A2DP 播放设备（NULL=同 playback） */
    int            a2dp_ringbuf_frames;/* A2DP 环形缓冲帧数（256） */
} audio_pipeline_config_t;
```

**接口说明：**

| 方法 | 行为 |
|------|------|
| `init(cfg)` | 内部链式初始化：mixer_init → audio_player_init → bt_a2dp_init → alsa_capture_init |
| `deinit()` | 逆序反初始化 |
| `play_tts(filepath)` | 异步播放 TTS WAV 文件，自动对 music 流做 ducking，播完恢复 |
| `stop_tts()` | 停止 TTS 播放 |
| `is_tts_playing()` | 查询 TTS 是否正在播放 |
| `music_start()` | 通知 pipeline A2DP 音乐开始 |
| `music_stop()` | 通知 pipeline A2DP 音乐停止 |
| `is_music_playing()` | 查询音乐是否在播放 |
| `capture_start()` | 启动录音 |
| `capture_stop()` | 停止录音 |
| `is_capturing()` | 查询是否在录音 |

---

### 2.11 wake_word（预留引擎层，PRIO 10）

**职责：** 离线唤醒词检测模块。当前采用能量阈值检测替代（集成在 `alsa_capture.c` 中），保留 Porcupine 唤醒词引擎接口代码（注释状态），待获取 Picovoice AccessKey 后取消注释即可启用。

**接口定义（`wake_word.h`）：**

| 方法 | 说明 |
|------|------|
| `wake_word_init(model, keyword, access_key, sensitivity)` | 初始化 Porcupine 引擎（可选，失败不影响系统运行） |
| `wake_word_process(pcm, frames)` | 处理 PCM 音频帧，检测到唤醒词发布 `EV_WAKEUP_DETECTED` |
| `wake_word_deinit()` | 释放引擎资源 |

**架构集成方式：**

- `alsa_capture` 的 capture_thread 每帧调用 `wake_word_process()`
- 检测到唤醒词发布 `EV_WAKEUP_DETECTED` 事件
- `voice_agent` 订阅该事件，状态机从 `ST_IDLE` → `ST_PROCESSING`（动作 `do_wake_response`）
- TTS 播报期间通过 `alsa_capture_set_wake_muted(true)` 防止自触发

---

## 三、线程模型

| 线程 | 持有的锁 | 职责 |
|------|:--------:|------|
| `main` | 无 | 模块初始化、信号等待 |
| `event_loop` | 无 | 从队列取事件 → 拷贝订阅列表 → 释放队列锁 → 串行分发（不在锁内调用回调） |
| `bt_spp_thread`（N 个） | `subs_lock` | BlueZ 回调传入 fd 后创建，SPP read → publish `EV_BT_DATA_RECEIVED` |
| `bt_dispatch_thread` | `subs_lock` | `sd_bus_process` 轮询，处理 D-Bus vtable 回调 |
| `bt_a2dp_thread` | `subs_lock` | PCM 接收 → 环形缓冲 → publish |
| `ai_worker`（N 个，线程池） | `subs_lock` | HTTP 请求 → publish 结果 |
| `audio_thread`（TTS 播放） | `subs_lock` | ALSA `snd_pcm_writei` 循环 → publish 完成/错误事件 |
| `capture_thread` | `subs_lock` | `snd_pcm_readi` → 能量检测 → publish 录音帧 |
| `state_machine` | `s_sm.lock` | dispatch 操作原子执行，动作回调不再次进入状态机 |

**关键设计：** `event_bus` 分发时先拷贝订阅列表再释放锁，保证回调执行期间不持有锁，避免死锁。所有工作线程发布事件时仅获取 `subs_lock` 完成 `publish` 操作。

---

## 四、事件 ID 全集

```c
/* 系统事件（0x00xx） */
EV_SYS_SHUTDOWN      = 0x0001  // SIGINT/SIGTERM 触发退出
EV_SYS_ERROR         = 0x0002  // 系统级错误

/* 蓝牙事件（0x01xx） */
EV_BT_DEVICE_CONN    = 0x0103  // 蓝牙设备连接
EV_BT_DEVICE_DISCONN = 0x0104  // 蓝牙设备断开
EV_BT_DATA_RECEIVED  = 0x0105  // SPP 接收到文本命令

/* 音频事件（0x02xx） */
EV_AUDIO_PLAY_DONE      = 0x0201  // TTS 播放完成
EV_AUDIO_MUSIC_START    = 0x0202  // A2DP 音乐开始播放
EV_AUDIO_MUSIC_STOP     = 0x0203  // A2DP 音乐停止播放
EV_AUDIO_ERROR          = 0x0204  // 音频播放错误
EV_AUDIO_CAPTURE_DATA   = 0x0205  // 录音 PCM 数据帧
EV_AUDIO_CAPTURE_START  = 0x0206  // 录音开始
EV_AUDIO_CAPTURE_STOP   = 0x0207  // 录音停止

/* AI 事件（0x03xx） */
EV_AI_RESP_READY     = 0x0300  // AI 回复完整就绪
EV_AI_TTS_DONE       = 0x0301  // TTS 合成完成
EV_AI_ERROR          = 0x0303  // AI 请求失败
EV_AI_STREAM_CHUNK   = 0x0304  // AI 流式数据块（预留）

/* 唤醒词事件（0x04xx） */
EV_WAKEUP_DETECTED   = 0x0400  // 离线唤醒词或能量阈值触发

/* 网络事件（0x05xx，预留） */
EV_NET_CONNECTED     = 0x0500  // 网络连接恢复
EV_NET_DISCONNECTED  = 0x0501  // 网络连接断开
```

---

## 五、文件组织

```
code/smart-speaker/
├── README.md                  # 项目说明 + 构建部署指南
├── ARCHITECTURE.md            # 架构设计文档
├── Makefile                   # 构建脚本
├── speaker.conf.example       # 配置文件示例
│
├── inc/                       # 公共头文件
│   ├── common.h               # 公共类型定义（err_t, event_id 等）
│   ├── event_bus.h            # 事件总线接口
│   ├── state_machine.h        # 状态机接口
│   ├── thread_pool.h          # 线程池接口
│   ├── module.h               # 模块生命周期管理（MODULE_DEFINE 宏）
│   ├── config.h               # 配置加载接口
│   ├── logger.h               # 日志接口
│   ├── bt_manager.h           # 蓝牙 SPP 管理接口
│   ├── bt_a2dp.h              # 蓝牙 A2DP 接口
│   ├── audio_player.h         # 音频播放接口（文件播放 + 流式播放）
│   ├── audio_mixer.h          # 软件混音器 + ducking 接口
│   ├── audio_pipeline.h       # 音频统一调度入口
│   ├── alsa_capture.h         # USB 声卡录音接口
│   ├── ai_client.h            # AI 客户端 + TTS 配置 + 对话管理接口
│   ├── voice_agent.h          # 状态机编排器接口
│   └── wake_word.h            # 唤醒词引擎接口（预留）
│
├── src/                       # 源码实现
│   ├── main.c                 # 程序入口
│   │
│   ├── core/                  # 框架层
│   │   ├── event_bus.c        # 事件总线实现
│   │   ├── state_machine.c    # 状态机实现
│   │   ├── thread_pool.c      # 线程池实现
│   │   ├── module.c           # 模块生命周期实现
│   │   └── voice_agent.c      # 应用层状态机编排器
│   │
│   ├── bluetooth/             # 蓝牙服务层
│   │   ├── bt_manager.c       # BlueZ D-Bus Profile1 vtable SPP 实现
│   │   └── bt_a2dp.c          # A2DP Sink + PCM 环形缓冲
│   │
│   ├── audio/                 # 音频服务层
│   │   ├── audio_player.c     # ALSA PCM 播放
│   │   ├── audio_mixer.c      # 软件混音器 + ducking
│   │   ├── audio_pipeline.c   # 播放/录音/A2DP 协调调度
│   │   └── alsa_capture.c     # USB 声卡录音 + 唤醒能量检测
│   │
│   ├── ai/                    # AI 服务层
│   │   ├── ai_client.c        # libcurl HTTP + 同步/流式API + 重试
│   │   ├── ai_conv.c          # 对话历史管理 + JSON 构建
│   │   ├── ai_tts.c           # 腾讯云 TTS（TC3-HMAC-SHA256 签名）
│   │   └── wake_word.c        # 唤醒词引擎实现（预留，注释状态）
│   │
│   └── utils/                 # 工具模块
│       ├── logger.c           # 日志实现
│       └── config.c           # 配置解析实现
│
└── tests/                     # 单元测试
    ├── test_event_bus.c
    ├── test_state_machine.c
    └── test_thread_pool.c
```

---

## 六、错误处理标准

| 层级 | 策略 |
|:----:|------|
| **init** | 任一步骤失败 → 回滚所有已初始化模块 → `main` 退出 |
| **运行时** | 单模块异常 → 发布 `EV_*_ERROR` 事件 → `voice_agent` 回退到 `ST_IDLE` 状态 |
| **deinit** | 某模块泄漏或反初始化失败 → 记录日志 → 继续反初始化其他模块 |
| **OOM** | 所有 `malloc` / `realloc` 返回值全量检查 → 返回 `ERR_NOMEM` → 调用方回滚已分配资源 |
| **网络** | ai_client 内置重试（最多 3 次，间隔 200ms）；超时返回 `ERR_TIMEOUT` |
| **音频** | underrun 触发 → `snd_pcm_recover` 尝试恢复 → 连续 3 次失败才上报 `EV_AUDIO_ERROR` |

---

## 七、编码规范

1. **返回值检查** — 所有函数返回值必须检查，非 void 函数必须被调用方检查返回码
2. **资源对称** — 每个 `malloc` 对应一个 `free`，每个 `init` 对应一个 `deinit`
3. **全局状态** — 模块级全局变量统一使用 `static struct { ... } s_xxx = {0}` 模式
4. **符号可见性** — 内部函数加 `static`，外部接口在对应头文件中声明
5. **日志分级** — `DEBUG` / `INFO` / `WARN` / `ERROR`，关键路径必须有日志
6. **注释风格** — 统一使用 `/* */` 风格注释
7. **编译标准** — 零警告编译（`-Wall -Wextra -Werror`）
8. **线程安全** — 共享数据必须在持有锁的上下文中访问

---

## 八、开发路线图

### Phase 1：架构闭环

- 实现 `voice_agent.c/h` 状态机编排器，搭建四层架构骨架
- 重构 `ai_conv.c`，剥离之前散落在其中的编排逻辑
- 调整 `main.c` 注册 `voice_agent` 模块
- 目标：`state_machine` + `thread_pool` 完全接入，事件总线双向通信验证通过

### Phase 2：企业级加固

- 事件总线：锁内不调用回调（先拷贝订阅列表再分发），队列满时返回 `ERR_FULL` 而非阻塞
- 状态机：action 回调支持传递 `void *data` 上下文
- AI 客户端：增加 HTTP 状态码检查与自动重试
- 蓝牙管理：`read()` 增加超时控制
- 应用层：实现状态超时监控（30s 处理超时 / 60s 播报超时）

### Phase 3：服务层独立实现

- 手写 `ai_tts.c`：腾讯云 TTS（TC3-HMAC-SHA256 签名，base64 解码）
- 手写 `audio_player.c`：ALSA PCM 播放（play / stop / stream_start / stream_write）
- ALSA 双工直通验证：`speaker-test` + `arecord` 同时运行 30 分钟

### Phase 4：蓝牙完整化

- `bt_manager.c` 重写：从 `system()` 调用升级为完整的 BlueZ D-Bus Profile1 vtable SPP 实现
- 新增 `bt_a2dp.c`：A2DP Sink 实现 + PCM 环形缓冲
- `voice_agent.c` 扩展：A2DP 状态转移 + ducking 逻辑

### Phase 5：音频管道

- `alsa_capture.c`：USB 声卡录音（16bit 16kHz PCM）+ 唤醒能量检测
- `audio_mixer.c`：软件混音器 + 集中式 ducking 管理
- `audio_pipeline.c`：播放 / 录音 / A2DP 统一调度入口

### Phase 6：联调与稳定性

- Orange Pi Zero 3 实机部署和运行
- SPP AI 对话全链路端到端验证
- A2DP 音乐播放中触发 TTS ducking 场景验证
- valgrind 内存泄漏检测与修复
- 配置项收尾与 bugfix

---

## 九、质量检查项

以下检查项在每次模块开发完成后逐一核对：

- [ ] 所有 `malloc` / `realloc` / `fopen` 返回值检查
- [ ] 每个 `malloc` 在 `deinit` 中有对应的 `free`
- [ ] `init` 失败路径回滚所有已分配资源
- [ ] 所有函数参数做 `NULL` 检查
- [ ] 关键路径有日志输出
- [ ] 编译零警告（`-Wall -Wextra -Werror`）
- [ ] 共享数据在锁内访问（线程安全）

---

> 本文档维护于 `code/smart-speaker/ARCHITECTURE.md`，是项目架构设计的唯一权威参考。
