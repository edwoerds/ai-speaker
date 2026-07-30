#include "voice_agent.h"
#include "ai_client.h"
#include "ai_stt.h"          /* v2.0: STT 语音识别 */
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
#include <time.h>            /* v2.0: clock_gettime */

#define RETRY_MAX      3
#define RETRY_DELAY_MS 200

static struct {
    const event_t     *current_ev;
    int64_t            state_entry_time;
    pthread_t          timeout_thread;
} s_va = {0};

/* ==================================================================
 * 录音缓冲区（STT：唤醒后累计 PCM 音频）
 * 16kHz × 16bit × mono，最大 2.5 秒 = 40000 帧
 * ================================================================ */

#define LISTEN_DURATION_MS  2500
#define LISTEN_MAX_FRAMES   48000

static int16_t  *s_listen_buf    = NULL;
static size_t    s_listen_frames = 0;
static int64_t   s_listen_start  = 0;
static bool      s_listen_active = false;

static void listen_buf_reset(void)
{
    free(s_listen_buf);
    s_listen_buf    = NULL;
    s_listen_frames = 0;
    s_listen_active = false;
}

static void listen_buf_append(const int16_t *pcm, size_t frames)
{
    if (!s_listen_active || !pcm || frames == 0) return;
    size_t new_frames = s_listen_frames + frames;
    if (new_frames > LISTEN_MAX_FRAMES) {
        size_t room = LISTEN_MAX_FRAMES - s_listen_frames;
        if (room == 0) return;
        frames = room;
        new_frames = LISTEN_MAX_FRAMES;
    }
    int16_t *p = (int16_t *)realloc(s_listen_buf, new_frames * sizeof(int16_t));
    if (!p) { LOG_ERROR("VA: listen_buf OOM"); return; }
    memcpy(p + s_listen_frames, pcm, frames * sizeof(int16_t));
    s_listen_buf = p;
    s_listen_frames = new_frames;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

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

/* ==================================================================
 * 唤醒 → STT → AI 任务（v2.0 新增）
 *
 * 从录音缓冲区取 PCM → 百度 STT → 文本 → AI 对话 → TTS
 * 全部在 thread_pool 中异步执行，不阻塞事件循环
 * ================================================================ */

static void stt_and_ai_task(void *arg)
{
    (void)arg;

    /* 1. 把 s_listen_buf 搬到局部变量，释放全局缓冲区 */
    int16_t  *audio = s_listen_buf;
    size_t    frames = s_listen_frames;
    s_listen_buf = NULL;
    s_listen_frames = 0;
    s_listen_active = false;

    if (!audio || frames == 0) {
        LOG_WARN("VA: STT no audio captured");
        event_publish_simple(EV_AI_ERROR);
        return;
    }

    /* 2. STT：PCM → 文本 */
    char *text = NULL;
    err_t err = ai_stt_process(audio, frames, &text);
    free(audio);  /* 释放 PCM 缓冲区 */

    if (IS_ERR(err) || !text || text[0] == '\0') {
        LOG_WARN("VA: STT failed or silent, fallback greeting");
        /* 降级：发默认问候语 */
        char *fallback = strdup("你好");
        if (fallback) {
            ai_request_task(fallback);  /* 复用 AI 请求任务 */
        } else {
            event_publish_simple(EV_AI_ERROR);
        }
        free(text);
        return;
    }

    LOG_INFO("VA: STT recognized: \"%s\"", text);

    /* 3. 文本 → AI 对话 */
    ai_conv_append("user", text);
    free(text);

    char *payload = ai_conv_build_payload();
    if (!payload) {
        event_publish_simple(EV_AI_ERROR);
        return;
    }

    char *response = NULL;
    err = ai_client_chat_sync(payload, &response);
    free(payload);

    if (IS_OK(err) && response && response[0]) {
        event_publish_string(EV_AI_RESP_READY, response);
    } else {
        event_publish_simple(EV_AI_ERROR);
    }
    free(response);
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

/* 唤醒词触发：启动录音采集，累计 2.5 秒后做 STT → AI */
static void do_wake_response(int from, int to, int event, void *data)
{
    (void)from; (void)to; (void)event; (void)data;
    LOG_INFO("VA: wake detected, listening for %.1fs...",
             LISTEN_DURATION_MS / 1000.0);

    /* 重置录音缓冲区 */
    listen_buf_reset();
    s_listen_start  = now_ms();
    s_listen_active = true;

    /* TTS 播报期间听不到外部声音——播完恢复时会解锁唤醒 */
    alsa_capture_set_wake_muted(true);
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
    /* v2.0 STT: 唤醒 → 录音 → STT → AI 回复 */
    { ST_IDLE,       EV_WAKEUP_DETECTED,     ST_LISTENING,  do_wake_response },
    { ST_LISTENING,  EV_AI_RESP_READY,       ST_SPEAKING,   do_tts_and_play },
    { ST_LISTENING,  EV_AI_ERROR,            ST_IDLE,       on_error },
    { ST_IDLE,       EV_BT_DEVICE_DISCONN,   ST_IDLE,       NULL },
    { -1,            EV_SYS_SHUTDOWN,        ST_EXITING,    on_shutdown },
    { -1,            EV_SYS_ERROR,           ST_IDLE,       on_error },
};

/* ============= 事件回调 ============= */

static void on_event(const event_t *ev, void *user_data)
{
    (void)user_data;

    /* ---- v2.0：录音中 → 累计 PCM 音频 ---- */
    if (ev->id == EV_AUDIO_CAPTURE_DATA && s_listen_active) {
        capture_data_t *cap = (capture_data_t *)ev->data;
        if (cap && cap->data && cap->frames > 0) {
            listen_buf_append((const int16_t *)cap->data, cap->frames);

            /* 超时检查：录音累积够了，提交 STT */
            if (now_ms() - s_listen_start >= LISTEN_DURATION_MS) {
                s_listen_active = false;
                LOG_INFO("VA: listen timeout (%zu frames), STT start",
                         s_listen_frames);
                /* 提交 STT+AI 任务（thread_pool 中执行，不阻塞事件循环） */
                task_id_t tid = thread_pool_submit(stt_and_ai_task,
                                                     NULL, "stt");
                if (!tid) {
                    LOG_ERROR("VA: stt task submit failed");
                    event_publish_simple(EV_AI_ERROR);
                }
            }
        }
        return;  /* 录音数据不进入状态机 */
    }

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

    /* 3.5. STT 语音识别（v2.0，配置为空时静默降级） */
    {
        const char *stt_key = config_get_str(g_config, "stt", "api_key", "");
        const char *stt_secret = config_get_str(g_config, "stt", "secret_key", "");
        ai_stt_config_t stt_cfg = {
            .api_key    = stt_key,
            .secret_key = stt_secret,
            .timeout_ms = 5000,
        };
        ai_stt_init(&stt_cfg);
    }

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
    event_subscribe(EV_AUDIO_CAPTURE_DATA, on_event, NULL);  /* v2.0 STT */
    event_subscribe(EV_BT_DATA_RECEIVED,   on_event, NULL);
    event_subscribe(EV_WAKEUP_DETECTED,    on_event, NULL);
    event_subscribe(EV_AI_RESP_READY,      on_event, NULL);
    event_subscribe(EV_AUDIO_PLAY_DONE,    on_event, NULL);
    event_subscribe(EV_AUDIO_MUSIC_START,  on_event, NULL);
    event_subscribe(EV_AUDIO_MUSIC_STOP,   on_event, NULL);
    event_subscribe(EV_SYS_SHUTDOWN,       on_event, NULL);
    event_subscribe(EV_AI_ERROR,           on_event, NULL);
    event_subscribe(EV_AUDIO_ERROR,        on_event, NULL);
    event_subscribe(EV_BT_DEVICE_DISCONN,  on_event, NULL);
    event_subscribe(EV_SYS_ERROR,          on_event, NULL);

    LOG_INFO("Voice agent initialized (model=%s)", model);
    return ERR_OK;
}

void voice_agent_shutdown(void)
{
    sm_dispatch(EV_SYS_SHUTDOWN, NULL);
}

void voice_agent_deinit(void)
{
    /* 停止录音采集（v2.0 STT） */
    listen_buf_reset();
    ai_stt_deinit();

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
