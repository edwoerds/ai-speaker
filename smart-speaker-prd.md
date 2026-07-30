# 蓝牙AI智能音箱 — 产品需求分析

> 版本：v1.0 | 日期：2026-07-16 | 作者：曹彬
> 平台：香橙派 Zero 3（ARM64） | 系统：Armbian | 语言：C

---

## 1. 产品定位

### 1.1 一句话定义
一款基于ARM Linux的蓝牙AI智能音箱，支持手机蓝牙音乐播放、AI语音对话、智能助手三大核心功能。

### 1.2 用户场景
| 场景 | 描述 | 优先级 |
|------|------|:------:|
| 蓝牙音响 | 用户手机连接音箱，播放音乐（A2DP Sink） | P0 |
| AI语音聊天 | 用户说"小X小X，今天天气怎么样"，AI回答并播报 | P0 |
| AI文字助手 | 用户通过手机蓝牙串口发送文字，AI回复播报 | P0 |
| 手机语音输入 | 用户蓝牙连接后，用手机麦克风说话→音箱调用AI→播报 | P1 |

### 1.3 竞品对标
- **天猫精灵/小爱同学**：完全对标的产品形态，我们只做核心功能
- **百元级蓝牙音箱**：增加AI能力，形成差异化

---

## 2. 硬件平台

### 2.1 硬件清单
| 组件 | 型号/规格 | 状态 |
|------|----------|:----:|
| SoC | 全志 H618, 4×Cortex-A53 @1.5GHz | ✅ 已集成 |
| 内存 | 1GB DDR3 | ✅ 已集成 |
| 存储 | TF卡 32GB | ✅ 已购买 |
| 蓝牙 | 板载RTL8821CS（或同封装蓝牙芯片） | ✅ 已集成 |
| WiFi | 板载 | ✅ 已配置 |
| 音频输出 | USB声卡 | ⏳ 约7/20到货 |
| 扬声器 | 有源音箱/3.5mm音箱 | ❌ 需要自备 |
| 麦克风 | USB声卡自带mic（或外接USB麦克风） | ⏳ 待确认 |

### 2.2 硬件约束
| 参数 | 值 | 对软件的影响 |
|------|----|-------------|
| CPU频率 | 1.5GHz | 软件TTS（espeak）实时可用，但AI大模型需云端 |
| 内存 | 1GB | 不能本地跑大模型，纯云端方案 |
| 存储 | 32GB | 绰绰有余，日志+配置+缓存随意 |
| USB声卡延迟 | 典型20-50ms | 全双工需PCM buffer调优 |

---

## 3. 软件架构

### 3.1 层级划分

```
┌─────────────────────────────────────────────────────────┐
│                    应用层 Application                    │
│  ┌──────────────────────────────────────────────────┐   │
│  │              voice_agent (主控状态机)              │   │
│  │    IDLE → WAKEUP → LISTEN → PROCESS → SPEAK      │   │
│  └──────┬──────────┬──────────┬──────────┬──────────┘   │
│         │          │          │          │               │
│    ┌────┴───┐ ┌───┴────┐ ┌───┴────┐ ┌──┴──────┐        │
│    │ 蓝牙   │ │ 音频   │ │ AI     │ │ 配置/日志│        │
│    │ 管理层 │ │ 管道   │ │ 引擎   │ │ 工具    │        │
│    └───┬───┘ └───┬────┘ └───┬────┘ └──┬──────┘        │
├────────┼─────────┼──────────┼─────────┼────────────────┤
│        │   服务抽象层 Service Abstraction               │
│  ┌─────┴──────┐ ┌┴────────┐ ┌┴────────┐                │
│  │  BlueZ     │ │ ALSA    │ │ libcurl  │                │
│  │  D-Bus lib │ │ asound  │ │          │                │
│  └────────────┘ └─────────┘ └──────────┘                │
├─────────────────────────────────────────────────────────┤
│                  事件总线 Event Bus                      │
│         publish/subscribe 模块间解耦通信                 │
├─────────────────────────────────────────────────────────┤
│                   底层框架 Framework                     │
│        线程池 · 状态机 · 模块管理器 · 公共类型           │
└─────────────────────────────────────────────────────────┘
```

### 3.2 架构设计原则
1. **模块间解耦**：所有模块间通信走事件总线，禁止直接函数调用
2. **统一生命周期**：每个模块注册 init/deinit，main 按序启动/停止
3. **错误不崩溃**：任何一个模块异常，不影响其他模块运行
4. **可测试**：每个模块可以独立编译测试

### 3.3 事件总线设计

```
事件定义（枚举）：
├── BT_DEVICE_CONNECTED      // 蓝牙设备已连接
├── BT_DEVICE_DISCONNECTED   // 蓝牙设备已断开
├── BT_DATA_RECEIVED         // 收到蓝牙数据（文本命令）
├── AUDIO_PLAYBACK_DONE      // 音频播放完毕
├── AI_RESPONSE_READY        // AI回复已就绪
├── AI_TTS_DONE              // TTS生成完毕
├── VOICE_WAKEUP_DETECTED    // 检测到唤醒词
├── VOICE_RECOGNIZED         // 语音识别结果
├── SYS_SHUTDOWN             // 系统关闭信号
└── SYS_ERROR                // 系统级错误
```

---

## 4. 详细功能需求

### 4.1 蓝牙模块 (P0)

#### 功能清单
| 功能 | 描述 | 验收标准 |
|------|------|---------|
| 蓝牙适配器初始化 | 打开蓝牙、设置可发现、可连接 | `hcitool dev` 可看到设备 |
| 设备扫描 | 扫描附近蓝牙设备 | 能发现手机蓝牙 |
| 配对处理 | 接受或发起配对请求 | 手机能配对成功 |
| SPP服务 | 注册RFCOMM串口服务 | 手机蓝牙串口APP能连接并收发数据 |
| A2DP Sink | 注册音频接收服务 | 手机播放音乐，音箱出声 |
| 断线重连 | 蓝牙断开后自动扫描重连 | 手机关闭蓝牙再打开，自动连回 |
| 多设备支持 | 可记忆最近3台配对设备 | 重新上电自动回连 |

#### D-Bus 接口交互（面试重点）
```
BlueZ D-Bus 核心接口：
  org.bluez.Adapter1:
    → StartDiscovery()          // 开始扫描
    → StopDiscovery()           // 停止扫描
    → RemoveDevice()            // 删除绑定设备

  org.bluez.Device1:
    → Pair()                     // 配对
    → Connect()                  // 连接
    → Disconnect()               // 断开
    ♢ PropertiesChanged 信号     // 设备状态变化通知

  org.bluez.Profile1:
    → NewConnection()            // 新SPP连接建立
    → RequestDisconnection()     // SPP断开请求
```

**面试可展开：** D-Bus消息格式、BlueZ对象路径树、信号匹配规则、PropertyChanged异步通知处理

### 4.2 音频模块 (P0)

#### 功能清单
| 功能 | 描述 | 验收标准 |
|------|------|---------|
| PCM播放 | ALSA播报AI回复 | 人声清晰，无杂音 |
| PCM播放参数探测 | 自动检测USB声卡支持的采样率/格式 | 对不同声卡自适应 |
| Underrun恢复 | 播放中断（underrun）时自动恢复 | 连续播放24h不卡死 |
| 音量控制 | ALSA mixer音量调节 | `amixer` 可调，重启保持 |
| 录音 | USB声卡mic录音，16bit 16kHz | WAV文件可正常播放 |
| 音频管道 | TTS+音乐软件混音 | TTS播报时音乐音量自动降低（ducking） |

#### PCM参数协商（面试重点）
```
snd_pcm_hw_params_t 配置流程：
1. 打开设备 "default" 或 "hw:1,0"
2. snd_pcm_hw_params_any() — 获取设备全部能力
3. snd_pcm_hw_params_set_access() — 设置交错模式
4. snd_pcm_hw_params_set_format() — 设置S16_LE格式
5. snd_pcm_hw_params_set_rate_near() — 设置采样率（优先16000/44100）
6. snd_pcm_hw_params_set_channels_near() — 声道数
7. snd_pcm_hw_params() — 写入配置
8. snd_pcm_prepare() — 准备就绪
9. snd_pcm_sw_params_current/set/write() — 软件参数（threshold等）
10. snd_pcm_writei() — 开始写入数据

关键参数：
  buffer_size = period_size × period_count
  period_size 越小 → 延迟越低 → CPU占用越高
  period_size 越大 → 延迟越高 → CPU占用越低
  语音场景原则：period_size偏小（低延迟）
  音乐场景原则：period_size偏大（防断续）
```

**面试可展开：** 
- 为什么出现underrun？—— 应用层没及时喂数据，DMA已播完
- 为什么用`_near`函数？—— 声卡硬件不一定支持你指定的精确值
- `hw:1,0` vs `default` 区别？—— 前者绕开dmix直接操作硬件，后者经过alsa-lib混音
- 双声道降单声道怎么做？—— 软件downmix，取左右平均值或只取左声道

### 4.3 AI引擎模块 (P0)

#### 功能清单
| 功能 | 描述 | 验收标准 |
|------|------|---------|
| HTTP客户端 | libcurl POST/GET封装 | 能调通AI API |
| 流式SSE解析 | AI流式返回边收边播 | 首字延迟<2s |
| 指数退避重试 | 网络异常自动重试 | 连续失败3次后放弃 |
| 多轮对话 | 管理最近N轮对话context | "刚才说的..." 能理解上下文 |
| Token裁剪 | context超限时，丢弃最旧轮次 | 始终不超过模型token限制 |
| TTS对接 | 调用百度/TTS API | 返回音频播放 |
| 多模型切换 | 智能问答用大模型，TTS用小模型 | 配置选择 |
| 降级策略 | 网络不可用时本地回复"网络异常" | 不上天也不崩溃 |

#### AI API调用流程（面试重点）
```
VoiceAgent → AI引擎 → libcurl → [HTTP请求] → AI云服务
                                         ↓
VoiceAgent ← AI引擎 ← cJSON解析 ← [HTTP响应] ←┘

流式模式（SSE）：
AI引擎 → libcurl → 持续读取HTTP流
         ↓
    每收到一个 chunk：
      ↓
    累积 + cJSON解析
      ↓
    如果收到完整句子 → 抛事件给VoiceAgent → 触发TTS+播放
```

**面试可展开：**
- HTTP长连接 vs 短连接选择？—— Keep-Alive复用，避免每次建连3次握手
- SSE协议是什么？—— Server-Sent Events，HTTP长连接，`data: ...\n\n` 分割
- 为什么边收边播？—— 减少用户等待感，首字延迟从5s降到1.5s
- Token裁剪算法？—— 维护滑动窗口，超出limit时从最旧消息开始丢弃，保留system prompt

### 4.4 主控状态机 (P0)

#### 状态转移表
```
当前状态        事件                   下一状态          动作
───────────    ──────────────         ───────────      ─────────────────
IDLE           BT_DATA_RECEIVED       PROCESSING       下发AI请求
IDLE           VOICE_WAKEUP           LISTENING        开启录音
LISTENING      VOICE_RECOGNIZED       PROCESSING       停止录音，下发AI请求
LISTENING      VOICE_TIMEOUT          IDLE             超时无语音，回到空闲
PROCESSING     AI_RESPONSE_READY      SPEAKING         开始TTS+播放
PROCESSING     AI_ERROR               IDLE             播报"出错了"回到空闲
SPEAKING       AUDIO_PLAYBACK_DONE    IDLE             播放完毕回到空闲
IDLE           BT_DISCONNECTED        IDLE             日志记录，不改变状态
ANY            SYS_SHUTDOWN           EXITING          资源清理，退出
ANY            SYS_ERROR              IDLE             日志记录错误，回到安全状态
```

**面试可展开：**
- 为什么用状态表不用if-else？—— 状态表是数据驱动的，新增状态不改代码逻辑
- 状态表的数据结构？—— 二维数组/链表，表项 = {当前状态, 事件, 下一状态, 动作函数指针}
- 怎么保证状态机的线程安全？—— 状态切换加锁，事件队列串行化

### 4.5 线程模型

```
主线程（main）：
  │
  ├─ 模块初始化（按注册顺序）
  ├─ 信号处理（SIGINT/SIGTERM）
  └─ 等待所有线程退出

事件分发线程（Event Loop）：
  │
  从事件队列取事件 → 查状态机表 → 执行动作

蓝牙线程（BT Thread）：
  │
  ├─ D-Bus主循环（g_main_loop_run）
  ├─ 监听SPP数据
  └─ 蓝牙事件 → 封装成事件 → 抛入事件队列

音频线程（Audio Thread）：
  │
  ├─ 播放PCM数据
  ├─ 录音PCM数据
  └─ 音频事件 → 封装成事件 → 抛入事件队列

AI工作线程（AI Worker Pool）：
  │
  ├─ 从任务队列取AI请求
  ├─ HTTP调用AI API
  └─ AI完成 → 封装成事件 → 抛入事件队列
```

**线程安全说明：**
- 所有模块通过事件队列间接通信，无直接数据竞争
- 事件队列本身加锁（mutex + condition variable）
- 状态机只在事件循环线程中执行，天然串行
- 蓝牙/AI线程的发送是"写事件队列"，是唯一需要加锁的地方

---

## 5. 非功能性需求

### 5.1 可靠性
| 要求 | 指标 | 实现手段 |
|------|------|---------|
| 7×24h不崩溃 | 连续运行7天无异常 | 内存泄漏检测（valgrind） |
| 蓝牙断线恢复 | 30s内自动重连 | 指数退避重试机制 |
| 网络异常处理 | 不崩溃，有降级提示 | curl超时配置（5s connect + 10s data） |
| 音频异常恢复 | underrun自动恢复 | ALSA错误码检测+snd_pcm_recover |

### 5.2 性能
| 指标 | 目标 | 备注 |
|------|:----:|------|
| AI应答首字延迟 | <2s | 流式SSE+TTS并行化 |
| CPU占用 | 空闲<5%，忙碌<50% | 线程池控制并发数 |
| 内存占用 | <50MB常驻 | 静态分配优先，动态池化 |
| 启动时间 | <10s | 延迟初始化非关键模块 |

### 5.3 电源管理
| 场景 | 行为 |
|------|------|
| 蓝牙断开>5min | 降低扫描频率（从5s/次→60s/次） |
| 系统空闲>30min | 进入低功耗模式（可选） |

---

## 6. 文件组织

```
code/smart-speaker/
├── Makefile
├── README.md
├── config/
│   └── speaker.conf              # 运行时配置文件
│
├── inc/                          # 公共头文件
│   ├── common.h                  # 公共类型、错误码、宏
│   ├── event_bus.h               # 事件总线接口
│   ├── state_machine.h           # 状态机接口
│   ├── thread_pool.h             # 线程池接口
│   ├── module.h                  # 模块注册框架
│   ├── logger.h                  # 日志接口
│   └── config.h                  # 配置解析接口
│
├── src/
│   ├── main.c                    # 初始化 + 信号处理 + 优雅退出
│   ├── core/
│   │   ├── event_bus.c           # 事件总线实现
│   │   ├── state_machine.c       # 状态机实现
│   │   ├── thread_pool.c         # 线程池实现
│   │   └── module.c              # 模块管理器实现
│   │
│   ├── bluetooth/
│   │   ├── bt_manager.c          # BlueZ D-Bus 全管理
│   │   ├── bt_spp.c              # SPP串口服务
│   │   └── bt_a2dp.c             # A2DP音频接收
│   │
│   ├── audio/
│   │   ├── alsa_playback.c       # PCM播放
│   │   ├── alsa_capture.c        # 录音
│   │   ├── audio_mixer.c         # 软件混音
│   │   └── audio_pipeline.c      # 音频管道编排
│   │
│   ├── ai/
│   │   ├── ai_client.c           # HTTP封装
│   │   ├── ai_stream.c           # 流式SSE解析
│   │   ├── ai_tts.c              # 文字转语音
│   │   ├── ai_stt.c              # 语音转文字（预留）
│   │   └── ai_conv.c             # 多轮对话管理
│   │
│   └── utils/
│       ├── logger.c              # 线程安全日志
│       └── config.c              # 配置解析
│
└── tests/                        # 单元测试（预留）
    ├── test_event_bus.c
    ├── test_state_machine.c
    └── test_thread_pool.c
```

### 6.1 模块间依赖关系（面试重点）
```
                  main.c
                    │
           ┌────────┼────────┐
           │        │        │
       module.c  event_bus  thread_pool
           │        │        │
    ┌──────┼────────┼────────┘
    │      │        │
    ▼      ▼        ▼
bt_manager  audio_pipeline  ai_client
    │           │              │
    ▼           ▼              ▼
  bt_spp    alsa_playback   ai_stream
  bt_a2dp   alsa_capture    ai_tts
              audio_mixer   ai_conv
                              │
                          ai_stt (预留)
```

**依赖规则（面试可展开）：**
1. 只允许上层依赖下层，禁止循环依赖
2. 同层模块间通过event_bus通信，不直接调用
3. 每个模块通过`module_register()`注册init/deinit，main不硬编码模块列表
4. 模块init顺序由注册优先级决定（先框架→再服务→最后应用）

---

## 7. 配置文件设计

```ini
# speaker.conf — 蓝牙AI音箱配置文件

[bluetooth]
device_name = AI-Speaker
discoverable_timeout = 300        # 可发现时间（秒）
scan_interval = 5                 # 扫描间隔（秒）
auto_reconnect = true             # 断线自动重连
max_paired_devices = 3            # 记忆配对设备数

[audio]
device = default                  # ALSA设备名
sample_rate = 16000               # 采样率
channels = 1                      # 声道数
volume = 80                       # 音量百分比
duck_volume = 30                  # TTS播报时音乐降低到x%

[ai]
api_url = https://api.deepseek.com/v1/chat/completions
api_key = sk-xxxxxxxxxxxx          # API密钥
model = deepseek-chat             # 模型名
system_prompt = 你是AI音箱助手，请用简短友好的中文回答。
max_tokens = 1024
timeout_connect = 5               # 连接超时（秒）
timeout_data = 10                 # 数据传输超时（秒）
retry_times = 3                   # 失败重试次数

[tts]
provider = edge-tts               # 可选: edge-tts / baidu / espeak
language = zh-CN
voice = zh-CN-XiaoxiaoNeural

[wakeword]
enabled = false                   # 唤醒词开关（需训练模型）
keyword = 小艾小艾
sensitivity = 0.7

[log]
level = info                      # debug/info/warn/error
file = /var/log/speaker.log
max_size = 10485760               # 单文件最大10MB
max_files = 3                     # 保留3个轮换文件
```

---

## 8. 部署方案

### 8.1 构建
```bash
# 交叉编译（在PC上）
make CROSS_COMPILE=aarch64-linux-gnu-

# 或本地编译（在Zero 3上）
make

# 产物
output/
└── speaker                      # 静态链接的可执行文件
```

### 8.2 运行
```bash
# 首次部署
scp output/speaker root@192.168.77.100:/root/
scp config/speaker.conf root@192.168.77.100:/etc/

# 作为系统服务（/etc/systemd/system/speaker.service）
[Unit]
Description=AI Smart Speaker
After=bluetooth.target sound.target network.target

[Service]
ExecStart=/root/speaker -c /etc/speaker.conf
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target

# 启动
systemctl enable speaker
systemctl start speaker
```

---

## 9. 验收标准

### 9.1 功能验收
| 测试项 | 步骤 | 预期结果 |
|-------|------|---------|
| 蓝牙配对 | 手机打开蓝牙，搜索到AI-Speaker，点击配对 | 配对上，无需密码 |
| SPP文本命令 | 手机蓝牙串口APP发送"今天天气" | 音箱回答天气并播报 |
| A2DP音乐 | 手机播放音乐，选择AI-Speaker播放 | 音箱出声，音质正常 |
| 断线重连 | 手机蓝牙关闭再打开 | 30s内自动重连 |
| 多轮对话 | "介绍深圳"→"有什么好吃的" | 上下文连贯，知道在说深圳 |
| 持续运行 | 运行24h | 无崩溃，内存不增长 |

### 9.2 代码质量验收
- [ ] valgrind 0 内存泄漏
- [ ] 无未处理的函数返回值
- [ ] 所有malloc对应free
- [ ] 所有文件描述符正确关闭
- [ ] 线程退出前已join
- [ ] 无全局变量（除配置外）
- [ ] 错误信息有代码位置+时间戳

---

## 10. 迭代路线图

> 架构文档：`code/smart-speaker/ARCHITECTURE.md`（唯一架构真相源）

```
Week 1 (7/16-7/19，声卡到前)
  Day1: 项目骨架 + 事件总线 + 状态机 + 线程池 ✓
  Day2: 蓝牙模块(bt_manager + bt_spp) ✓
  Day3: AI客户端(HTTP封装 + 流式解析) ✓ —— ai_client.c/ai_conv.c/ai_tts.c 已写
  Day4: 架构重构 + voice_agent 应用层
        · 新建 voice_agent.c/h（主控状态机，接上 state_machine + thread_pool）
        · 重构 ai_conv.c（删编排逻辑，只留历史管理）
        · ai_tts/audio_player 独立注册模块
        · 编译验证闭环

Week 2 (7/20-7/23，声卡到位后)
  Day5: audio_player.c 手写 + 讲解（ALSA PCM 播放器）
  Day6: alsa_capture（录音） + audio_mixer（混音，TTS ducking）
  Day7: 全链路联调（蓝牙→AI→TTS→播放，全流程验证）
  Day8: D-Bus 蓝牙集成 + 稳定性测试 + bugfix + 配置收尾

Bonus（有余力）
  - 语音唤醒词集成（Porcupine）
  - WiFi配网页面（BLE或HTTP AP）
  - OLED显示（i2c驱动）
```

---

## 11. 面试话术储备

### "介绍一下你的智能音箱项目"

**30秒电梯版：**
"这是一个基于香橙派Zero3的AI智能音箱，我用C语言写了4700行代码。架构上分了四层：底层框架（事件总线/状态机/线程池）、服务层（BlueZ蓝牙/ALSA音频/curl AI客户端）、应用层（主控状态机编排流程）、工具层（日志/配置）。核心功能：蓝牙音乐播放、蓝牙文字命令AI应答、语音唤醒AI对话。硬件上跑在ARM64 Linux上，部署为systemd服务。"

**2分钟展开版：**
（按面试官兴趣选重点模块展开，见各模块"面试可展开"部分）

### "这个项目里最难的地方"
（备选3个，看面试官方向选）
1. **蓝牙D-Bus**：BlueZ这套异步D-Bus接口跟普通socket编程完全是两回事，状态变化靠信号回调，调试起来很痛苦
2. **ALSA音频**：PCM参数协商、underrun恢复这些坑很多，一开始播放经常断断续续，调了buffer_size才解决
3. **流式SSE**：AI返回是流式的，要边收边解析边播，不能等全收完再播否则用户等太久

---

## 12. 附录

### 参考资源
| 内容 | 链接/关键词 | 阶段 |
|------|-----------|:----:|
| BlueZ D-Bus API | git.kernel.org - bluez/doc | 蓝牙 |
| ALSA编程指南 | alsa-project.org/alsa-doc | 音频 |
| libcurl easy API | curl.se/libcurl/c | AI |
| cJSON | github.com/DaveGamble/cJSON | AI |
| Porcupine唤醒词 | github.com/Picovoice/porcupine | 进阶 |

### 术语表
| 缩写 | 全称 | 说明 |
|------|------|------|
| A2DP | Advanced Audio Distribution Profile | 蓝牙音频传输协议 |
| SPP | Serial Port Profile | 蓝牙串口仿真协议 |
| D-Bus | Desktop Bus | Linux桌面/嵌入式IPC总线 |
| ALSA | Advanced Linux Sound Architecture | Linux音频架构 |
| PCM | Pulse Code Modulation | 脉冲编码调制（数字音频格式） |
| SSE | Server-Sent Events | 服务器推送事件（HTTP流式响应） |
| TTS | Text To Speech | 文字转语音 |
| STT | Speech To Text | 语音转文字 |
