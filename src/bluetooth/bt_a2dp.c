#include "bt_a2dp.h"
#include "audio_player.h"
#include "event_bus.h"
#include "logger.h"
#include "module.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * A2DP Sink — bluez-alsa + 环形缓冲（互斥锁保护）
 *
 * 架构:
 *   phone → [BlueZ] → [bluez-alsa] → bluealsa PCM (capture)
 *                                        ↓
 *                               pcm_reader → ringbuf(mutex)
 *                                              ↓
 *                                     playback_thread → audio_player → USB声卡
 *
 * 环形缓冲解耦读写线程，audio_player 的 ALSA 写入阻塞不会反压到 bluealsa。
 * 互斥锁保护环形缓冲，解决 ARM 多核 volatile 撕裂问题。
 */

#define DUCKING_FACTOR 0.3f

#define A2DP_CHANNELS      2
#define A2DP_BITS          16
#define A2DP_FRAME_BYTES   (A2DP_CHANNELS * A2DP_BITS / 8)
#define A2DP_SAMPLE_RATE   44100

#define RINGBUF_FRAMES      16384   /* ~370ms@44100Hz */
#define IDLE_SILENCE_FRAMES 4096
#define PCM_READ_FRAMES     1024

/* ==================================================================
 * 环形缓冲（互斥锁保护，SPSC）
 * ================================================================ */

typedef struct {
    int16_t        *buf;
    size_t          capacity;
    size_t          read_pos, write_pos, count;
    pthread_mutex_t lock;
} pcm_ringbuf_t;

static void ringbuf_init(pcm_ringbuf_t *rb, size_t frames)
{
    rb->buf = calloc(frames, A2DP_FRAME_BYTES);
    rb->capacity = frames;
    rb->read_pos = rb->write_pos = rb->count = 0;
    pthread_mutex_init(&rb->lock, NULL);
}

static void ringbuf_destroy(pcm_ringbuf_t *rb)
{
    free(rb->buf);
    rb->buf = NULL;
    rb->capacity = 0;
    pthread_mutex_destroy(&rb->lock);
}

static void ringbuf_put(pcm_ringbuf_t *rb, const int16_t *frames, size_t n)
{
    pthread_mutex_lock(&rb->lock);
    for (size_t i = 0; i < n; i++) {
        rb->buf[rb->write_pos * A2DP_CHANNELS + 0] = frames[i * A2DP_CHANNELS + 0];
        rb->buf[rb->write_pos * A2DP_CHANNELS + 1] = frames[i * A2DP_CHANNELS + 1];
        rb->write_pos = (rb->write_pos + 1) % rb->capacity;
        if (rb->count < rb->capacity) rb->count++;
        else rb->read_pos = (rb->read_pos + 1) % rb->capacity;
    }
    pthread_mutex_unlock(&rb->lock);
}

static size_t ringbuf_get(pcm_ringbuf_t *rb, int16_t *frames, size_t n)
{
    size_t read = 0;
    pthread_mutex_lock(&rb->lock);
    while (read < n && rb->count > 0) {
        frames[read * A2DP_CHANNELS + 0] = rb->buf[rb->read_pos * A2DP_CHANNELS + 0];
        frames[read * A2DP_CHANNELS + 1] = rb->buf[rb->read_pos * A2DP_CHANNELS + 1];
        rb->read_pos = (rb->read_pos + 1) % rb->capacity;
        rb->count--;
        read++;
    }
    pthread_mutex_unlock(&rb->lock);
    return read;
}

/* ==================================================================
 * 全局状态
 * ================================================================ */

static struct {
    bt_a2dp_config_t cfg;
    pcm_ringbuf_t    ringbuf;

    pthread_t       play_thread, reader_thread;
    volatile bool   running, ducking, streaming, paused;
} s_a2dp = {0};

/* ==================================================================
 * Ducking
 * ================================================================ */

static inline void apply_ducking(int16_t *frames, size_t n)
{
    if (!s_a2dp.ducking) return;
    for (size_t i = 0; i < n * A2DP_CHANNELS; i++)
        frames[i] = (int16_t)(frames[i] * DUCKING_FACTOR);
}

/* ==================================================================
 * 播放线程（ringbuf → audio_player）
 * ================================================================ */

static void *playback_thread(void *arg)
{
    (void)arg;
    size_t nf = RINGBUF_FRAMES;
    int16_t *buf = malloc(nf * A2DP_FRAME_BYTES);
    int16_t *silence = calloc(IDLE_SILENCE_FRAMES, A2DP_FRAME_BYTES);
    if (!buf || !silence) { free(buf); free(silence); return NULL; }

    LOG_INFO("A2DP: playback thread started");
    while (s_a2dp.running) {
        size_t n = ringbuf_get(&s_a2dp.ringbuf, buf, nf);
        if (n > 0) {
            apply_ducking(buf, n);
            if (IS_ERR(audio_player_stream_write(buf, n))) usleep(1000);
        } else if (s_a2dp.streaming) {
            if (IS_ERR(audio_player_stream_write(silence, IDLE_SILENCE_FRAMES)))
                usleep(1000);
        } else {
            usleep(10000);
        }
    }
    free(silence); free(buf);
    LOG_INFO("A2DP: playback thread exited");
    return NULL;
}

/* ==================================================================
 * bluealsa PCM reader 线程
 * ================================================================ */

static int find_bluealsa_device(char *dev, size_t dev_sz)
{
    FILE *fp = popen(
        "bluealsa-aplay -L 2>/dev/null | head -4 | grep -E '^bluealsa:' | head -1",
        "r");
    if (!fp) return 0;

    int found = 0;
    if (fgets(dev, (int)dev_sz, fp)) {
        size_t len = strlen(dev);
        while (len > 0 && (dev[len-1] == '\n' || dev[len-1] == '\r'))
            dev[--len] = '\0';
        found = (len > 0);
    }
    pclose(fp);
    return found;
}

static void *pcm_reader_thread(void *arg)
{
    (void)arg;
    int16_t *buf = malloc(PCM_READ_FRAMES * A2DP_FRAME_BYTES);
    if (!buf) return NULL;

    LOG_INFO("A2DP: reader thread started");

    while (s_a2dp.running) {
        char pcm_dev[256] = {0};

        if (!find_bluealsa_device(pcm_dev, sizeof(pcm_dev))) {
            if (s_a2dp.streaming) {
                s_a2dp.streaming = false;
                audio_player_stream_stop();
                event_publish_simple(EV_AUDIO_MUSIC_STOP);
                LOG_INFO("A2DP: device disconnected");
            }
            sleep(2);
            continue;
        }

        if (s_a2dp.streaming || s_a2dp.paused) { sleep(2); continue; }

        snd_pcm_t *pcm = NULL;
        int r = snd_pcm_open(&pcm, pcm_dev, SND_PCM_STREAM_CAPTURE, 0);
        if (r < 0) {
            LOG_WARN("A2DP: open '%s' failed: %s", pcm_dev, snd_strerror(r));
            sleep(2);
            continue;
        }

        r = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                               SND_PCM_ACCESS_RW_INTERLEAVED,
                               A2DP_CHANNELS, A2DP_SAMPLE_RATE,
                               1, 50000);
        if (r < 0) {
            LOG_WARN("A2DP: set_params failed: %s", snd_strerror(r));
            snd_pcm_close(pcm);
            sleep(2);
            continue;
        }

        s_a2dp.streaming = true;
        audio_player_stream_start(A2DP_SAMPLE_RATE, A2DP_CHANNELS);
        event_publish_simple(EV_AUDIO_MUSIC_START);
        LOG_INFO("A2DP: streaming from '%s'", pcm_dev);

        while (s_a2dp.running) {
            /* TTS 播放期间暂停读取，释放声卡 */
            if (s_a2dp.paused) {
                usleep(50000);
                continue;
            }
            snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, PCM_READ_FRAMES);
            if (n < 0) {
                if (n == -EPIPE) {
                    if (snd_pcm_recover(pcm, (int)n, 1) < 0) break;
                    continue;
                }
                break;
            }
            if (n > 0)
                ringbuf_put(&s_a2dp.ringbuf, buf, (size_t)n);
        }

        snd_pcm_close(pcm);
        s_a2dp.streaming = false;
        audio_player_stream_stop();
        event_publish_simple(EV_AUDIO_MUSIC_STOP);
        LOG_INFO("A2DP: stream ended");
        sleep(1);
    }

    free(buf);
    LOG_INFO("A2DP: reader thread exited");
    return NULL;
}

/* ==================================================================
 * 公开 API
 * ================================================================ */

err_t bt_a2dp_init(const bt_a2dp_config_t *cfg)
{
    if (s_a2dp.running) return ERR_OK;
    if (cfg) s_a2dp.cfg = *cfg;

    ringbuf_init(&s_a2dp.ringbuf, RINGBUF_FRAMES);
    if (!s_a2dp.ringbuf.buf) return ERR_NOMEM;

    s_a2dp.running = true;
    pthread_create(&s_a2dp.play_thread, NULL, playback_thread, NULL);
    pthread_create(&s_a2dp.reader_thread, NULL, pcm_reader_thread, NULL);

    LOG_INFO("A2DP initialized via bluez-alsa (ringbuf=%d)", RINGBUF_FRAMES);
    return ERR_OK;
}

void bt_a2dp_set_ducking(bool duck)
{ s_a2dp.ducking = duck; }

void bt_a2dp_set_pause(bool pause)
{ s_a2dp.paused = pause; }

void bt_a2dp_shutdown(void)
{
    s_a2dp.running = false;
    s_a2dp.streaming = false;
    audio_player_stream_stop();
    LOG_INFO("A2DP: shutdown");
}

void bt_a2dp_deinit(void)
{
    bt_a2dp_shutdown();

    if (s_a2dp.play_thread) {
        pthread_join(s_a2dp.play_thread, NULL);
        s_a2dp.play_thread = 0;
    }
    if (s_a2dp.reader_thread) {
        pthread_join(s_a2dp.reader_thread, NULL);
        s_a2dp.reader_thread = 0;
    }

    ringbuf_destroy(&s_a2dp.ringbuf);
    s_a2dp.ducking = false;
    s_a2dp.streaming = false;
    LOG_INFO("A2DP deinitialized");
}
