#ifndef COMMON_H
#define COMMON_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
/*  错误码定义  */
typedef enum {
    ERR_OK          = 0,     /* 成功 */
    ERR_GENERAL     = -1,    /* 通用错误 */
    ERR_NOMEM       = -2,    /* 内存不足 */
    ERR_TIMEOUT     = -3,    /* 超时 */
    ERR_INVAL       = -4,    /* 参数无效 */
    ERR_NODEV       = -5,    /* 设备不存在 */
    ERR_BUSY        = -6,    /* 资源忙 */
    ERR_DISCONN     = -7,    /* 连接断开 */
    ERR_AGAIN       = -8,    /* 需要重试 */
    ERR_NOT_FOUND   = -9,    /* 未找到 */
    ERR_STATE_INV   = -13,   /* 状态错误（改名避免与 OpenSSL 冲突） */
    ERR_ABORTED     = -11,   /* 操作被终止 */
    ERR_FULL        = -12,   /* 资源已满 */
} err_t;

#define IS_ERR(e) ((e)<0)   /* 是不是错误 */
#define IS_OK(e) ((e)>=0)   /* 是不是成功 */

  /*  事件ID定义  */
  typedef enum {
      /* 系统事件 */
      EV_SYS_SHUTDOWN     = 0x0001,  /* 系统关闭 */
      EV_SYS_ERROR        = 0x0002,  /* 系统级错误 */

      /* 蓝牙事件 */
      EV_BT_DEVICE_CONN   = 0x0103,  /* 设备已连接 */
      EV_BT_DEVICE_DISCONN= 0x0104,  /* 设备已断开 */
      EV_BT_DATA_RECEIVED = 0x0105,  /* 收到数据（文本命令） */

      /* 音频事件 */
      EV_AUDIO_PLAY_DONE      = 0x0201,  /* TTS播放完毕 */
      EV_AUDIO_MUSIC_START    = 0x0202,  /* A2DP音乐开始 */
      EV_AUDIO_MUSIC_STOP     = 0x0203,  /* A2DP音乐停止 */
      EV_AUDIO_ERROR          = 0x0204,  /* 音频错误 */
      EV_AUDIO_CAPTURE_DATA   = 0x0205,  /* 录音PCM数据帧 */
      EV_AUDIO_CAPTURE_START  = 0x0206,  /* 录音开始 */
      EV_AUDIO_CAPTURE_STOP   = 0x0207,  /* 录音停止 */

      /* AI事件 */
      EV_AI_RESP_READY    = 0x0300,  /* AI回复就绪 */
      EV_AI_TTS_DONE      = 0x0301,  /* TTS音频就绪 */
      EV_AI_ERROR         = 0x0303,  /* AI调用失败 */
      EV_AI_STREAM_CHUNK  = 0x0304,  /* AI流式数据到了 */

      /* 唤醒事件 */
      EV_WAKEUP_DETECTED  = 0x0400,  /* 离线唤醒词触发 */

      /* 网络事件 */
      EV_NET_CONNECTED    = 0x0500,  /* 网络已连接 */
      EV_NET_DISCONNECTED = 0x0501,  /* 网络已断开 */
  } event_id_t;


    /*  事件通用结构  */
  typedef struct event {
      event_id_t      id;            /* 事件ID */
      int64_t         timestamp_ms;  /* 发生时间 */
      void           *data;          /* 附带数据（谁发布谁释放） */
      size_t          data_size;     /* 数据大小 */
      void          (*data_destroy)(void *data);  /* 释放data的函数 */
  } event_t;

   /* 事件回调函数类型 */
  typedef void (*event_handler_t)(const event_t *ev, void *user_data);
  typedef unsigned long task_id_t;
  /* 日志级别 */
  typedef enum {
      LOG_DEBUG = 0,
      LOG_INFO  = 1,
      LOG_WARN  = 2,
      LOG_ERROR = 3,
  } log_level_t;

  /* ======================== 辅助宏 ======================== */
  #define ARRAY_SIZE(a)       (sizeof(a) / sizeof((a)[0]))
  #define MIN(a, b)           ((a) < (b) ? (a) : (b))
  #define MAX(a, b)           ((a) > (b) ? (a) : (b))
  #define CLAMP(val, lo, hi)  (MAX(lo, MIN(val, hi)))

  /* 安全释放：free后把指针置NULL，防止double free */
  #define SAFE_FREE(p)        do { free(p); (p) = NULL; } while(0)

   /* ======================== 版本信息 ======================== */
  #define PROJECT_NAME    "AI-Speaker"
  #define PROJECT_VERSION "1.0.0"


#endif /* COMMON_H */