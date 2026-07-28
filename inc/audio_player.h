#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "common.h"

/*
 * 音频播放器 — ALSA PCM 后端
 *
 * 支持 WAV 格式播放（44.1kHz / 48kHz, 16bit, mono/stereo）
 * 播放完成后发布 EV_AUDIO_PLAY_DONE 事件
 *
 * 用法：
 *   audio_player_init("default");
 *   audio_player_play("/tmp/speaker/tts_output.wav");
 *   audio_player_deinit();
 */

/* ---------- 配置 ---------- */
typedef struct {
    const char *device;   /* ALSA PCM 设备名（"default", "plughw:0,0" 等） */
} audio_player_config_t;

err_t audio_player_init(const audio_player_config_t *cfg);
void audio_player_deinit(void);

/* 阻塞播放 WAV 文件，播放完才返回 */
err_t audio_player_play(const char *filepath);

/* 异步播放（单独线程），播放完发布 EV_AUDIO_PLAY_DONE */
err_t audio_player_play_async(const char *filepath);

/* 停止当前播放 */
void audio_player_stop(void);
/* === 流式模式（A2DP 连续 PCM 播放） === */
/*
 * 开始流式播放
 *   rate: 采样率（44100）
 *   channels: 声道数（2）
 *   返回 ERR_OK 成功，ERR_BUSY 如果正在放文件
 */
err_t audio_player_stream_start(unsigned int rate, int channels);

/*
 * 写入 PCM 帧（16bit 立体声交错）
 *   data: PCM 数据
 *   frames: 帧数
 *   阻塞直到数据写入 ALSA 缓冲区
 */
err_t audio_player_stream_write(const void *data, size_t frames);

/* 停止流式播放，关闭 ALSA 设备 */
void audio_player_stream_stop(void);
#endif /* AUDIO_PLAYER_H */
