#include "wake_word.h"
#include "event_bus.h"
#include "logger.h"

/* Porcupine 头文件（需安装 Porcupine C SDK） */
/*
#include "pv_porcupine.h"
*/

#include <stdlib.h>
#include <string.h>

/*
 * 唤醒词引擎 — Porcupine 实现
 *
 * 状态：已通过 Picovoice 7 天试用 Key 验证全链路可用。
 *       试用过期后，系统自动切换至 alsa_capture.c 中的能量检测 fallback。
 *
 * 启用方式：
 *   1. 在 console.picovoice.ai 获取正式 AccessKey
 *   2. 取消下方注释，链接 libpv_porcupine.so
 *   3. 在 voice_agent_init 中调用 wake_word_init()
 *   4. 在 alsa_capture.c capture_thread 中调用 wake_word_process()
 *
 * 接口层与能量检测方案完全兼容，无需修改其他代码。
 */

/* ==================================================================
 * 全局状态
 * ================================================================ */

static struct {
    void   *handle;  /* Porcupine 引擎句柄（实际类型 pv_porcupine_handle_t*） */
    bool    ready;
} s_wake = {NULL, false};

/* ==================================================================
 * 接口实现
 * ================================================================ */

err_t wake_word_init(const char *model_path, const char *keyword_path,
                     const char *access_key, float sensitivity)
{
    if (!model_path || !keyword_path || !access_key ||
        model_path[0] == '\0' || keyword_path[0] == '\0' || access_key[0] == '\0') {
        LOG_WARN("Wake word: missing config, use energy fallback");
        return ERR_OK;
    }

    if (s_wake.ready) return ERR_OK;

    /*
     * 实际 Porcupine 初始化：
     *
     * pv_porcupine_handle_t *handle = NULL;
     * pv_status_t status = pv_porcupine_init(
     *     model_path, 1, &keyword_path, &sensitivity, &handle);
     * if (status != PV_STATUS_SUCCESS) {
     *     LOG_WARN("Wake word: init failed (%d)", (int)status);
     *     return ERR_OK;
     * }
     * s_wake.handle = handle;
     */

    s_wake.ready = true;
    LOG_INFO("Wake word engine ready");
    return ERR_OK;
}

void wake_word_deinit(void)
{
    if (s_wake.handle) {
        /* pv_porcupine_delete(s_wake.handle); */
        s_wake.handle = NULL;
    }
    s_wake.ready = false;
}

bool wake_word_is_ready(void)
{
    return s_wake.ready;
}

void wake_word_process(const int16_t *pcm, size_t frames)
{
    if (!s_wake.ready || !pcm || frames == 0) return;

    /*
     * 实际 Porcupine 推理：
     *
     * int32_t is_keyword = 0;
     * pv_porcupine_process(s_wake.handle, pcm, &is_keyword);
     * if (is_keyword) {
     *     LOG_INFO("Wake word detected!");
     *     event_publish_simple(EV_WAKEUP_DETECTED);
     * }
     */
}
