# 部署指南 — Orange Pi Zero 3

> 适用平台：Orange Pi Zero 3（ARM64 / H618）| 系统：Armbian（Debian bookworm）

---

## 目录

1. [系统准备](#一系统准备)
2. [依赖安装](#二依赖安装)
3. [构建](#三构建)
4. [配置](#四配置)
5. [蓝牙设置](#五蓝牙设置)
6. [运行](#六运行)
7. [自启动（systemd）](#七自启动systemd)
8. [验证](#八验证)
9. [故障排除](#九故障排除)

---

## 一、系统准备

### 1.1 烧录 Armbian

从 [armbian.com](https://www.armbian.com/) 下载 Orange Pi Zero 3 的 Armbian 镜像（Bookworm），用 `balenaEtcher` 或 `dd` 写入 TF 卡：

```bash
# Linux 下烧录
sudo dd if=Armbian_xxx_Orangepizero3_bookworm.img of=/dev/sdX bs=4M status=progress
```

### 1.2 首次启动

插入 TF 卡上电，首次启动会提示设置 root 密码和创建普通用户。

### 1.3 连接网络

```bash
# 查看无线网卡
ip link show wlan0

# 编辑 Netplan 配置连接 WiFi
nano /etc/netplan/armbian-default.yaml
```

示例配置（手机热点）：

```yaml
network:
  version: 2
  renderer: networkd
  wifis:
    wlan0:
      dhcp4: true
      access-points:
        "你的热点名":
          password: "你的热点密码"
```

```bash
netplan apply
# 查看获取的 IP
ip addr show wlan0
```

### 1.4 确认蓝牙

```bash
# 检查蓝牙控制器
hciconfig -a
# 或
bluetoothctl show

# 预期输出：有 Controller AE:xx:xx:xx:xx:xx 且 Powered: yes
```

如果蓝牙未开启：

```bash
bluetoothctl power on
```

---

## 二、依赖安装

```bash
apt update

# 编译工具 + 运行时库
apt install -y build-essential \
               libasound2-dev \
               libcurl4-openssl-dev \
               libbluetooth-dev \
               libsystemd-dev \
               libssl-dev

# A2DP 蓝牙音乐（bluez-alsa）
apt install -y bluez-alsa-utils

# 确认 bluealsa 服务已启动
systemctl enable --now bluealsa
systemctl status bluealsa
```

---

## 三、构建

在 Zero 3 上**原生编译**（不支持交叉编译，ARM64 库头文件依赖多）：

```bash
# 上传源码到 Zero 3，或在 Zero 3 上 git clone
git clone https://github.com/edwoerds/ai-speaker.git /opt/smart-speaker
cd /opt/smart-speaker

# 编译
make clean && make

# 产物在 output/speaker
ls -l output/speaker

# 运行单元测试
make test
```

预期编译输出：零错误零警告。

---

## 四、配置

复制配置文件并修改：

```bash
cp speaker.conf.example /etc/speaker.conf
chmod 600 /etc/speaker.conf   # 配置文件包含 API Key，限制权限
vi /etc/speaker.conf
```

### 配置项说明

```ini
[log]
level = info          ; debug | info | warn | error

[bluetooth]
device_name = AI-Speaker
discoverable_timeout = 300
auto_reconnect = true

[ai]
; DeepSeek API（支持 OpenAI 兼容接口）
api_url = https://api.deepseek.com/chat/completions
api_key = sk-your-api-key-here
model = deepseek-v4-flash
temperature = 0.7
max_tokens = 2048
timeout = 30000

; 系统提示词（AI 的角色设定）
system_prompt = 你是一个智能音箱助手。请用简洁、清晰的中文回答问题，保持回答在100字以内。

[tts]
; 腾讯云语音合成
secret_id = your-secret-id-here
secret_key = your-secret-key-here
voice_type = 0           ; 0=晓薇(女), 1=晓晓(女), 101001=智逸(男)
output_dir = /tmp/speaker

[audio]
device = default          ; ALSA PCM 设备名
```

### 声卡配置说明

Zero 3 HDMI 无音频输出，需使用 USB 声卡。

**查看可用声卡：**

```bash
aplay -l
arecord -l
```

**BH900 PRO USB 声卡（推荐）：**

```ini
[audio]
device = default:CARD=PRO
```

如果 `default:CARD=PRO` 不行，尝试：

```ini
[audio]
device = plughw:1,0      ; card 1 = USB 声卡，card 0 = HDMI（无声）
```

**音量调节：**

```bash
alsamixer
# F6 → 选择 USB 声卡 → 调高音量
```

---

## 五、蓝牙设置

### 5.1 基本配置

```bash
bluetoothctl
```

在 `bluetoothctl` 交互界面中依次执行：

```
power on
agent NoInputNoOutput
default-agent
discoverable on
pairable on
exit
```

音箱的蓝牙名称由 `speaker.conf` 中的 `device_name` 控制，默认为 `AI-Speaker`。

### 5.2 SPP 蓝牙串口

手机安装 **Serial Bluetooth Terminal**（Play Store）或其他蓝牙串口 App。

1. 手机搜索蓝牙设备，连接 `AI-Speaker`
2. 配对成功后，在 App 中打开蓝牙串口连接
3. 发送文字即可触发 AI 对话

### 5.3 A2DP 蓝牙音乐

A2DP 由 `bluez-alsa` 自动处理。手机连接蓝牙后，在手机上选择"媒体音频"。

**验证 A2DP 链路：**

```bash
# 查看 bluez-alsa 发现的 A2DP 源
bluealsa-aplay -L

# 手机连接后应有类似输出：
# bluealsa:DEV=XX:XX:XX:XX:XX:XX,PROFILE=a2dp
```

---

## 六、运行

### 前台运行

```bash
cd /opt/smart-speaker
./output/speaker -c /etc/speaker.conf
```

运行后日志会输出到终端。按下 `Ctrl+C` 停止。

### 调试模式

```ini
[log]
level = debug
```

---

## 七、自启动（systemd）

### 7.1 创建服务文件

```bash
nano /etc/systemd/system/speaker.service
```

```ini
[Unit]
Description=AI Speaker Service
After=network.target bluetooth.target bluealsa.service
Wants=bluetooth.target bluealsa.service

[Service]
Type=simple
ExecStart=/opt/smart-speaker/output/speaker -c /etc/speaker.conf
Restart=on-failure
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
```

### 7.2 启用并启动

```bash
systemctl daemon-reload
systemctl enable speaker
systemctl start speaker

# 查看状态
systemctl status speaker

# 查看日志
journalctl -u speaker -f
```

---

## 八、验证

### 8.1 AI 对话（本地 stdin）

```bash
./output/speaker -c /etc/speaker.conf
# 在终端直接打字回车，音箱应 TTS 播报回复
```

### 8.2 SPP 蓝牙串口

手机连蓝牙 `AI-Speaker` → 打开 Serial Bluetooth Terminal → 输入文字 → 音箱播报回复。

### 8.3 A2DP 蓝牙音乐

手机连蓝牙 → 播放音乐 → 音箱出声。

### 8.4 TTS Ducking

音乐播放中 → 通过手机串口发送文字 → 音乐暂停/衰减 → TTS 播报 → 播完音乐恢复。

### 8.5 唤醒检测（能量触发）

对着麦克风大声说话或拍手 → 音箱自动播报"你好"。

---

## 九、故障排除

### 编译错误

| 错误 | 原因 | 解决 |
|------|------|------|
| `fatal error: alsa/asoundlib.h` | 缺 ALSA 头文件 | `apt install libasound2-dev` |
| `fatal error: bluetooth/bluetooth.h` | 缺 BlueZ 头文件 | `apt install libbluetooth-dev` |
| `fatal error: systemd/sd-bus.h` | 缺 systemd 头文件 | `apt install libsystemd-dev` |
| `fatal error: openssl/hmac.h` | 缺 OpenSSL 头文件 | `apt install libssl-dev` |

### 蓝牙问题

| 现象 | 原因 | 解决 |
|------|------|------|
| 手机找不到 AI-Speaker | 蓝牙未开启或不可见 | `bluetoothctl discoverable on` |
| 配对失败 | 缺 agent | `bluetoothctl agent NoInputNoOutput` + `default-agent` |
| 串口连不上 | NewConnection 回调未触发 | 检查 bt_manager 日志是否有 `BT initialized` |
| A2DP 不出声 | 声卡设备选错 | 确认 `aplay -L` 结果，改 `speaker.conf` 中 `device` |
| A2DP 声音卡顿 | 环形缓冲不足 | `a2dp_ringbuf_frames = 256` 已默认，无需调整 |

### 声卡问题

| 现象 | 原因 | 解决 |
|------|------|------|
| 无声音 | 声卡设备名错误 | `aplay -l` 查看正确 card number |
| 播放错误 `Device or resource busy` | A2DP 和 TTS 抢声卡 | 已自动处理，确认 dmix 正常工作 |
| 录音无数据 | capture 设备名错误 | `arecord -l` 查看正确设备 |
| 唤醒不触发 | 能量阈值太高/太低 | 暂不可调，对着麦克风大声说话测试 |

### AI/TTS 问题

| 现象 | 原因 | 解决 |
|------|------|------|
| AI 返回空 | API Key 无效或模型名错误 | 检查 `api_key` 和 `model` |
| TTS 合成失败 | SecretId/SecretKey 错误 | 检查腾讯云密钥 |
| 响应慢 | API 超时 | 检查网络连接和 `timeout` 配置 |

### 排查通用命令

```bash
# 查看音箱日志（前台运行有详细输出）
./output/speaker -c /etc/speaker.conf

# 检查蓝牙状态
bluetoothctl show
bluetoothctl devices

# 检查 bluez-alsa 状态
systemctl status bluealsa
bluealsa-aplay -L

# ALSA 设备列表
aplay -l
arecord -l
aplay -L

# 测试声卡播放
speaker-test -D plughw:1,0 -c 2 -r 44100 -t sine

# 测试录音
arecord -D plughw:1,0 -f S16_LE -c 1 -r 16000 -d 5 test.wav
aplay test.wav
```
