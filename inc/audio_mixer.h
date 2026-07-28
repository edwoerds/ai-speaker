#ifndef AUDIO_MIXER_H
#define AUDIO_MIXER_H

#include "common.h"

/*
 * 软件混音器 — 多路 PCM 混音 + 集中式 Ducking
 *
 * 支持多路输入流，每路独立增益控制。
 * TTS ducking 集中管理，不再分散在各模块。
 *
 * 用法：
 *   mixer_init();
 *   int ms = mixer_stream_register("music",  1.0f);
 *   int ts = mixer_stream_register("tts",    1.0f);
 *   mixer_stream_set_gain(ms, 0.3f);     // ducking
 *   mixer_stream_set_gain(ms, 1.0f);     // restore
 *   mixer_mix(src1, src2, dst, frames);  // 混音
 *   mixer_deinit();
 */

#define MIXER_MAX_STREAMS  8   /* 最大输入流数 */
#define MIXER_MAX_CHANNELS 2   /* 最大声道数 */

/* 流信息 */
typedef struct {
    char  name[32];        /* 流名（"music", "tts", "capture" 等） */
    float gain;            /* 增益 0.0 ~ 1.0 */
    bool  active;          /* 是否有数据 */
} mixer_stream_t;

/* ---------- 初始化 ---------- */
err_t mixer_init(void);
void mixer_deinit(void);

/* ---------- 流管理 ---------- */
int  mixer_stream_register(const char *name, float initial_gain);
void mixer_stream_unregister(int stream_id);
int  mixer_stream_find(const char *name);

/* ---------- 增益控制 ---------- */
err_t mixer_stream_set_gain(int stream_id, float gain);
float mixer_stream_get_gain(int stream_id);
void  mixer_set_master_gain(float gain);
float mixer_get_master_gain(void);

/* ---------- Ducking（集中管理） ---------- */
err_t mixer_duck_start(const char *stream_name, float duck_gain);
void  mixer_duck_restore(const char *stream_name);

/* ---------- PCM 混音 ---------- */
/*
 * 将两路 16bit PCM 混音到输出缓冲
 *   src1, src2: 输入 PCM（16bit, 交错）
 *   dst:        输出缓冲（可 = src1 原地混）
 *   frames:     帧数
 *   channels:   声道数
 *   算法：sum * gain_master，防溢出 clamp(-32768, 32767)
 */
void mixer_mix_2ch(const int16_t *src1, float gain1,
                   const int16_t *src2, float gain2,
                   int16_t *dst, size_t frames, int channels);

#endif /* AUDIO_MIXER_H */