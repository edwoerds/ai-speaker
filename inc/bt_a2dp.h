#ifndef BT_A2DP_H
#define BT_A2DP_H

#include "common.h"

/*
 * A2DP Sink 模块 — 接收手机蓝牙音乐 → PCM 环形缓冲 → ALSA 播放
 *
 * init:
 *   1. 注册 BlueZ A2DP Sink Profile（D-Bus MediaEndpoint1）
 *   2. 启动 ALSA 播放线程（从环形缓冲取数据播放）
 *   3. 发布事件: EV_AUDIO_MUSIC_START / STOP
 *
 * ducking:
 *   手机音乐播放时收到 TTS 播报 → PCM 采样值 *0.3 软件衰减
 *   TTS 播完 → 恢复 *1.0
 *   （USB 声卡无硬件混音器，CPU 增约 2%）
 */

typedef struct {
    const char *device;           /* ALSA PCM 设备名（"default", "plughw:0,0"） */
    int         ringbuf_frames;   /* 环形缓冲容量（帧数，默认 256） */
} bt_a2dp_config_t;

err_t bt_a2dp_init(const bt_a2dp_config_t *cfg);
void bt_a2dp_deinit(void);
void bt_a2dp_shutdown(void);

/* ducking 控制：true=TTS 播报中，音量 *0.3；false=恢复 */
void bt_a2dp_set_ducking(bool duck);

/* pause: TTS 期间禁止 A2DP 重启流，避免抢声卡 */
void bt_a2dp_set_pause(bool pause);

#endif /* BT_A2DP_H */