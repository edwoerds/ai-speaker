  #include "audio_player.h"    // 自己的头文件
  #include "event_bus.h"       // 播放完发事件
  #include "logger.h"          // 日志
  #include "module.h"          // 模块框架

  #include <alsa/asoundlib.h>  // ALSA API — 核心依赖
  #include <pthread.h>         // 异步播放线程
  #include <stdio.h>           // fopen/fread
  #include <stdlib.h>          // malloc/free
  #include <string.h>          // memcmp
  #include <unistd.h>          // 无实际使用，但习惯保留
  // ALSA = Advanced Linux Sound Architecture，Linux 上的标准音频框架。asoundlib.h 是它的用户态库，我们只用 PCM 播放功能。
  /*
 * ALSA 音频播放器
 *
 * 支持 WAV 文件播放，使用 ALSA PCM 接口输出
 *
 * WAV 文件格式（44 字节头部）：
 *   [0-3]   "RIFF"
 *   [4-7]   文件大小 - 8
 *   [8-11]  "WAVE"
 *   [12-15] "fmt "
 *   [16-19] 16 (PCM 格式块大小)
 *   [20-21] 1 (PCM)
 *   [22-23] 通道数
 *   [24-27] 采样率
 *   [28-31] 字节率
 *   [32-33] block align
 *   [34-35] bits per sample
 *   [36-39] "data"
 *   [40-43] data 大小
 *   [44+]   PCM 数据
 */


 // WAV 文件头部结构

 typedef struct __attribute__((packed)) {
    char     riff[4];          /* "RIFF" */
    uint32_t file_size;        /* 文件总大小 - 8 */
    char     wave[4];          /* "WAVE" */
    char     fmt[4];           /* "fmt " */
    uint32_t fmt_size;         /* fmt 块大小 (16 for PCM) */
    uint16_t audio_format;     /* 格式 (1 = PCM) */
    uint16_t num_channels;     /* 通道数 */
    uint32_t sample_rate;      /* 采样率 */
    uint32_t byte_rate;        /* 字节率 */
    uint16_t block_align;      /* block align */
    uint16_t bits_per_sample;  /* 位深 */
    char     data[4];          /* "data" */
    uint32_t data_size;        /* PCM 数据大小 */
} wav_header_t;

//全局状态
static struct{
      snd_pcm_t       *pcm_handle;    // ALSA PCM 设备句柄（WAV 模式用）
      audio_player_config_t cfg;      // 配置（设备名）
      pthread_t        play_thread;   // 异步播放线程ID
      volatile bool    playing;       // 是否正在播放
      volatile bool    stop;          // 是否请求停止
      volatile bool    inited;        // 是否已初始化

      /* 流式模式（A2DP 用） */
      snd_pcm_t       *stream_pcm;    // 流式 PCM 句柄
      bool             streaming;     // 流式模式激活中
} s_ap={0};

// ALSA PCM 初始化

static err_t alsa_open(const char *device,unsigned int rate,int channels){
    int rc;
    
    /* 打开 PCM 设备 */
    rc = snd_pcm_open(&s_ap.pcm_handle, device ? device : "default",
                      SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        LOG_ERROR("ALSA open failed (%s): %s", device ? device : "default",
                  snd_strerror(rc));
        return ERR_NODEV;
    }
    //设置硬件参数
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);//栈上分配 param 结构体
    snd_pcm_hw_params_any(s_ap.pcm_handle,params);//用默认值填充

    snd_pcm_hw_params_set_access(s_ap.pcm_handle, params,
                                   SND_PCM_ACCESS_RW_INTERLEAVED);
    
    snd_pcm_hw_params_set_format(s_ap.pcm_handle, params,
                                   SND_PCM_FORMAT_S16_LE);
     
    snd_pcm_hw_params_set_channels(s_ap.pcm_handle, params, channels);  
    unsigned int actual_rate = rate;
    snd_pcm_hw_params_set_rate_near(s_ap.pcm_handle, params, &actual_rate, 0);
    rc=snd_pcm_hw_params(s_ap.pcm_handle, params);  
    //把上面配置的参数提交给 ALSA 驱动   
     if (rc < 0) {
        LOG_ERROR("ALSA hw_params failed: %s", snd_strerror(rc));
        snd_pcm_close(s_ap.pcm_handle);
        s_ap.pcm_handle = NULL;
        return ERR_NODEV;
    }

    LOG_DEBUG("ALSA opened: device=%s, rate=%u, channels=%d",
              device ? device : "default", actual_rate, channels);
    return ERR_OK;                        
}

static void alsa_close(void){
    if(s_ap.pcm_handle){
        snd_pcm_drain(s_ap.pcm_handle);
        snd_pcm_close(s_ap.pcm_handle);
        s_ap.pcm_handle=NULL;
    }
}

//WAV 解析 + 播放核心
//打开文件 + 读 WAV 头
static err_t play_wav_internal(const char *filepath){
    //打开文件
    FILE *fp = fopen(filepath,"rb");
    if(!fp){
        LOG_ERROR("Cannot open audio file: %s", filepath);
        return ERR_NODEV;
    }
    //读取wav头
    wav_header_t hdr;
    if(fread(&hdr,sizeof(hdr),1,fp)!=1){
        LOG_ERROR("Failed to read WAV header: %s", filepath);
        fclose(fp);
        return ERR_INVAL;
    }

    //校验 WAV 签名
    if (memcmp(hdr.riff, "RIFF", 4) != 0 || memcmp(hdr.wave, "WAVE", 4) != 0) {
        LOG_ERROR("Not a valid WAV file: %s", filepath);
        fclose(fp);
        return ERR_INVAL;
    }

    //只支持 16-bit PCM
    if(hdr.audio_format !=1||hdr.bits_per_sample!=16){
        LOG_ERROR("Unsupported WAV format: fmt=%d bits=%d (only PCM16 supported)",
                  hdr.audio_format, hdr.bits_per_sample);
        fclose(fp);
        return ERR_INVAL;
    }

    //计算帧参数（用于缓冲区分配和播放循环）
    int frame_size = hdr.num_channels * (hdr.bits_per_sample / 8);
    int frames_total = hdr.data_size / frame_size;

    //打开ALSA
    err_t err= alsa_open(s_ap.cfg.device,hdr.sample_rate,hdr.num_channels);
    if(IS_ERR(err)){
        fclose(fp);
        return err;
    }
    
    //分配 PCM 数据缓冲区
    size_t buf_samples=4096;
    size_t buf_bytes=buf_samples *frame_size;
    char *buf=malloc(buf_bytes);
    if(!buf){
        alsa_close();
        fclose(fp);
        return ERR_NOMEM;
    }

    //播放循环
    int frames_played=0;
    s_ap.stop=false;

    while(frames_played<frames_total &&!s_ap.stop){
        size_t frames_to_read=buf_samples;
        if(frames_to_read>(size_t)(frames_total-frames_played))
            frames_to_read=(size_t)(frames_total-frames_played);
        
        size_t bytes_to_read=frames_to_read *frame_size;
        size_t bytes_read =fread(buf,1,bytes_to_read,fp);
        if(bytes_read==0) break;

        size_t frames_read=bytes_read / frame_size;
        if(frames_read==0) break;

        //写入ALSA
        int rc = snd_pcm_writei(s_ap.pcm_handle,buf,frames_read);
        if(rc==-EPIPE){
            //XRUN（缓冲区欠载）— 恢复
            LOG_WARN("ALSA xrun occurred, recovering...");
            snd_pcm_prepare(s_ap.pcm_handle);
        }else if(rc<0){
            LOG_ERROR("ALSA write error: %s", snd_strerror(rc));
            break;
        }else{
            frames_played+=rc;
        }
        }
        free(buf);
        fclose(fp);

        //等待所有数据流播放完
        if(!s_ap.stop){
            snd_pcm_drain(s_ap.pcm_handle);
        }
        alsa_close();

        if(s_ap.stop){
            LOG_INFO("Playback stopped: %s", filepath);
            return ERR_ABORTED;
        }
    LOG_INFO("Playback complete: %s (%d frames)", filepath, frames_played);
    return ERR_OK;
}

//异步播放线程
struct play_async_arg{
    char filepath[512];// 复制文件路径（栈上不安全）
};

static void *play_async_thread(void *arg){
    struct play_async_arg *pa = (struct play_async_arg *)arg;
    s_ap.playing=true;

    err_t err=play_wav_internal(pa->filepath);
    //内部播放 wav 音频文件的底层函数

    s_ap.playing=false;

    if(err==ERR_OK){
        event_publish_simple(EV_AUDIO_PLAY_DONE); // 播完通知
    }else if(err== ERR_ABORTED){
    }else{
        event_publish_simple(EV_AUDIO_ERROR);// 出错通知
    }

    free(pa);
    return NULL;
}

///接口实现
//init
err_t audio_player_init(const audio_player_config_t *cfg){
    if(s_ap.inited) return ERR_OK;

    if(cfg){
        s_ap.cfg=*cfg;
    }
    if(!s_ap.cfg.device) s_ap.cfg.device="default";

    s_ap.inited=true;
    LOG_INFO("Audio player initialized (device=%s)", s_ap.cfg.device);
    return ERR_OK;
}

//deinit
void audio_player_deinit(void){
    if(!s_ap.inited) return;
    //如果正在播放，请求停止并等待；
    if(s_ap.playing){
        audio_player_stop();
        pthread_join(s_ap.play_thread, NULL);
    }
    
    /* 关闭流式模式（如果有） */
    audio_player_stream_stop();

    alsa_close();
    s_ap.inited=false;
    LOG_INFO("Audio player deinitialized");
}

err_t audio_player_play(const char *filepath){
    if(!s_ap.inited) return ERR_GENERAL;
    return play_wav_internal(filepath);
}

err_t audio_player_play_async(const char *filepath){
    if(!s_ap.inited||!filepath) return ERR_INVAL;

    //如果正在播放，先停掉
    if(s_ap.playing){
        audio_player_stop();
        pthread_join(s_ap.play_thread, NULL);
    }

    struct play_async_arg *pa=calloc(1,sizeof(struct play_async_arg));
    if(!pa) return ERR_NOMEM;
    strncpy(pa->filepath, filepath, sizeof(pa->filepath)-1);

    pthread_create(&s_ap.play_thread, NULL, play_async_thread, pa);
    pthread_detach(s_ap.play_thread);

    return ERR_OK;
}

void audio_player_stop(void){
    s_ap.stop=true;
    //关闭 PCM 设备踢醒阻塞的 snd_pcm_writei
    if(s_ap.pcm_handle){
        snd_pcm_drop(s_ap.pcm_handle);
    }
}

/* ==================================================================
 * 流式模式（A2DP 连续 PCM 播放）
 * ================================================================ */

err_t audio_player_stream_start(unsigned int rate, int channels)
{
    /* 互斥检查：WAV 正在播放时不能开流式 */
    if (s_ap.playing) return ERR_BUSY;
    /* 已经在流式了 */
    if (s_ap.streaming) return ERR_OK;

    int rc;

    /* 打开 PCM 设备 */
    rc = snd_pcm_open(&s_ap.stream_pcm,
                      s_ap.cfg.device ? s_ap.cfg.device : "default",
                      SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        LOG_ERROR("AP stream open failed: %s", snd_strerror(rc));
        return ERR_NODEV;
    }

    /* 设置硬件参数 */
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(s_ap.stream_pcm, params);

    snd_pcm_hw_params_set_access(s_ap.stream_pcm, params,
                                  SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(s_ap.stream_pcm, params,
                                  SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(s_ap.stream_pcm, params, channels);

    unsigned int actual_rate = rate;
    snd_pcm_hw_params_set_rate_near(s_ap.stream_pcm, params,
                                     &actual_rate, 0);

    /* period 256 帧 = 约 6ms@44100Hz */
    snd_pcm_uframes_t period = 256;
    snd_pcm_hw_params_set_period_size_near(s_ap.stream_pcm, params,
                                            &period, 0);

    rc = snd_pcm_hw_params(s_ap.stream_pcm, params);
    if (rc < 0) {
        LOG_ERROR("AP stream hw_params failed: %s", snd_strerror(rc));
        snd_pcm_close(s_ap.stream_pcm);
        s_ap.stream_pcm = NULL;
        return ERR_NODEV;
    }

    s_ap.streaming = true;
    LOG_INFO("AP streaming started (%u Hz, %d ch)", actual_rate, channels);
    return ERR_OK;
}

err_t audio_player_stream_write(const void *data, size_t frames)
{
    if (!s_ap.streaming || !s_ap.stream_pcm) return ERR_GENERAL;

    int rc = snd_pcm_writei(s_ap.stream_pcm, data, frames);
    if (rc == -EPIPE) {
        /* XRUN — 恢复 */
        LOG_WARN("AP stream xrun, recovering...");
        snd_pcm_prepare(s_ap.stream_pcm);
        /* 恢复后重试一次 */
        rc = snd_pcm_writei(s_ap.stream_pcm, data, frames);
    }
    if (rc < 0) {
        LOG_ERROR("AP stream write error: %s", snd_strerror(rc));
        return ERR_GENERAL;
    }
    return ERR_OK;
}

void audio_player_stream_stop(void)
{
    if (!s_ap.streaming) return;

    if (s_ap.stream_pcm) {
        snd_pcm_drain(s_ap.stream_pcm);
        snd_pcm_close(s_ap.stream_pcm);
        s_ap.stream_pcm = NULL;
    }

    s_ap.streaming = false;
    LOG_INFO("AP streaming stopped");
}