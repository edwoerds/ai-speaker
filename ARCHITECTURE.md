# AI智能音箱 -- 企业级架构与开发计划

> 版本：v3.1 | 日期：2026-07-18
> 这是项目的唯一真相源，所有代码、所有决策、所有排期严格按本文档执行。

---

## 一、架构总览

```
+--------------------------------------------------------------+
|                 应用层 Application（PRIO 20）                  |
|                                                              |
|  voice_agent.c        主控状态机，项目唯一编排器                 |
|  职责：状态表驱动、事件路由、任务调度、超时监控、错误恢复           |
|  通信：只通过 event_bus.subscribe() / publish()，不直调任何模块  |
|  线程：AI 请求通过 thread_pool.submit() 异步执行                |
+---------------------------+--------------------------+-------+
                            |                          |
                  event_bus publish/subscribe
                            |                          |
+---------------------------+--------------------------+-------+
|                 服务层 Service（PRIO 10）                      |
|                                                              |
|  bt_manager        ai_client        ai_tts        audio_player|
|  - BlueZ D-Bus    - libcurl HTTP   - TTS API      - ALSA PCM |
|  - SPP 服务        - SSE 流式解析    - 文字->MP3    - WAV/PCM  |
|  - 指数退避重连    - 重试+降级       - 文件管理     - underrun  |
|                                                              |
|  bt_a2dp           ai_conv                                  |
|  - A2DP Sink      - 对话历史管理（append/build_payload/clear） |
|  - PCM 环形缓冲    - Token 裁剪 + JSON 手动构建                |
|  - TTS ducking                                               |
|                                                              |
|  原则：模块间不直接调用，不 include 对方头文件，只发布/订阅事件     |
+----------------------------------------------------------------+
                            |
                    唯一通信通道: event_bus
                            |
+----------------------------------------------------------------+
|                 事件总线 Event Bus（PRIO 0）                    |
|                                                              |
|  event_bus.c        模块间唯一通信通道                          |
|  机制：环形缓冲队列 + 订阅链表 + mutex + cond var                |
|  分发：独立线程串行 dispatch，先拷贝订阅列表再分发（防死锁）        |
+----------------------------------------------------------------+
                            |
                     为上层提供能力
                            |
+----------------------------------------------------------------+
|                 框架层 Framework（PRIO 0）                      |
|                                                              |
|  state_machine.c     thread_pool.c      module.c    common.h |
|  表驱动状态转移       异步任务执行        生命周期管理   公共类型  |
|  - sm_init/dispatch  - submit/worker    - constructor - err_t |
|  - void*data 传递    - 队列深度监控      - 失败回滚  - event_id |
+----------------------------------------------------------------+
```

---

## 二、模块接口契约

### 2.1 voice_agent（应用层，PRIO 20）

```
接口（voice_agent.h）:
  // voice_agent 不提供外部 API, 全部通过 event_bus 驱动
  // 模块注册由 MODULE_DEFINE 完成, main 不需要显式调任何函数

状态常量:
  ST_IDLE       = 0  // 空闲, 等待事件
  ST_MUSIC      = 1  // A2DP 音乐播放中
  ST_PROCESSING = 2  // AI 请求处理中
  ST_SPEAKING   = 3  // TTS 播报中
  ST_EXITING    = 4  // 收到关闭信号

init:
  1. sm_init(s_transitions, 11, ST_IDLE)
  2. event_subscribe(EV_BT_DATA_RECEIVED, on_event, NULL)
  3. event_subscribe(EV_AI_RESP_READY, on_event, NULL)
  4. event_subscribe(EV_AUDIO_PLAY_DONE, on_event, NULL)
  5. event_subscribe(EV_AUDIO_MUSIC_START, on_event, NULL)
  6. event_subscribe(EV_AUDIO_MUSIC_STOP, on_event, NULL)
  7. event_subscribe(EV_SYS_SHUTDOWN, on_event, NULL)

shutdown:
  sm_dispatch(EV_SYS_SHUTDOWN) -> EXITING

deinit:
  取消所有订阅, 状态重置

状态表：
  ST_IDLE       + EV_BT_DATA_RECEIVED    -> ST_PROCESSING   do_ai_request
  ST_IDLE       + EV_AUDIO_MUSIC_START   -> ST_MUSIC        on_music_start
  ST_MUSIC      + EV_AUDIO_MUSIC_STOP    -> ST_IDLE         on_music_stop
  ST_MUSIC      + EV_BT_DATA_RECEIVED    -> ST_PROCESSING   do_ai_request (duck音乐)
  ST_PROCESSING + EV_AI_RESP_READY       -> ST_SPEAKING     do_tts_and_play (duck音乐)
  ST_PROCESSING + EV_AI_ERROR            -> ST_IDLE         on_error (恢复音乐)
  ST_SPEAKING   + EV_AUDIO_PLAY_DONE     -> ST_IDLE         on_play_done  (注: 恢复音乐在action里做)
  ST_SPEAKING   + EV_AUDIO_ERROR         -> ST_IDLE         on_error
  ST_IDLE       + EV_BT_DEVICE_DISCONN   -> ST_IDLE         NULL
  ST_ANY(-1)    + EV_SYS_SHUTDOWN        -> ST_EXITING      on_shutdown
  ST_ANY(-1)    + EV_SYS_ERROR           -> ST_IDLE         on_error

状态超时：
  ST_PROCESSING 超过 30s -> 自动回 ST_IDLE + EV_AI_ERROR
  ST_SPEAKING   超过 60s -> 自动回 ST_IDLE + EV_AUDIO_ERROR
```

### 2.2 bt_manager（服务层，PRIO 10）

```
init:
  1. BlueZ D-Bus 连接 -> 获取 Adapter1 接口
  2. 设设备名、可发现、可配对
  3. 注册 RFCOMM SPP Profile（channel 1）
  4. 启动独立线程：accept -> read -> publish(EV_BT_DATA_RECEIVED)
  5. 指数退避重连（1s->2s->4s->8s->16s->30s 封顶）

deinit:
  running=false -> shutdown(srv_fd) -> pthread_join -> D-Bus 断开

发布事件：
  EV_BT_DEVICE_CONN / EV_BT_DEVICE_DISCONN / EV_BT_DATA_RECEIVED
```

### 2.3 ai_client（服务层，PRIO 10）

```
init:
  curl_global_init(CURL_GLOBAL_ALL)
  保存 api_url/api_key/model/timeout/temperature/max_tokens

chat(json_payload, callback, user_data):
  同步阻塞在调用者线程, libcurl POST -> SSE write_cb -> sse_feed ->
  sse_process_line -> extract_content -> callback(chunk, 0)
  -> [DONE] 或异常 -> callback(full_text, 1)
  失败返回 ERR_GENERAL（调用方决定重试）

chat_sync(json_payload, &response):
  同步全量返回, 不走 SSE 解析

deinit:
  curl_global_cleanup()
```

### 2.4 ai_conv（服务层，PRIO 10）

```
init(system_prompt): 保存 prompt, 初始化 history[11], acc_text 缓冲区
append(role, content): 超长截断 + 满则删最旧一对 + 追加
build_payload(): 遍历历史, 转义 " 和 \, 返回 malloc 的 JSON
clear(): 保留 system prompt, 清空 history[1..]
deinit(): 释放全部
```

### 2.5 ai_tts（服务层，PRIO 10）

```
init: 检查 edge-tts 和 mpg123 是否可用，创建输出目录
speak(text, &out_path):
  1. system("edge-tts --text ... --write-media tmp.mp3")
  2. system("mpg123 -w tmp.wav tmp.mp3")
  3. HTTP != 200 -> 删半成品文件, 返回 ERR_GENERAL
  4. 成功 -> out_path=strdup(wav_path), 调用方 free
deinit: 清理临时文件
```

### 2.6 audio_player（服务层，PRIO 10）

```
init: 保存设备名
play_async(filepath): 正播放则返回 ERR_BUSY, 否则创建线程:
  打开 ALSA -> PCM 协商 -> snd_pcm_writei -> drain
  完成 publish(EV_AUDIO_PLAY_DONE)
  错误 publish(EV_AUDIO_ERROR)
play_pcm(pcm_data, len): 直接播放 PCM 数据（供 A2DP 用）
stop(): snd_pcm_drop 踢醒线程
deinit: 停播 + join + 关 PCM
```

### 2.7 bt_a2dp（服务层，PRIO 10）

```
init:
  1. 启动 ALSA 播放线程（环形缓冲 -> audio_player）
  2. 启动 bluealsa PCM reader 线程
  3. 自动发现 bluealsa 设备（bluealsa-aplay -L）

环形缓冲:
  容量 16384 帧（每帧 16bit*2ch=4 bytes，约 370ms@44100Hz）
  满时: 丢弃最老的帧（覆盖旧数据），不阻塞 bluealsa 发射端
  空时: ALSA 播放静音帧（防止 underrun）

注：原计划手写 BlueZ D-Bus A2DP vtable，但 H618 平台有 PDU malformed
兼容性bug，改用 bluez-alsa 方案。D-Bus vtable 留待以后研究。

功能:
  - 手机蓝牙音频 -> PCM 环形缓冲 -> ALSA 播放
  - TTS 播报时: PCM 采样值*0.3 做软件衰减（ducking），播完恢复*1.0
    （USB声卡无硬件混音器，不用 dmix 插件，CPU增约2%）
  - 发布事件: EV_AUDIO_MUSIC_START / STOP

deinit: 停收 -> 释缓冲 -> 注销 Profile
```

### 2.8 wake_word（预留引擎层，PRIO 10）

```
接口（wake_word.h）:
  wake_word_init(model_path, keyword_path, access_key, sensitivity)
    初始化 Porcupine 离线唤醒词引擎（可选，失败不影响系统）
  wake_word_process(pcm, frames)
    处理 PCM 音频帧，检测到唤醒词发布 EV_WAKEUP_DETECTED
  wake_word_deinit()
    释放引擎

当前实现:
  用 alsa_capture.c 中的能量检测替代（音量阈值触发）
  wake_word.h/c 保留 Porcupine 接口代码（注释状态），待获取
  Picovoice AccessKey 后取消注释即可启用

设计说明:
  唤醒词引擎通过独立模块封装，与系统架构解耦。
  - alsa_capture 的 capture_thread 每帧调用 wake_word_process()
  - 检测到唤醒词发布 EV_WAKEUP_DETECTED 事件
  - voice_agent 订阅该事件，状态机从 ST_IDLE -> ST_PROCESSING
  - TTS 播报期间通过 alsa_capture_set_wake_muted(true) 防止自触发

事件:
  EV_WAKEUP_DETECTED = 0x0400  // 唤醒词/能量触发
```

---

## 三、线程模型

```
线程              锁              职责
main              无              初始化、信号等待
event_loop        无              取事件->拷贝订阅->释放锁->分发（不在锁内调回调）
bt_spp_thread     subs_lock       accept+read -> publish
bt_a2dp_thread    subs_lock       PCM接收 -> 环形缓冲 -> publish
ai_worker(N)      subs_lock       HTTP请求 -> publish
audio_thread      subs_lock       snd_pcm_writei -> publish
state_machine     s_sm.lock       dispatch 原子, 动作不回调状态机
```

---

## 四、事件ID全集

```c
EV_SYS_SHUTDOWN      = 0x0001  // SIGINT/SIGTERM -> 退出
EV_SYS_ERROR         = 0x0002  // 系统级错误
EV_BT_DEVICE_CONN    = 0x0103  // 蓝牙连接
EV_BT_DEVICE_DISCONN = 0x0104  // 蓝牙断开
EV_BT_DATA_RECEIVED  = 0x0105  // 文本命令
EV_AUDIO_PLAY_DONE   = 0x0201  // 播放完成
EV_AUDIO_MUSIC_START = 0x0202  // A2DP 音乐开始
EV_AUDIO_MUSIC_STOP  = 0x0203  // A2DP 音乐停止
EV_AUDIO_ERROR       = 0x0204  // 音频错误
EV_AI_RESP_READY     = 0x0300  // AI 回复就绪
EV_AI_TTS_DONE       = 0x0301  // TTS 合成完成
EV_AI_ERROR          = 0x0303  // AI 失败
EV_AI_STREAM_CHUNK   = 0x0304  // AI 流式数据块
EV_WAKEUP_DETECTED   = 0x0400  // 离线唤醒词/能量触发
EV_NET_CONNECTED     = 0x0500  // 网络恢复（预留）
EV_NET_DISCONNECTED  = 0x0501  // 网络断开（预留）
```

---

## 五、文件组织

```
code/smart-speaker/
+-- README.md              # 项目说明+构建部署指南
+-- ARCHITECTURE.md         # 架构文档
+-- Makefile                # 构建
+-- speaker.conf.example    # 配置示例
+-- inc/
|   +-- common.h / event_bus.h / state_machine.h / thread_pool.h
|   +-- module.h / config.h / logger.h / bt_manager.h
|   +-- audio_player.h / ai_client.h / voice_agent.h
|   +-- wake_word.h         # 唤醒词引擎接口（预留）
+-- src/
|   +-- main.c
|   +-- core/
|   |   +-- event_bus.c / state_machine.c / thread_pool.c
|   |   +-- module.c / voice_agent.c
|   +-- bluetooth/
|   |   +-- bt_manager.c / bt_a2dp.c
|   +-- audio/
|   |   +-- audio_player.c / alsa_capture.c / audio_mixer.c / audio_pipeline.c
|   +-- ai/
|   |   +-- ai_client.c / ai_conv.c / ai_tts.c
|   |   +-- wake_word.c     # 唤醒词引擎实现（预留）
|   +-- utils/
|       +-- logger.c / config.c
+-- tests/
    +-- test_event_bus.c / test_state_machine.c / test_thread_pool.c
```

---

## 六、错误处理标准

| 层级 | 策略 |
|------|------|
| init 阶段 | 任一步失败 -> 回滚所有已初始化模块 -> main 退出 |
| 运行时 | 单模块异常 -> 发 EV_*_ERROR -> voice_agent 回 IDLE |
| deinit | 某模块失败 -> 记日志 -> 继续 deinit 其他 |
| OOM | malloc/realloc 返回值全检查 -> ERR_NOMEM -> 调用方回滚 |
| 网络 | 超时 -> ERR_TIMEOUT -> 调用方重试 |
| 音频 | underrun -> snd_pcm_recover -> 超3次才报错 |

---

## 七、编码规范

1. 所有函数返回值必须检查
2. 每个 malloc 对应 free，init 对应 deinit
3. 全局状态用 static struct { ... } s_xxx = {0}
4. 内部函数加 static, 外部接口在头文件声明
5. 日志分级: DEBUG/INFO/WARN/ERROR
6. 注释用 /* */ 风格
7. 编译零警告（-Wall -Wextra -Werror）

---

## 八、开发计划

### Phase 1：架构闭环

| 模块 | 方式 | 行数 |
|------|------|:---:|
| voice_agent.c/h | 你手写, 我引导 | +250 |
| ai_conv.c 重构 | 你手改, 删编排逻辑 | -200 |
| main.c 调整 | 注册 voice_agent | 0 |
| 编译验证 | make, 零错误零警告 | -- |
| 结果 | 四层闭环, state_machine+thread_pool用上 | 2895 |

### Phase 2：企业级加固

| 模块 | 内容 | 行数 |
|------|------|:---:|
| event_bus.c | 锁内不调回调（先拷贝再分发） + 队列满返回ERR_FULL不阻塞 | +40 |
| state_machine.c | action 传 void*data | +20 |
| ai_client.c | HTTP 状态码检查 + 重试 | +50 |
| bt_manager.c | accept/read 加超时 | +30 |
| voice_agent.c | 状态超时监控 | +40 |
| 结果 | 企业级可靠性基线 | 3065 |

### Phase 3：服务层手写

| 模块 | 方式 | 行数 |
|------|------|:---:|
| ai_tts.c | 你手写（替换参考版） | 194 |
| audio_player.c | 你手写（替换参考版） | 324 |
| **ALSA 双工直通测试** | speaker-test + arecord 同时跑30min, 定死 period/buffer | -- |
| 结果 | 服务层全部你手写, 声卡双工已验证 | 3065（替换不增行） |

### Phase 4：蓝牙完整化

| 模块 | 内容 | 行数 |
|------|------|:---:|
| bt_manager.c 重写 | system() -> BlueZ D-Bus (SPP) | +300 |
| bt_a2dp.c | A2DP Sink, PCM 环形缓冲 | +250 |
| voice_agent.c | A2DP 状态转移 + ducking 逻辑 | +30 |
| 结果 | SPP+A2DP 双 Profile | 3445 |

### Phase 5：音频管道（Day12~13，7/27~28）

| 模块 | 内容 | 行数 |
|------|------|:---:|
| alsa_capture.c | USB声卡录音, 16bit 16kHz | +200 |
| audio_mixer.c | 软件混音, TTS ducking | +150 |
| audio_pipeline.c | 播放+录音+A2DP 协调 | +150 |
| 结果 | 音频子系统完整 | 3945 |

### Phase 6：联调+稳定性（Day14~15，7/29~30）

| 内容 | 行数 |
|------|:---:|
| Zero 3 实机部署 | -- |
| SPP AI 对话全链路 | -- |
| A2DP 音乐 + TTS ducking 场景 | -- |
| valgrind 内存泄漏检测 | -- |
| bugfix + 配置收尾 | +100 |
| 结果 | **4045 行** |

---

## 九、检查清单（每个模块写完对照）

- [ ] 所有 malloc/realloc/fopen 返回值检查
- [ ] 每个 malloc 对应 free 在 deinit 里
- [ ] init 失败路径回滚了所有已分配资源
- [ ] 所有函数参数 NULL 检查
- [ ] 关键路径有日志
- [ ] 编译零警告
- [ ] 线程安全：共享数据在锁内访问
