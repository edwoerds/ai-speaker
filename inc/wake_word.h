#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include "common.h"
#include <stdint.h>

/*
 * 离线唤醒词引擎 — Porcupine 封装
 *
 * 此模块演示了唤醒词引擎的标准接口。
 * 当前系统使用的是 alsa_capture.c 中的能量检测方案。
 * 拿到 Picovoice AccessKey 后，替换 wake_word.c 实现即可。
 *
 * 已验证通过：使用 Picovoice 7 天试用 Key 跑通了全链路。
 * 试用过期后切换为能量检测 fallback，接口层保持不变。
 */

err_t wake_word_init(const char *model_path, const char *keyword_path,
                     const char *access_key, float sensitivity);
void wake_word_deinit(void);
bool wake_word_is_ready(void);

/*
 * 处理 PCM 音频帧
 *   pcm:    16kHz, 16-bit, mono PCM 数据
 *   frames: 帧数（建议 512，与 capture period 一致）
 */
void wake_word_process(const int16_t *pcm, size_t frames);

#endif /* WAKE_WORD_H */
