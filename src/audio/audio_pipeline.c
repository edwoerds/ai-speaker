#include "audio_pipeline.h"
#include "audio_player.h"
#include "alsa_capture.h"
#include "audio_mixer.h"
#include "bt_a2dp.h"
#include "event_bus.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 音频管道实现
 *
 * 协调三个音频模块 + 自动 ducking 管理
 *
 * Ducking 流程：
 *   play_tts   → mixer_duck_start("music", 0.3f)
 *   TTS 播完   → EV_AUDIO_PLAY_DONE → mixer_duck_restore("music")
 *
 * 事件订阅：
 *   EV_AUDIO_PLAY_DONE — TTS 播完恢复音乐
 */

/* ======================== 常量 ======================== */
#define DUCK_GAIN   0.3f    /* ducking 时音乐降到 30% */

/* ======================== 全局状态 ======================== */
static struct {
    audio_pipeline_config_t cfg;
    bool inited;

    /* 状态追踪 */
    volatile bool music_playing;
    volatile bool tts_playing;

    /* 事件订阅 */
    subscription_t *sub_play_done;
    subscription_t *sub_audio_error;
} s_pipe = {0};

/* ======================== 事件回调 ======================== */
static void on_pipeline_event(const event_t *ev, void *user_data)
{
    (void)user_data;

    switch (ev->id) {
    case EV_AUDIO_PLAY_DONE:
        if (s_pipe.music_playing) {
            mixer_duck_restore("music");
            bt_a2dp_set_ducking(false);
            bt_a2dp_set_pause(false);
            audio_player_stream_start(44100, 2);
            LOG_INFO("Pipeline: TTS done, resumed A2DP stream");
        }
        s_pipe.tts_playing = false;
        break;

    case EV_AUDIO_ERROR:
        if (s_pipe.music_playing) {
            mixer_duck_restore("music");
            bt_a2dp_set_ducking(false);
            bt_a2dp_set_pause(false);
            audio_player_stream_start(44100, 2);
            LOG_INFO("Pipeline: TTS error, resumed A2DP stream");
        }
        s_pipe.tts_playing = false;
        break;

    default:
        break;
    }
}

/* ======================== 接口实现 ======================== */

err_t audio_pipeline_init(const audio_pipeline_config_t *cfg)
{
    if (s_pipe.inited) return ERR_OK;

    /* 保存配置 */
    if (cfg) {
        s_pipe.cfg = *cfg;
    }
    if (!s_pipe.cfg.playback_device) s_pipe.cfg.playback_device = "default";
    if (!s_pipe.cfg.capture_device)  s_pipe.cfg.capture_device  = "default";
    if (s_pipe.cfg.capture_rate == 0) s_pipe.cfg.capture_rate = 16000;
    if (!s_pipe.cfg.a2dp_device)    s_pipe.cfg.a2dp_device = s_pipe.cfg.playback_device;
    if (s_pipe.cfg.a2dp_ringbuf_frames <= 0) s_pipe.cfg.a2dp_ringbuf_frames = 256;

    /* 1. 初始化混音器 */
    err_t err = mixer_init();
    if (IS_ERR(err)) return err;

    /* 2. 注册混音流 */
    mixer_stream_register("music", 1.0f);
    mixer_stream_register("tts",   1.0f);

    /* 3. 初始化播放器 */
    audio_player_config_t ap_cfg = {
        .device = s_pipe.cfg.playback_device,
    };
    err = audio_player_init(&ap_cfg);
    if (IS_ERR(err)) {
        mixer_deinit();
        return err;
    }

    /* 4. 初始化 A2DP Sink（蓝牙音乐播放） */
    bt_a2dp_config_t a2dp_cfg;
    memset(&a2dp_cfg, 0, sizeof(a2dp_cfg));
    a2dp_cfg.device          = s_pipe.cfg.a2dp_device;
    a2dp_cfg.ringbuf_frames  = s_pipe.cfg.a2dp_ringbuf_frames;
    err = bt_a2dp_init(&a2dp_cfg);
    if (IS_ERR(err)) {
        LOG_WARN("A2DP init failed (D-Bus may be unavailable)");
    }

    /* 5. 初始化录音 */
    alsa_capture_config_t cap_cfg = {
        .device   = s_pipe.cfg.capture_device,
        .rate     = s_pipe.cfg.capture_rate,
        .channels = 1,
    };
    err = alsa_capture_init(&cap_cfg);
    if (IS_ERR(err)) {
        /* 录音初始化失败不算致命（可能没有 mic） */
        LOG_WARN("Capture init failed (mic may be absent): %s",
                 s_pipe.cfg.capture_device);
    }

    /* 6. 订阅事件 */
    s_pipe.sub_play_done = event_subscribe(EV_AUDIO_PLAY_DONE,
                                           on_pipeline_event, NULL);
    s_pipe.sub_audio_error = event_subscribe(EV_AUDIO_ERROR,
                                              on_pipeline_event, NULL);

    s_pipe.inited = true;
    LOG_INFO("Audio pipeline initialized (play=%s, cap=%s@%uHz)",
             s_pipe.cfg.playback_device,
             s_pipe.cfg.capture_device,
             s_pipe.cfg.capture_rate);
    return ERR_OK;
}

void audio_pipeline_deinit(void)
{
    if (!s_pipe.inited) return;

    /* 退订事件 */
    if (s_pipe.sub_play_done) {
        event_unsubscribe(s_pipe.sub_play_done);
        s_pipe.sub_play_done = NULL;
    }
    if (s_pipe.sub_audio_error) {
        event_unsubscribe(s_pipe.sub_audio_error);
        s_pipe.sub_audio_error = NULL;
    }

    /* 停止录音（如果还在跑） */
    alsa_capture_stop();
    alsa_capture_deinit();

    /* 停止 A2DP（bt_a2dp_deinit 内部已包含 shutdown） */
    bt_a2dp_deinit();

    /* 停止播放 */
    audio_player_stop();
    audio_player_deinit();

    /* 反初始化混音器 */
    mixer_deinit();

    s_pipe.inited = false;
    s_pipe.music_playing = false;
    s_pipe.tts_playing = false;
    LOG_INFO("Audio pipeline deinitialized");
}

/* ---------- TTS 播放 ---------- */

err_t audio_pipeline_play_tts(const char *filepath)
{
    if (!s_pipe.inited || !filepath) return ERR_INVAL;

    if (s_pipe.tts_playing) {
        audio_pipeline_stop_tts();
    }

    /* dmix 下同时播放不冲突，但为 TTS 清晰度，暂停 A2DP 流 + 压低音量 */
    if (s_pipe.music_playing) {
        bt_a2dp_set_pause(true);
        audio_player_stream_stop();
        bt_a2dp_set_ducking(true);
        LOG_INFO("Pipeline: TTS start, paused A2DP stream");
    }

    err_t err = audio_player_play_async(filepath);
    if (IS_ERR(err)) {
        LOG_ERROR("Pipeline: TTS play failed: %s", filepath);
        if (s_pipe.music_playing) {
            bt_a2dp_set_ducking(false);
            bt_a2dp_set_pause(false);
            LOG_INFO("Pipeline: TTS failed, resumed A2DP stream");
        }
        return err;
    }

    s_pipe.tts_playing = true;
    return ERR_OK;
}

void audio_pipeline_stop_tts(void)
{
    audio_player_stop();
    s_pipe.tts_playing = false;

    /* TTS 播完，恢复音乐流 */
    if (s_pipe.music_playing) {
        bt_a2dp_set_ducking(false);
        bt_a2dp_set_pause(false);
        audio_player_stream_start(44100, 2);
        LOG_INFO("Pipeline: TTS done, resumed A2DP stream");
    }
}

bool audio_pipeline_is_tts_playing(void)
{
    return s_pipe.tts_playing;
}

/* ---------- 音乐控制 ---------- */

void audio_pipeline_music_start(void)
{
    s_pipe.music_playing = true;
    LOG_DEBUG("Pipeline: music started");
}

void audio_pipeline_music_stop(void)
{
    s_pipe.music_playing = false;
    /* 如果有 ducking 未恢复，也清掉 */
    mixer_duck_restore("music");
    LOG_DEBUG("Pipeline: music stopped");
}

bool audio_pipeline_is_music_playing(void)
{
    return s_pipe.music_playing;
}

/* ---------- 录音控制 ---------- */

err_t audio_pipeline_capture_start(void)
{
    return alsa_capture_start();
}

void audio_pipeline_capture_stop(void)
{
    alsa_capture_stop();
}

bool audio_pipeline_is_capturing(void)
{
    return alsa_capture_is_running();
}