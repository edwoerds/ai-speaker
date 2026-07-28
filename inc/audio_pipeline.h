#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include "common.h"

/*
 * 音频管道 — 统一音频调度入口
 *
 * 协调 audio_player（播放）、alsa_capture（录音）、audio_mixer（混音）
 * 提供统一的音频 API，隐藏背后的 ALSA 细节
 *
 * voice_agent 只需调 pipeline 接口，不直接操作音频模块。
 *
 * 用法：
 *   audio_pipeline_init(cfg);
 *   audio_pipeline_play_tts("/tmp/tts.wav");
 *   audio_pipeline_capture_start();
 *   audio_pipeline_deinit();
 */

/* ---------- 配置 ---------- */
typedef struct {
    const char *playback_device;  /* 播放设备名 ("default") */
    const char *capture_device;   /* 录音设备名 ("plughw:1,0") */
    unsigned int capture_rate;    /* 录音采样率 (16000) */
    const char *a2dp_device;      /* A2DP 播放设备 (NULL = 同 playback) */
    int a2dp_ringbuf_frames;      /* A2DP 环形缓冲帧数 (256) */
} audio_pipeline_config_t;

/* ---------- 初始化/反初始化 ---------- */
err_t audio_pipeline_init(const audio_pipeline_config_t *cfg);
void audio_pipeline_deinit(void);

/* ---------- TTS 播放 ---------- */
/*
 * 异步播放 TTS WAV 文件
 *   自动对 "music" 流做 ducking
 *   播完恢复 music 音量
 */
err_t audio_pipeline_play_tts(const char *filepath);

/* 停止 TTS 播放 */
void audio_pipeline_stop_tts(void);

/* 查询 TTS 是否正在播放 */
bool audio_pipeline_is_tts_playing(void);

/* ---------- 音乐（A2DP）控制 ---------- */
/*
 * 通知 pipeline 音乐开始/停止
 *   pipeline 据此管理 ducking 状态
 */
void audio_pipeline_music_start(void);
void audio_pipeline_music_stop(void);

/* 查询音乐是否在播放 */
bool audio_pipeline_is_music_playing(void);

/* ---------- 录音控制 ---------- */
err_t audio_pipeline_capture_start(void);
void audio_pipeline_capture_stop(void);
bool audio_pipeline_is_capturing(void);

#endif /* AUDIO_PIPELINE_H */