#ifndef AI_CLIENT_H
#define AI_CLIENT_H
#include "common.h"
typedef struct {
      const char *api_url;       /* API 端点 URL */
      const char *api_key;       /* API Key */
      const char *model;         /* 模型名 */
      int         timeout_ms;    /* HTTP 超时（默认 30000） */
      float       temperature;   /* 生成温度（0.0~2.0） */
      int         max_tokens;    /* 最大生成 token 数 */
} ai_client_config_t;
typedef void (*ai_stream_cb_t)(const char *chunk, int is_done, void *user_data);
  //ai_stream_cb_t 就是"每次蹦出一个字时，调用的那个函数的长相"：
  // chunk — 蹦出来的文本片段
  // is_done — 0 表示还有更多，1 表示流结束了
  // user_data — 你想捎给回调函数的额外数据（比如你想在回调里知道是哪个设备发起的请求，放这里）


  err_t ai_client_init(const ai_client_config_t *cfg);
  void ai_client_deinit(void);
  // init — 做全局一次性初始化（比如调 curl_global_init 初始化 libcurl 库）。参数传配置。
  //deinit — 做清理（比如 curl_global_cleanup）

  err_t ai_client_chat(const char *json_payload, ai_stream_cb_t cb, void *user_data);
  //流式调用：
  //1. 接收一个 JSON 字符串（对话历史 + 参数拼成的 payload）
  //2. 发给 AI API
  //3. 每收到一个 content 块，调一次 cb
  //4. 流结束了，调一次 cb 且 is_done=1
  err_t ai_client_chat_sync(const char *json_payload, char **out_response);
  // 同步调用：
  //1. 同样发 JSON payload
  //2. 等全部结果回来，拼成完整字符串
  //3. 通过 out_response 返回（调用方用完要 free）
  //har **out_response 的意思是：函数内部 malloc 一段内存存结果，通过这个二级指针交给你。这是 C 里"函数返回动态分配内存"的标准模式。

  #define CONV_MAX_TURNS      10
  #define CONV_MAX_CONTENT   4096
  //对话管理的宏：
  //- CONV_MAX_TURNS — 最多记住 10 轮对话
  //- CONV_MAX_CONTENT — 每条消息最多 4096 个字符

  typedef struct {
      char *role;      /* "system" | "user" | "assistant" */
      char *content;   /* 消息内容 */
  } conv_entry_t;

  err_t ai_conv_init(const char *system_prompt);
  void ai_conv_deinit(void);
  err_t ai_conv_append(const char *role, const char *content);
  char *ai_conv_build_payload(void);
  void ai_conv_clear(void);
    //对话管理接口：
  //- init — 设置 system prompt，初始化历史
  //- deinit — 清理历史
  //- append — 追加一条消息（自动裁剪超出的旧消息）
  //- build_payload — 把当前历史拼成 JSON 字符串（就是发给 AI 的那个请求体）
  //- clear — 清空历史，保留 system prompt
  typedef struct {
      const char *secret_id;       /* 腾讯云 SecretId */
      const char *secret_key;      /* 腾讯云 SecretKey */
      int         voice_type;      /* 音色编号（0=晓薇, 1=晓晓, 101001=智逸等） */
      const char *output_dir;      /* 输出目录 */
  } ai_tts_config_t;
    //- api_url — TTS 接口地址（默认 OpenAI 的 /v1/audio/speech）
  //- api_key — API Key
  //- voice — 音色（alloy / echo / fable / nova / shimmer）
  //- output_dir — 生成的音频文件存到哪
  err_t ai_tts_init(const ai_tts_config_t *cfg);
  void ai_tts_deinit(void);
  err_t ai_tts_speak(const char *text, char **out_path);
   #endif /* AI_CLIENT_H */