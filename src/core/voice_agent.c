#include "voice_agent.h"
#include "ai_client.h"
#include "alsa_capture.h"
#include "audio_pipeline.h"
#include "event_bus.h"
#include "logger.h"
#include "thread_pool.h"
#include "module.h"
#include "state_machine.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define RETRY_MAX      3
#define RETRY_DELAY_MS 200

static struct {
    const event_t     *current_ev;
    int64_t            state_entry_time;
    pthread_t          timeout_thread;
} s_va = {0};

static void on_event(const event_t *ev, void *user_data);

/* 线程池任务：AI 请求（同步 API，避免 DeepSeek v4-flash 流式 reasoning_content 干扰） */
static void ai_request_task(void *arg)
{
    char *text = (char *)arg;
    if (!text) return;

    ai_conv_append("user", text);

    char *payload = ai_conv_build_payload();
    if (!payload) {
        LOG_ERROR("VA: failed to build payload");
        event_publish_simple(EV_AI_ERROR);
        free(text);
        return;
    }

    char *response = NULL;
    err_t err = ai_client_chat_sync(payload, &response);
    free(payload);

    if (IS_OK(err) && response && response[0]) {
        event_publish_string(EV_AI_RESP_READY, response);
    } else {
        LOG_ERROR("VA: AI chat request failed");
        event_publish_simple(EV_AI_ERROR);
    }
    free(response);
    free(text);
}

/* TTS 合成 + 播放 */
static void tts_task(void *arg)
{
    char *text = (char *)arg;
    if (!text) return;

    ai_conv_append("assistant", text);

    char *path = NULL;
    err_t err = ai_tts_speak(text, &path);
    free(text);

    if (IS_OK(err) && path) {
        LOG_INFO("VA: TTS OK, starting playback: %s", path);
        err = audio_pipeline_play_tts(path);
        if (IS_ERR(err))
            LOG_WARN("VA: audio_pipeline_play_tts failed");
        free(path);
    } else {
        LOG_WARN("VA: TTS generation failed");
        event_publish_simple(EV_AUDIO_ERROR);
    }
}

/* ============= 状态机动作函数 ============= */

static void do_ai_request(int from, int to, int event, void *data)
{
    (void)from; (void)to; (void)event; (void)data;
    alsa_capture_set_wake_muted(true);  /* 处理期间静音唤醒检测 */
    if (!s_va.current_ev || !s_va.current_ev->data) {
        LOG_ERROR("VA: do_ai_request with no data");
        event_publish_simple(EV_AI_ERROR);
        return;
    }
    const char *text = (const char *)s_va.current_ev->data;
    if (text[0] == '\0') {
        event_publish_simple(EV_AI_ERROR);
        return;
    }
    char *copy = strdup(text);
    if (!copy) {
        event_publish_simple(EV_AI_ERROR);
        return;
    }
    task_id_t tid = thread_pool_submit(ai_request_task, copy, "ai_req");
    if (!tid) {
        LOG_ERROR("VA: thread_pool_submit failed");
        free(copy);
        event_publish_simple(EV_AI_ERROR);
    }
}

static void do_tts_and_play(int from, int to, int event, void *data)
{
    (void)from; (void)to; (void)event; (void)data;
    if (!s_va.current_ev || !s_va.current_ev->data) {
        LOG_ERROR("VA: do_tts_and_play with no data");
        event_publish_simple(EV_AUDIO_ERROR);
        return;
    }
    const char *text = (const char *)s_va.current_ev->data;
    if (text[0] == '\0') {
        event_publish_simple(EV_AUDIO_ERROR);
        return;
    }
    char *copy = strdup(text);
    if (!copy) {
        event_publish_simple(EV_AUDIO_ERROR);
        return;
    }
    task_id_t tid = thread_pool_submit(tts_task, copy, "tts");
    if (!tid) {
        LOG_ERROR("VA: thread_pool_submit failed");
        free(copy);
        event_publish_simple(EV_AUDIO_ERROR);
    }
}

/* 唤醒词触发：用默认文本发起 AI 请求 */
static void do_wake_response(int from, int to, int event, void *data)
{
    (void)from; (void)to; (void)event; (void)data;
    LOG_INFO("VA: wake word triggered, sending greeting");
    alsa_capture_set_wake_muted(true);  /* 处理期间静音唤醒检测 */
    char *copy = strdup("你好");
    if (!copy) return;
    task_id_t tid = thread_pool_submit(ai_request_task, copy, "wake");
    if (!tid) free(copy);
}

static void on_music_start(int from, int to, int event, void *data)
{
    (void)from; (void)to; (void)event; (void)data;
    LOG_INFO("VA: music started");
}

static void on_music_stop(int from, int to, int event, void *data)
{
    (void)from; (void)to; (void)event; (void)data;
    LOG_INFO("VA: music stopped");
}

static void on_play_done(int from, int to, int event, void *data)
{
    (void)from; (void)to; (void)event; (void)data;
    alsa_capture_set_wake_muted(false);  /* 播完恢复唤醒检测 */
    LOG_INFO("VA: playback done");
}

static void on_error(int from, int to, int event, void *data)
{
    (void)from; (void)to; (void)event; (void)data;
    alsa_capture_set_wake_muted(false);  /* 异常恢复唤醒检测 */
    LOG_WARN("VA: error, returning to IDLE (state %d)", from);
}

static void on_shutdown(int from, int to, int event, void *data)
{
    (void)from; (void)to; (void)event; (void)data;
    LOG_INFO("VA: shutdown requested");
}

/* ============= 状态表 ============= */

static const sm_transition_t s_transitions[] = {
    { ST_IDLE,       EV_BT_DATA_RECEIVED,    ST_PROCESSING, do_ai_request },
    { ST_IDLE,       EV_AUDIO_MUSIC_START,   ST_MUSIC,      on_music_start },
    { ST_MUSIC,      EV_AUDIO_MUSIC_STOP,    ST_IDLE,       on_music_stop },
    { ST_MUSIC,      EV_BT_DATA_RECEIVED,    ST_PROCESSING, do_ai_request },
    { ST_PROCESSING, EV_AI_RESP_READY,       ST_SPEAKING,   do_tts_and_play },
    { ST_PROCESSING, EV_AI_ERROR,            ST_IDLE,       on_error },
    { ST_SPEAKING,   EV_AUDIO_PLAY_DONE,     ST_IDLE,       on_play_done },
    { ST_SPEAKING,   EV_AUDIO_ERROR,         ST_IDLE,       on_error },
    { ST_IDLE,       EV_WAKEUP_DETECTED,     ST_PROCESSING, do_wake_response },
    { ST_IDLE,       EV_BT_DEVICE_DISCONN,   ST_IDLE,       NULL },
    { -1,            EV_SYS_SHUTDOWN,        ST_EXITING,    on_shutdown },
    { -1,            EV_SYS_ERROR,           ST_IDLE,       on_error },
};

/* ============= 事件回调 ============= */

static void on_event(const event_t *ev, void *user_data)
{
    (void)user_data;
    s_va.current_ev = ev;
    sm_dispatch(ev->id, ev->data);
    s_va.current_ev = NULL;
}

/* ============= 模块接口 ============= */

err_t voice_agent_init(void)
{
    /* ---- 从全局配置读取 ---- */
    const char *api_key   = config_get_str(g_config, "ai", "api_key", "");
    const char *model     = config_get_str(g_config, "ai", "model", "deepseek-chat");
    const char *device    = config_get_str(g_config, "audio", "device", "default");

    /* 1. AI 客户端 */
    ai_client_config_t ai_cfg = {
        .api_key    = api_key,
        .model      = model,
        .timeout_ms = 30000,
        .temperature = 0.7f,
        .max_tokens = 2048,
    };
    err_t err = ai_client_init(&ai_cfg);
    if (IS_ERR(err)) return err;

    /* 2. TTS */
    ai_tts_config_t tts_cfg = {
        .secret_id  = config_get_str(g_config, "tts", "secret_id", ""),
        .secret_key = config_get_str(g_config, "tts", "secret_key", ""),
        .voice_type = config_get_int(g_config, "tts", "voice_type", 0),
        .output_dir = config_get_str(g_config, "tts", "output_dir", "/tmp/speaker"),
    };
    err = ai_tts_init(&tts_cfg);
    if (IS_ERR(err)) { ai_client_deinit(); return err; }

    /* 3. 对话管理 */
    err = ai_conv_init("You are a helpful voice assistant.");
    if (IS_ERR(err)) { ai_tts_deinit(); ai_client_deinit(); return err; }

    /* 4. 音频管道（内部链式 init：mixer → player → a2dp → capture） */
    /* 播放用 default 设备（内建 dmix+plug，多句柄共享），录音用 plughw */
    const char *play_dev = config_get_str(g_config, "audio", "play_device",
                            "default:CARD=PRO");
    audio_pipeline_config_t pipe_cfg = {
        .playback_device = play_dev,
        .capture_device  = device,
        .capture_rate    = 16000,
    };
    err = audio_pipeline_init(&pipe_cfg);
    if (IS_ERR(err)) { ai_conv_deinit(); ai_tts_deinit(); ai_client_deinit(); return err; }

    /* 5. 启动录音（用于唤醒检测） */
    audio_pipeline_capture_start();

    /* ---- 状态机 ---- */
    sm_init(s_transitions, ARRAY_SIZE(s_transitions), ST_IDLE);

    /* 订阅事件 */
    event_subscribe(EV_BT_DATA_RECEIVED,  on_event, NULL);
    event_subscribe(EV_WAKEUP_DETECTED,   on_event, NULL);
    event_subscribe(EV_AI_RESP_READY,     on_event, NULL);
    event_subscribe(EV_AUDIO_PLAY_DONE,   on_event, NULL);
    event_subscribe(EV_AUDIO_MUSIC_START, on_event, NULL);
    event_subscribe(EV_AUDIO_MUSIC_STOP,  on_event, NULL);
    event_subscribe(EV_SYS_SHUTDOWN,      on_event, NULL);
    event_subscribe(EV_AI_ERROR,          on_event, NULL);
    event_subscribe(EV_AUDIO_ERROR,       on_event, NULL);
    event_subscribe(EV_BT_DEVICE_DISCONN, on_event, NULL);
    event_subscribe(EV_SYS_ERROR,         on_event, NULL);

    LOG_INFO("Voice agent initialized (model=%s)", model);
    return ERR_OK;
}

void voice_agent_shutdown(void)
{
    sm_dispatch(EV_SYS_SHUTDOWN, NULL);
}

void voice_agent_deinit(void)
{
    /* 按 init 逆序释放 */
    audio_pipeline_capture_stop();
    audio_pipeline_deinit();
    ai_conv_deinit();
    ai_tts_deinit();
    ai_client_deinit();
    sm_reset(ST_IDLE);
    LOG_INFO("VA: deinitialized");
}

static err_t va_init(void) { return voice_agent_init(); }
static void va_deinit(void) { voice_agent_deinit(); }
static void va_shutdown(void) { voice_agent_shutdown(); }

MODULE_DEFINE(va, va_init, va_deinit, va_shutdown, MODULE_PRIO_APPLICATION);
