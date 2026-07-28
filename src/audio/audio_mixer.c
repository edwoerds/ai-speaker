#include "audio_mixer.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>

/*
 * 软件混音器实现
 *
 * 混音算法：
 *   out = clamp( (src1 * gain1 + src2 * gain2) * master_gain, -32768, 32767 )
 *
 * Ducking 机制：
 *   mixer_duck_start("music", 0.3f)  → 保存当前 gain → 设为 0.3
 *   mixer_duck_restore("music")       → 恢复之前保存的 gain
 *   可嵌套调用（同一流多次 duck_start 会覆盖 saved_gain）
 */

/* ======================== 全局状态 ======================== */
static struct {
    mixer_stream_t streams[MIXER_MAX_STREAMS];// 8 路流
    int            stream_count;        // 当前已注册几路
    float          master_gain;        /* 主音量 0.0 ~ 1.0 */
    bool           inited;

    /* ducking 保存区：每个流保存一个 restore gain */
    float          duck_saved[MIXER_MAX_STREAMS];// 保存被 duck 之前的 gain
    bool           duck_active[MIXER_MAX_STREAMS];// 当前是否处于 duck 状态
} s_mix = {0};

//内部:找流 ID

static int stream_id_by_name(const char *name)
{
    for (int i = 0; i < s_mix.stream_count; i++) {
        if (strcmp(s_mix.streams[i].name, name) == 0)
            return i;
    }
    return -1;
}

//接口实现
err_t mixer_init(void)
{
    if (s_mix.inited) return ERR_OK;

    memset(&s_mix, 0, sizeof(s_mix));
    s_mix.master_gain = 1.0f;
    s_mix.inited = true;

    LOG_INFO("Mixer initialized (%d streams max)", MIXER_MAX_STREAMS);
    return ERR_OK;
}

void mixer_deinit(void)
{
    s_mix.inited = false;
    s_mix.stream_count = 0;
    LOG_INFO("Mixer deinitialized");
}

int mixer_stream_register(const char *name, float initial_gain)
{
    if (!s_mix.inited) return -1;
    if (s_mix.stream_count >= MIXER_MAX_STREAMS) {
        LOG_ERROR("Mixer stream limit reached (%d)", MIXER_MAX_STREAMS);
        return -1;
    }
    /* 不允许重名 */
    if (stream_id_by_name(name) >= 0) {
        LOG_WARN("Mixer stream '%s' already registered", name);
        return stream_id_by_name(name);
    }

    int id = s_mix.stream_count++;
    strncpy(s_mix.streams[id].name, name, sizeof(s_mix.streams[id].name) - 1);//strncpy + 手动 -1 — 防缓冲区溢出，但注意 strncpy 在源串过长时不加 \0，所以头文件的 name[32] 保证了有空间
    s_mix.streams[id].gain    = CLAMP(initial_gain, 0.0f, 1.0f);
    s_mix.streams[id].active  = false;

    LOG_DEBUG("Mixer stream registered: id=%d name=%s gain=%.2f",
              id, name, initial_gain);
    return id;
}

void mixer_stream_unregister(int stream_id)
{
    if (!s_mix.inited) return;
    if (stream_id < 0 || stream_id >= s_mix.stream_count) return;

    /* 用最后一个覆盖当前位置 */
    if (stream_id < s_mix.stream_count - 1) {
        s_mix.streams[stream_id] = s_mix.streams[s_mix.stream_count - 1];
        s_mix.duck_saved[stream_id]  = s_mix.duck_saved[s_mix.stream_count - 1];
        s_mix.duck_active[stream_id] = s_mix.duck_active[s_mix.stream_count - 1];
    }
    s_mix.stream_count--;
    LOG_DEBUG("Mixer stream unregistered: id=%d", stream_id);
}

int mixer_stream_find(const char *name)
{
    return stream_id_by_name(name);
}

err_t mixer_stream_set_gain(int stream_id, float gain)
{
    if (!s_mix.inited) return ERR_GENERAL;
    if (stream_id < 0 || stream_id >= s_mix.stream_count) return ERR_INVAL;

    s_mix.streams[stream_id].gain = CLAMP(gain, 0.0f, 1.0f);
    return ERR_OK;
}

float mixer_stream_get_gain(int stream_id)
{
    if (stream_id < 0 || stream_id >= s_mix.stream_count) return 0.0f;
    return s_mix.streams[stream_id].gain;
}

void mixer_set_master_gain(float gain)
{
    s_mix.master_gain = CLAMP(gain, 0.0f, 1.0f);
}

float mixer_get_master_gain(void)
{
    return s_mix.master_gain;
}

//Ducking 接口实现
err_t mixer_duck_start(const char *stream_name, float duck_gain)
{
    int id = stream_id_by_name(stream_name);
    if (id < 0) {
        LOG_WARN("Duck: stream '%s' not found", stream_name);
        return ERR_NOT_FOUND;
    }

    /* 保存当前 gain（如果还没被 duck） */
    if (!s_mix.duck_active[id]) {
        s_mix.duck_saved[id] = s_mix.streams[id].gain;
        s_mix.duck_active[id] = true;
    }

    s_mix.streams[id].gain = CLAMP(duck_gain, 0.0f, 1.0f);
    LOG_DEBUG("Duck: %s gain=%.2f (saved=%.2f)",
              stream_name, duck_gain, s_mix.duck_saved[id]);
    return ERR_OK;
}

void mixer_duck_restore(const char *stream_name)
{
    int id = stream_id_by_name(stream_name);
    if (id < 0) return;
    if (!s_mix.duck_active[id]) return;

    s_mix.streams[id].gain = s_mix.duck_saved[id];
    s_mix.duck_active[id] = false;
    LOG_DEBUG("Duck restore: %s gain=%.2f",
              stream_name, s_mix.streams[id].gain);
}
//PCM 混音核心
void mixer_mix_2ch(const int16_t *src1, float gain1,
                   const int16_t *src2, float gain2,
                   int16_t *dst, size_t frames, int channels)
{
    if (!src1 || !dst || frames == 0) return;

    float master = s_mix.master_gain;
    size_t samples = frames * (size_t)channels;

    if (src2 && gain2 > 0.0f) {
        /* 双路混音 */
        for (size_t i = 0; i < samples; i++) {
            float sum = (float)src1[i] * gain1
                      + (float)src2[i] * gain2;
            sum *= master;

            /* clamp 到 int16 范围 */
            if (sum > 32767.0f)      sum = 32767.0f;
            else if (sum < -32768.0f) sum = -32768.0f;

            dst[i] = (int16_t)sum;
        }
    } else {
        /* 单路直通（带 gain） */
        if (gain1 != 1.0f || master != 1.0f) {
            for (size_t i = 0; i < samples; i++) {
                float v = (float)src1[i] * gain1 * master;
                if (v > 32767.0f)      v = 32767.0f;
                else if (v < -32768.0f) v = -32768.0f;
                dst[i] = (int16_t)v;
            }
        } else if (dst != src1) {
            /* gain=1.0, master=1.0, 且 dst != src1 → 直接复制 */
            memcpy(dst, src1, samples * sizeof(int16_t));
        }
        /* else: gain=1.0, master=1.0, dst==src1 → 什么都不做 */
    }
}