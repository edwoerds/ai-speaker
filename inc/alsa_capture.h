#ifndef ALSA_CAPTURE_H
#define ALSA_CAPTURE_H

#include "common.h"

/*
 * ALSA 录音模块 — USB 声卡音频输入
 *
 * 从 USB 声卡 mic 录制 PCM 音频（16kHz / 16bit / mono）
 * 录音数据通过事件总线发布给语音识别模块
 *
 * 用法：
 *   alsa_capture_init("plughw:1,0");
 *   alsa_capture_start();                   // 启动独立录音线程
 *   ... 收到 EV_AUDIO_CAPTURE_DATA 事件 ...
 *   alsa_capture_stop();
 *   alsa_capture_deinit();
 */

/* ---------- 配置 ---------- */
typedef struct {
    const char *device;   /* ALSA PCM 设备名（"plughw:1,0", "default" 等） */
    unsigned int rate;    /* 采样率（推荐 16000） */
    int channels;         /* 声道数（推荐 1, mono） */
} alsa_capture_config_t;

/* 录音数据帧（随 EV_AUDIO_CAPTURE_DATA 发布） */
typedef struct {
    void   *data;         /* PCM 数据（16bit signed, mono） */
    size_t  frames;       /* 帧数 */
    unsigned int rate;    /* 采样率 */
} capture_data_t;

/* 初始化录音模块 */
err_t alsa_capture_init(const alsa_capture_config_t *cfg);

/* 反初始化 */
void alsa_capture_deinit(void);

/* 启动录音（创建独立 capture 线程） */
err_t alsa_capture_start(void);

/* 停止录音（停止线程） */
void alsa_capture_stop(void);

/* 查询是否在录音中 */
bool alsa_capture_is_running(void);

/* 唤醒检测静音控制（TTS 播报时静音，防止自触发） */
void alsa_capture_set_wake_muted(bool muted);

#endif /* ALSA_CAPTURE_H */