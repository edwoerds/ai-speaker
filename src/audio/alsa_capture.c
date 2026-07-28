#include "alsa_capture.h"
#include "logger.h"
#include "event_bus.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * ALSA 录音模块实现
 *
 * 工作方式：
 *   capture 线程循环：snd_pcm_readi → publish(EV_AUDIO_CAPTURE_DATA)
 *   每 period（默认 512 帧 ≈ 32ms@16kHz）发一次事件
 *   事件数据由 event_bus 的 data_destroy 回调释放
 */

/* ======================== 常量 ======================== */
#define CAPTURE_PERIOD_FRAMES  512   /* 每次读取 512 帧 ≈32ms */

/* ======================== 全局状态 ======================== */
static struct {
    alsa_capture_config_t cfg;       /* 配置 */
    snd_pcm_t            *pcm;       /* ALSA capture PCM 句柄 */
    pthread_t             thread;    /* capture 线程 */
    volatile bool         running;   /* capture 运行中 */
    volatile bool         inited;    /* 模块已初始化 */
    bool                  thread_created; /* 线程已创建 */
    volatile bool         wake_muted;     /* 唤醒检测静音（TTS 播报时置 true） */
    volatile int          wake_holdoff;   /* unmute 后强制冷却帧数 */
} s_cap = {0};

/* ======================== PCM 数据释放回调 ======================== */
static void capture_data_destroy(void *data)
{
    if (data) {
        /* capture_data_t 是和 PCM 数据一起 malloc 的，见 capture_thread */
        free(data);
    }
}

///内部: ALSA capture 初始化
static err_t alsa_capture_open(void){
    int rc;
    const char *dev=s_cap.cfg.device ? s_cap.cfg.device : "default";
    unsigned int rate=s_cap.cfg.rate?s_cap.cfg.rate: 16000;
    int ch=(s_cap.cfg.channels>0)?s_cap.cfg.channels: 1;

    //打开 capture PCM
    rc= snd_pcm_open(&s_cap.pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
       LOG_ERROR("Capture open failed (%s): %s", dev, snd_strerror(rc));
        return ERR_NODEV;
    }

    //设置硬件参数（用 set_params 简化，和 arecord 行为一致）
    unsigned int actual_rate = rate ? rate : 16000;
    rc = snd_pcm_set_params(s_cap.pcm, SND_PCM_FORMAT_S16_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            ch, actual_rate, 1, 500000);
    if (rc < 0) {
        LOG_ERROR("Capture set_params failed: %s", snd_strerror(rc));
        snd_pcm_close(s_cap.pcm);
        s_cap.pcm = NULL;
        return ERR_NODEV;
    }
    LOG_INFO("Capture opened: dev=%s, rate=%u, ch=%d",
             dev, actual_rate, ch);
    return ERR_OK;
}

//内部: ALSA capture 关闭
  static void alsa_capture_close(void)
  {
      if (s_cap.pcm) {
          snd_pcm_drain(s_cap.pcm);  // 等所有数据读完
          snd_pcm_close(s_cap.pcm);  // 关闭设备
          s_cap.pcm = NULL;          // 防悬空指针
      }
  }

//capture线程
static void *capture_thread(void *arg){
    (void)arg;
    //分配 PCM 读取缓冲
    size_t buf_frames=CAPTURE_PERIOD_FRAMES;
    size_t buf_bytes=buf_frames*sizeof(int16_t);
    int16_t *buf=(int16_t*)malloc(buf_bytes);
    if(!buf){
        LOG_ERROR("Capture OOM");
        s_cap.running = false;
        return NULL;
    }
    LOG_INFO("Capture thread started");

    /* 能量检测唤醒：冷却计数器（触发后抑制约3秒） */
    int wake_cooldown = 0;
#define WAKE_ENERGY_THRESHOLD 3000
#define WAKE_COOLDOWN_MAX    100

    /* 确保 PCM 状态正确再启动流 */
    snd_pcm_prepare(s_cap.pcm);
    int start_rc = snd_pcm_start(s_cap.pcm);
    if (start_rc < 0) {
        LOG_ERROR("Capture start failed: %s", snd_strerror(start_rc));
    } else {
        LOG_INFO("Capture stream started");
    }

    while(s_cap.running){
        //从 ALSA 读取 PCM 数据
        int rc=snd_pcm_readi(s_cap.pcm, buf, buf_frames);
        if(rc==-EPIPE){
            //XRUN — 恢复
            LOG_WARN("Capture xrun, recovering...");
            snd_pcm_prepare(s_cap.pcm);
            snd_pcm_start(s_cap.pcm);
            continue;
        }
        if(rc<0){
            if (rc == -EIO) {
                LOG_WARN("Capture EIO, recover...");
                snd_pcm_prepare(s_cap.pcm);
                snd_pcm_start(s_cap.pcm);
                continue;
            }
            LOG_ERROR("Capture read error: %s", snd_strerror(rc));
            break;
        }

        size_t frames_read=(size_t)rc;

        LOG_DEBUG("Capture read %zu frames", frames_read);

        /* 能量检测唤醒 */
        if (s_cap.wake_holdoff > 0) {
            s_cap.wake_holdoff--;
        } else if (wake_cooldown > 0) {
            wake_cooldown--;
        } else if (s_cap.wake_muted) {
            /* TTS/A2DP 播放中，跳过 */
        } else {
            long long sum = 0;
            for (size_t i = 0; i < frames_read; i++)
                sum += abs(buf[i]);
            int avg = (int)(sum / (long long)frames_read);
            if (avg > WAKE_ENERGY_THRESHOLD) {
                LOG_INFO("Wake: energy trigger (%d), publishing event", avg);
                s_cap.wake_muted = true;
                event_publish_simple(EV_WAKEUP_DETECTED);
                wake_cooldown = WAKE_COOLDOWN_MAX;
            }
        }

        //构造事件数据：capture_data_t + PCM 数据一起 malloc
        size_t data_size=sizeof(capture_data_t)+frames_read*sizeof(int16_t);
        capture_data_t *evdata=(capture_data_t*)malloc(data_size);
        if(!evdata){
            LOG_ERROR("Capture alloc evdata OOM");
            continue;
        }
        evdata->frames = frames_read;
        evdata->rate   = s_cap.cfg.rate ? s_cap.cfg.rate : 16000;
        //PCM 数据紧跟在结构体后面
        memcpy((uint8_t *)evdata+sizeof(capture_data_t), buf,frames_read * sizeof(int16_t));
        evdata->data=(uint8_t *)evdata+sizeof(capture_data_t);

        //发布事件
        event_publish(EV_AUDIO_CAPTURE_DATA, evdata,data_size, capture_data_destroy);
    }
    free(buf);
    LOG_INFO("Capture thread stopped");
    return NULL;
}

//接口实现
err_t alsa_capture_init(const alsa_capture_config_t *cfg)
{
    if (s_cap.inited) return ERR_OK;

    if (cfg) {
        s_cap.cfg = *cfg;
    }
    if (!s_cap.cfg.device) s_cap.cfg.device = "default";
    if (s_cap.cfg.rate == 0) s_cap.cfg.rate = 16000;
    if (s_cap.cfg.channels <= 0) s_cap.cfg.channels = 1;

    /* 注意：这里不再打开 ALSA 设备，只在 start 时才打开，避免占用声卡 */
    s_cap.inited = true;
    LOG_INFO("Capture module initialized (dev=%s, %uHz, %dch)",
             s_cap.cfg.device, s_cap.cfg.rate, s_cap.cfg.channels);
    return ERR_OK;
}

void alsa_capture_deinit(void)
{
    if (!s_cap.inited) return;

    alsa_capture_stop();
    alsa_capture_close();
    s_cap.inited = false;
    LOG_INFO("Capture module deinitialized");
}

err_t alsa_capture_start(void)
{
    if (!s_cap.inited) return ERR_GENERAL;
    if (s_cap.running) return ERR_OK;

    /* 打开 ALSA capture 设备（延迟打开，避免与 A2DP 冲突） */
    err_t err = alsa_capture_open();
    if (IS_ERR(err)) return err;

    /* 重置 PCM 状态，让 snd_pcm_readi 自动启动流 */
    snd_pcm_drop(s_cap.pcm);
    int rc = snd_pcm_prepare(s_cap.pcm);
    if (rc < 0) {
        LOG_ERROR("Capture prepare failed: %s", snd_strerror(rc));
        alsa_capture_close();
        return ERR_NODEV;
    }

    s_cap.running = true;

    rc = pthread_create(&s_cap.thread, NULL, capture_thread, NULL);
    if (rc != 0) {
        s_cap.running = false;
        alsa_capture_close();
        LOG_ERROR("Capture thread create failed: %d", rc);
        return ERR_GENERAL;
    }
    s_cap.thread_created = true;

    event_publish_simple(EV_AUDIO_CAPTURE_START);
    LOG_INFO("Capture started");
    return ERR_OK;
}

void alsa_capture_stop(void)
{
    if (!s_cap.running) return;

    s_cap.running = false;

    /* 关闭 PCM 设备踢醒阻塞的 snd_pcm_readi */
    if (s_cap.pcm) {
        snd_pcm_drop(s_cap.pcm);
    }

    if (s_cap.thread_created) {
        pthread_join(s_cap.thread, NULL);
        s_cap.thread_created = false;
    }

    alsa_capture_close();
    event_publish_simple(EV_AUDIO_CAPTURE_STOP);
    LOG_INFO("Capture stopped");
}

bool alsa_capture_is_running(void)
{
    return s_cap.running;
}

void alsa_capture_set_wake_muted(bool muted)
{
    s_cap.wake_muted = muted;
    if (!muted) {
        s_cap.wake_holdoff = 150;  /* unmute 后约 4.8 秒内不检测 */
    }
}