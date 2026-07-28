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
   - 2.8 wake_word
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
         │  │ - BlueZ-DBus │  │ - libcurl  │  │ - TTS API│  │  - ALSA PCM  │  │
         │  │ - SPP 服务   │  │ - SSE 流式  │  │ - MP3→WAV│  │  - WAV/PCM   │  │
         │  │ - 指数退避   │  │ - 重试+降级 │  │ - 文件管  │  │  - underrun  │  │
         │  └─────────────┘  └───────────┘  └──────────┘  └──────────────┘  │
         │                                                                   │
         │  ┌─────────────┐  ┌───────────┐                                  │
         │  │  bt_a2dp     │  │ ai_conv   │                                  │
         │  │ - A2DP Sink  │  │ - 对话历史 │                                  │
         │  │ - PCM 环形   │  │ - Token裁  │                                  │
         │  │ - TTS ducking│  │ - JSON构建 │                                  │
         │  └─────────────┘  └───────────┘                                  │
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

**设计说明：** 这是整个系统的唯一编排器，基于状态表驱动的事件响应。不提供外部 API——全部通过 event_bus 驱动，模块注册由 `MODULE_DEFINE` 完成，`main` 不需要显式调用任何初始化函数。

**状态常量：**

| 状态 | 值 | 说明 |
|------|:--:|------|
| `ST_IDLE` | 0 | 空闲，等待事件 |
| `ST_MUSIC` | 1 | A2DP 音乐播放中 |
| `ST_PROCESSING` | 2 | AI 请求处理中 |
| `ST_SPEAKING` | 3 | TTS 播报中 |
| `ST_EXITING` | 4 | 收到关闭信号 |

**初始化流程：**

1. `sm_init(s_transitions, 11, ST_IDLE)` —— 初始化状态机并注册 11 条转移
2. 订阅 7 个事件：`EV_BT_DATA_RECEIVED`、`EV_AI_RESP_READY`、`EV_AUDIO_PLAY_DONE`、`EV_AUDIO_MUSIC_START`、`EV_AUDIO_MUSIC_STOP`、`EV_SYS_SHUTDOWN`

**状态转移表：**

| 当前状态 | 事件 | 下一状态 | 动作 |
|----------|------|:--------:|------|
| `ST_IDLE` | `EV_BT_DATA_RECEIVED` | `ST_PROCESSING` | `do_ai_request` |
| `ST_IDLE` | `EV_AUDIO_MUSIC_START` | `ST_MUSIC` | `on_music_start` |
| `ST_MUSIC` | `EV_AUDIO_MUSIC_STOP` | `ST_IDLE` | `on_music_stop` |
| `ST_MUSIC` | `EV_BT_DATA_RECEIVED` | `ST_PROCESSING` | `do_ai_request`（同时 duck 音乐音量） |
| `ST_PROCESSING` | `EV_AI_RESP_READY` | `ST_SPEAKING` | `do_tts_and_play`（同时 duck 音乐音量） |
| `ST_PROCESSING` | `EV_AI_ERROR` | `ST_IDLE` | `on_error`（恢复音乐播放） |
| `ST_SPEAKING` | `EV_AUDIO_PLAY_DONE` | `ST_IDLE` | `on_play_done`（恢复音乐由动作内处理） |
| `ST_SPEAKING` | `EV_AUDIO_ERROR` | `ST_IDLE` | `on_error` |
| `ST_IDLE` | `EV_BT_DEVICE_DISCONN` | `ST_IDLE` | NULL（不处理） |
| `ST_ANY` | `EV_SYS_SHUTDOWN` | `ST_EXITING` | `on_shutdown` |
| `ST_ANY` | `EV_SYS_ERROR` | `ST_IDLE` | `on_error` |

**状态超时监控：**

- `ST_PROCESSING` 超过 30s —— 自动回退到 `ST_IDLE`，发布 `EV_AI_ERROR`
- `ST_SPEAKING` 超过 60s —— 自动回退到 `ST_IDLE`，发布 `EV_AUDIO_ERROR`

---

### 2.2 bt_manager（服务层，PRIO 10）

**职责：** 蓝牙 SPP 串口服务管理，与手机 APP 建立双向数据通道。

**初始化流程：**

1. 建立 BlueZ D-Bus 连接，获取 Adapter1 接口
2. 设置设备名称、可发现模式、可配对
3. 注册 RFCOMM SPP Profile（channel 1）
4. 启动独立线程循环：[accept] -> [read] -> [publish(`EV_BT_DATA_RECEIVED`)]
5. 断线时指数退避重连：1s → 2s → 4s → 8s → 16s → 30s（封顶）

**反初始化：**
设置 `running = false` → 关闭 server fd → `pthread_join` → 断开 D-Bus

**发布事件：**
`EV_BT_DEVICE_CONN`、`EV_BT_DEVICE_DISCONN`、`EV_BT_DATA_RECEIVED`

---

### 2.3 ai_client（服务层，PRIO 10）

**职责：** 封装 libcurl HTTP 请求，向 LLM API 发送对话并接收 SSE 流式响应。

**接口说明：**

- `chat(json_payload, callback, user_data)` —— 同步阻塞在调用者线程中，libcurl POST 发起请求。通过 SSE write_cb 逐行回调 `sse_feed` → `sse_process_line` → `extract_content`，每收到一个 chunk 调用 `callback(chunk, 0)`。收到 `[DONE]` 或异常时调用 `callback(full_text, 1)`。失败返回 `ERR_GENERAL`，由调用方决定是否重试。
- `chat_sync(json_payload, &response)` —— 同步全量返回，不经过 SSE 解析，适用于直接获取完整响应。

**反初始化：** `curl_global_cleanup()`

---

### 2.4 ai_conv（服务层，PRIO 10）

**职责：** 管理多轮对话历史，自动裁剪 token 长度，构建符合 API 要求的 JSON payload。

**接口说明：**

| 方法 | 行为 |
|------|------|
| `init(system_prompt)` | 保存系统提示词，初始化 history 环形缓冲区（容量 11 条） |
| `append(role, content)` | 超长内容截断；历史满时删除最旧的一对问答再追加 |
| `build_payload()` | 遍历历史，转义 `"` 和 `\`，返回 malloc 分配的 JSON 字符串 |
| `clear()` | 保留 system prompt，清空 history[1..] |
| `deinit()` | 释放全部动态内存 |

---

### 2.5 ai_tts（服务层，PRIO 10）

**职责：** 调用 Microsoft Edge TTS 服务，将文字合成为 WAV 音频文件。

**接口说明：**

| 方法 | 行为 |
|------|------|
| `init()` | 检查 `edge-tts` 和 `mpg123` 命令是否可用，创建输出目录 |
| `speak(text, &out_path)` | ① `edge-tts --text ... --write-media tmp.mp3` → ② `mpg123 -w tmp.wav tmp.mp3` → ③ 失败则删除半成品文件返回 `ERR_GENERAL` → ④ 成功返回 `out_path = strdup(wav_path)`，调用方负责 free |
| `deinit()` | 清理临时文件 |

---

### 2.6 audio_player（服务层，PRIO 10）

**职责：** ALSA PCM 音频播放，支持文件播放和 PCM 数据直接播放。

**接口说明：**

| 方法 | 行为 |
|------|------|
| `init()` | 保存 ALSA 设备名称 |
| `play_async(filepath)` | 若正在播放则返回 `ERR_BUSY`；否则创建播放线程：[打开 ALSA → PCM 参数协商 → `snd_pcm_writei` 循环 → drain]；完成时发布 `EV_AUDIO_PLAY_DONE`，出错时发布 `EV_AUDIO_ERROR` |
| `play_pcm(pcm_data, len)` | 直接播放 PCM 裸数据（供 A2DP 使用） |
| `stop()` | 调用 `snd_pcm_drop` 踢醒播放线程 |
| `deinit()` | 停止播放 → 等待线程退出 → 关闭 PCM 句柄 |

---

### 2.7 bt_a2dp（服务层，PRIO 10）

**职责：** A2DP Sink 实现，将手机蓝牙音乐通过 PCM 环形缓冲播放到 ALSA 输出。

**初始化流程：**

1. 启动 ALSA 播放线程（从环形缓冲读取数据发往 audio_player）
2. 启动 bluealsa PCM reader 线程
3. 自动发现 bluealsa 设备（`bluealsa-aplay -L`）

**环形缓冲设计：**

- 容量：16384 帧（每帧 16bit × 2ch = 4 字节，约 370ms @ 44100Hz）
- 满时：丢弃最老帧（覆盖旧数据），不阻塞 bluealsa 发射端
- 空时：ALSA 播放静音帧（防止 underrun）

**实现说明：** 原计划手写 BlueZ D-Bus A2DP vtable，但 H618 平台存在 PDU malformed 兼容性 bug，改用 `bluez-alsa` 方案。D-Bus vtable 方案留待以后研究。

**TTS Ducking 实现：**

TTS 播报时 PCM 采样值乘以 0.3 做软件衰减，播完恢复 ×1.0。USB 声卡无硬件混音器，不引入 dmix 插件，CPU 增加约 2%。

**发布事件：** `EV_AUDIO_MUSIC_START`、`EV_AUDIO_MUSIC_STOP`

---

### 2.8 wake_word（预留引擎层，PRIO 10）

**职责：** 离线唤醒词检测模块。当前采用能量阈值检测替代，保留 Porcupine 唤醒词引擎接口代码（注释状态），待获取 Picovoice AccessKey 后取消注释即可启用。

**接口定义（wake_word.h）：**

| 方法 | 说明 |
|------|------|
| `wake_word_init(model, keyword, access_key, sensitivity)` | 初始化 Porcupine 引擎（可选，失败不影响系统运行） |
| `wake_word_process(pcm, frames)` | 处理 PCM 音频帧，检测到唤醒词发布 `EV_WAKEUP_DETECTED` |
| `wake_word_deinit()` | 释放引擎资源 |

**架构解耦设计：**

- `alsa_capture` 的 capture_thread 每帧调用 `wake_word_process()`
- 检测到唤醒词发布 `EV_WAKEUP_DETECTED` 事件
- `voice_agent` 订阅该事件，状态机从 `ST_IDLE` → `ST_PROCESSING`
- TTS 播报期间通过 `alsa_capture_set_wake_muted(true)` 防止自触发

**事件：** `EV_WAKEUP_DETECTED = 0x0400`

---

## 三、线程模型

| 线程 | 持有的锁 | 职责 |
|------|:--------:|------|
| `main` | 无 | 模块初始化、信号等待 |
| `event_loop` | 无 | 从队列取事件 → 拷贝订阅列表 → 释放队列锁 → 串行分发（不在锁内调用回调） |
| `bt_spp_thread` | `subs_lock` | SPP 端口 accept + read → 发布事件 |
| `bt_a2dp_thread` | `subs_lock` | PCM 数据接收 → 写入环形缓冲 → 发布事件 |
| `ai_worker(N)` | `subs_lock` | HTTP 请求 → 发布事件 |
| `audio_thread` | `subs_lock` | ALSA `snd_pcm_writei` 循环 → 发布完成/错误事件 |
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
EV_AUDIO_PLAY_DONE   = 0x0201  // 音频播放完成
EV_AUDIO_MUSIC_START = 0x0202  // A2DP 音乐开始播放
EV_AUDIO_MUSIC_STOP  = 0x0203  // A2DP 音乐停止播放
EV_AUDIO_ERROR       = 0x0204  // 音频播放错误

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
│   ├── module.h               # 模块生命周期管理
│   ├── config.h               # 配置加载接口
│   ├── logger.h               # 日志接口
│   ├── bt_manager.h           # 蓝牙 SPP 管理接口
│   ├── audio_player.h         # 音频播放接口
│   ├── ai_client.h            # AI 客户端接口
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
│   │   ├── bt_manager.c       # BlueZ D-Bus SPP 实现
│   │   └── bt_a2dp.c          # A2DP Sink + PCM 环形缓冲
│   │
│   ├── audio/                 # 音频服务层
│   │   ├── audio_player.c     # ALSA PCM 播放
│   │   ├── alsa_capture.c     # USB 声卡录音（16bit 16kHz）
│   │   ├── audio_mixer.c      # 软件混音 + TTS ducking
│   │   └── audio_pipeline.c   # 播放/录音/A2DP 协调
│   │
│   ├── ai/                    # AI 服务层
│   │   ├── ai_client.c        # libcurl HTTP + SSE 解析
│   │   ├── ai_conv.c          # 对话历史管理 + JSON 构建
│   │   ├── ai_tts.c           # Edge TTS 合成
│   │   └── wake_word.c        # 唤醒词引擎实现（预留）
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
| **网络** | 超时返回 `ERR_TIMEOUT` → 调用方决定重试策略 |
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
- 蓝牙管理：`accept() / read()` 增加超时机制
- 应用层：实现状态超时监控（30s 处理超时 / 60s 播报超时）

### Phase 3：服务层独立实现

- 手写 `ai_tts.c`：Edge TTS 合成 + mpg123 转码
- 手写 `audio_player.c`：ALSA PCM 播放（play_async / stop / play_pcm）
- ALSA 双工直通验证：`speaker-test` + `arecord` 同时运行 30 分钟

### Phase 4：蓝牙完整化

- `bt_manager.c` 重写：从 `system()` 调用升级为完整的 BlueZ D-Bus SPP 实现
- 新增 `bt_a2dp.c`：A2DP Sink 实现 + PCM 环形缓冲
- `voice_agent.c` 扩展：A2DP 状态转移 + TTS ducking 逻辑

### Phase 5：音频管道

- `alsa_capture.c`：USB 声卡录音（16bit 16kHz PCM）
- `audio_mixer.c`：软件混音器 + TTS ducking 音量衰减
- `audio_pipeline.c`：播放线程 / 录音线程 / A2DP 数据流的统一协调

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
