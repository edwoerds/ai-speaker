#ifndef AI_STT_H
#define AI_STT_H

#include "common.h"

/*
 * 语音识别模块（Speech To Text）— 百度短语音识别 API
 *
 * 将 PCM 音频（16kHz/16bit/mono）发送到百度云端语音识别，
 * 返回识别文本。内部通过 libcurl POST 实现 HTTP 调用。
 *
 * 使用流程：
 *   1. ai_stt_init(&cfg)     — 初始化，获取百度 access_token
 *   2. ai_stt_process(...)   — PCM → 文本（caller free 返回值）
 *   3. ai_stt_deinit()       — 清理
 *
 * 配置项（speaker.conf [stt] 段）：
 *   api_key    — 百度 API Key
 *   secret_key — 百度 Secret Key
 */

typedef struct {
    const char *api_key;      /* 百度 API Key */
    const char *secret_key;   /* 百度 Secret Key */
    int         timeout_ms;   /* HTTP 超时（毫秒） */
} ai_stt_config_t;

/* 初始化 STT 模块（获取 access_token） */
err_t ai_stt_init(const ai_stt_config_t *cfg);

/* 反初始化 */
void ai_stt_deinit(void);

/*
 * 语音识别：PCM → 文本
 *
 * audio     - PCM 数据（16kHz, 16bit, mono）
 * frames    - 帧数
 * out_text  - 输出文本（malloc 分配，调用方 free）
 *
 * 返回: ERR_OK 成功, 其他 失败
 *
 * 注意: 如果 cfg->api_key 为空（用户未配置），函数返回 ERR_NOT_FOUND，
 *        调用方降级为默认打招呼流程。
 */
err_t ai_stt_process(const int16_t *audio, size_t frames, char **out_text);

#endif /* AI_STT_H */
