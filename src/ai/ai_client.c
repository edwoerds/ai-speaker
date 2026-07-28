#include "ai_client.h"
#include "logger.h"
#include <curl/curl.h>
#include <stdlib.h>    
#include <string.h>
#include <stdio.h>
  #include <unistd.h>
  #define RETRY_MAX      3
  #define RETRY_DELAY_MS 200
//内部数据结构
//SSE 流式解析状态
typedef struct {
      char     *buffer;       /* 行缓冲区：累积不完整的行 */
      size_t    buflen;       /* buffer 当前长度 */
      size_t    bufsz;        /* buffer 容量 */
      char     *accumulated;  /* 已累积的完整响应文本 */
      size_t    acclen;//  - acclen — 已经攒了多少字符
      size_t    accsz;//- accsz — accumulated 的容量
      ai_stream_cb_t cb;      /* 每块的回调 */
      void     *user_data;
  } sse_parser_t;

  //libcurl write 回调的上下文
  typedef struct {
      int                is_streaming;    /* 0=同步模式, 1=流式模式 */
      struct curl_slist *headers;         /* 请求头指针（清理时释放用） */
      union {
          struct {                        /* 流式模式：用 SSE 解析器 */
              sse_parser_t *parser;
          } s;
          struct {                        /* 同步模式：直接累积文本 */
              char   *buf;
              size_t  len;
              size_t  sz;
          } u;
      } u;
  } write_ctx_t;
  //全局状态
    static struct {
      ai_client_config_t cfg;    /* AI 客户端配置 */
      bool    inited;            /* 是否已初始化的标记 */
  } s_ai = {0};
  //SSE 解析器
  //从data json片段中提取“content”字段指
  static char *extract_content(const char *json_frag){
    /* 查找 "content":" */
    const char *key="\"content\":\"";
    const char *start=strstr(json_frag,key);
    //strstr 在 json_frag 里搜索 key，找到就返回指向 key 开头位置的指针，找不到返回 NULL。
    if(!start) return NULL;
    start += strlen(key);
    //找结束引导
    const char *end=start;
    while(*end){
        if(*end == '\\' && (*(end+1) == '"' || *(end+1) == '\\' || *(end+1) == 'n'))
            end+=2;
        else if(*end == '"')
                break;  
        else 
             end++;
    }
    if (end<=start) return NULL;
    
    size_t len=(size_t)(end-start);
    char *out =malloc(len+1);
    if(!out) return NULL;
    //解码转义
    size_t j=0;
    for(size_t i=0;i<len;i++){
        if(start[i]=='\\' && i+1<len){
            switch(start[i+1]){
                  case 'n': out[j++] = '\n'; i++; continue;
                  case '"': out[j++] = '"';  i++; continue;
                  case '\\':out[j++] = '\\'; i++; continue;
                  case 'r':                  i++; continue;
                  case 't': out[j++] = '\t'; i++; continue;
                  default:  out[j++] = start[i]; continue;
            }
        }
       out[j++]=start[i];
    }
    out[j]='\0';
    return out; 
  }
  
  /* 解析一行 SSE 数据 */
  static void sse_process_line(sse_parser_t *p, const char *line, size_t len)
  {
      if (len == 0) return;

      /* 检查 "data:" 前缀 */
      const char *data_prefix = "data:";
      size_t prefix_len = strlen(data_prefix);
      if (len < prefix_len || strncmp(line, data_prefix, prefix_len) != 0)
          return;

      const char *content = line + prefix_len;
      size_t clen = len - prefix_len;

      /* 检查结束标记 [DONE] */
      if (clen == strlen("[DONE]") &&
          memcmp(content, "[DONE]", strlen("[DONE]")) == 0) {
          if (p->accumulated && p->cb) {
              char *full = strdup(p->accumulated);
              if (full) {
                  p->cb(full, 1, p->user_data);
                  free(full);
              }
          } else if (p->cb) {
              p->cb(NULL, 0, p->user_data);
          }
          return;
      }

      /* 从 JSON 片段中提取 content 字段 */
      char *chunk = extract_content(content);
      if (!chunk) return;

      /* 累积到完整响应 */
      size_t chunk_len = strlen(chunk);
      if (p->acclen + chunk_len + 1 > p->accsz) {
          p->accsz = p->accsz ? p->accsz * 2 : 4096;
          while (p->acclen + chunk_len + 1 > p->accsz)
              p->accsz *= 2;
          char *new_buf = realloc(p->accumulated, p->accsz);
          if (!new_buf) {
              free(chunk);
              return;
          }
          p->accumulated = new_buf;
      }
      memcpy(p->accumulated + p->acclen, chunk, chunk_len);
      p->acclen += chunk_len;

      /* 回调通知 */
      if (p->cb)
          p->cb(chunk, 0, p->user_data);

      free(chunk);
  }

//sse_feed 函数
//向 SSE 解析器喂数据（可能跨多次回调）
static void sse_feed(sse_parser_t *p,const char *data,size_t len){
    for(size_t i=0; i<len; i++){
        char c =data[i];
        if(c=='\n'){
            //一行结束
            p->buffer[p->buflen]='\0';
            sse_process_line(p, p->buffer, p->buflen);
            p->buflen=0;
        }else if(c=='\r'){
            //忽略CR等LF
        }
        else{
            //追加到行缓冲
            if(p->buflen+1>=p->bufsz){
                p->bufsz= p->bufsz?p->bufsz*2:256;
                char *new_buf=realloc(p->buffer, p->bufsz);
                if(!new_buf){
                    p->buflen=0;
                    break;
                }
                p->buffer=new_buf;
            }
            p->buffer[p->buflen++]=c;   
        }
    }
}


//sse_parser_create
//构造函数——创建一个 SSE 解析器实例
static sse_parser_t *sse_parser_create(ai_stream_cb_t cb, void *user_data){
    sse_parser_t *p = calloc(1, sizeof(sse_parser_t));
    if (!p) return NULL;
    p->bufsz=256;
    p->buffer = malloc(p->bufsz);
    p->accsz = 4096;
    p->accumulated = malloc(p->accsz);
    p->cb = cb;
    p->user_data = user_data;
    if (!p->buffer || !p->accumulated) {
        free(p->buffer); free(p->accumulated); free(p);
        return NULL;
    }
    p->accumulated[0] = '\0';
    //accumulated[0] = '\0' — 初始化为空字符串。方便后面用 strlen 之类函数处理
    return p;
}

//sse_parser_destroy
// 析构函数——释放 SSE 解析器占用的内存
static void sse_parser_destroy(sse_parser_t *p)
{
    if (!p) return;
    free(p->buffer);
    free(p->accumulated);
    free(p);
}

/* ==================================================================
* libcurl 回调
* ================================================================ */

//write_cb 函数
//这是 libcurl 的写回调。libcurl 每次从服务器收到一段数据，就会调这个函数
//  参数：
//- data — 收到的 HTTP 响应数据
//- size — 每个数据块的大小（总是 1）
//- nmemb — 数据块的数量
//- userp — 我们传的上下文（write_ctx_t *）
static size_t write_cb(char *data, size_t size, size_t nmemb, void *userp)
{
    write_ctx_t *ctx = (write_ctx_t *)userp;
    size_t total = size * nmemb;
    if(ctx->is_streaming){
        sse_feed(ctx->u.s.parser, data, total);
    }else{
        //同步模式：累积到缓冲区
        if(ctx->u.u.len + total + 1 > ctx->u.u.sz){
            ctx->u.u.sz = ctx->u.u.sz ? ctx->u.u.sz*2 : 4096;
            while(ctx->u.u.len + total + 1 > ctx->u.u.sz) ctx->u.u.sz *= 2;
            char *new_buf = realloc(ctx->u.u.buf, ctx->u.u.sz);
            if(!new_buf) return 0;
            ctx->u.u.buf = new_buf;
        }
        memcpy(ctx->u.u.buf + ctx->u.u.len, data, total);
        ctx->u.u.len += total;
        ctx->u.u.buf[ctx->u.u.len] = '\0';
    }
    // 返回消费的字节数，告诉 libcurl "数据已处理完毕"。
    return total;
}

/* ==================================================================
 * AI 客户端接口实现
 * ================================================================ */
//ai_client_init 函数
//初始化 AI 客户端  
err_t ai_client_init(const ai_client_config_t *cfg)
{
    if (!cfg) return ERR_INVAL;
    s_ai.cfg = *cfg;
     /* 用默认值补全 */
    if (!s_ai.cfg.api_url)  s_ai.cfg.api_url  = "https://api.deepseek.com/chat/completions";
    if (!s_ai.cfg.model)    s_ai.cfg.model    = "deepseek-chat";
    if (s_ai.cfg.timeout_ms <= 0) s_ai.cfg.timeout_ms = 30000;
    if (s_ai.cfg.temperature <= 0) s_ai.cfg.temperature = 0.7f;
    if (s_ai.cfg.max_tokens <= 0)  s_ai.cfg.max_tokens  = 2048;
    //curl_global_init(CURL_GLOBAL_ALL) — 初始化 libcurl 库。CURL_GLOBAL_ALL 表示初始化 SSL 和所有子系统。
    //这行必须在整个程序任何curl调用之前执行一次
    CURLcode rc=curl_global_init(CURL_GLOBAL_ALL);
    if(rc!=CURLE_OK){
        LOG_ERROR("curl_global_init failed: %s", curl_easy_strerror(rc));
        return ERR_GENERAL;
    }
    //标记已初始化，记录日志，返回成功。
    s_ai.inited = true;
    LOG_INFO("AI client initialized (model=%s, url=%s)",
            s_ai.cfg.model, s_ai.cfg.api_url);
    return ERR_OK;
}

//ai_client_deinit
void ai_client_deinit(void)
{
    if (!s_ai.inited) return;
    //curl_global_cleanup() — 清理 libcurl 全局状态
    curl_global_cleanup();
    s_ai.inited = false;
    LOG_INFO("AI client deinitialized");
}

//create_curl_handle
//内部：创建并配置 CURL 句柄
static CURL *create_curl_handle(const char *json_payload,write_ctx_t *wctx){
    CURL *curl =curl_easy_init();
    //创建一个 CURL 句柄（相当于一个 HTTP 请求对象）。失败返回 NULL
    //构建 HTTP 请求头：
  //- Content-Type: application/json — 告诉服务器请求体是 JSON
  //- 如果有 API Key，加 Authorization: Bearer sk-xxx — 身份验证
    if(!curl) return NULL;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    //curl_slist_append 构建一个字符串链表，每调一次加一个头
    if(s_ai.cfg.api_key&&s_ai.cfg.api_key[0]){
        char auth_hdr[512];
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", s_ai.cfg.api_key);
        headers=curl_slist_append(headers, auth_hdr);
    }
    wctx->headers=headers;
    //存下来，清理时释放
    curl_easy_setopt(curl, CURLOPT_URL,            s_ai.cfg.api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     json_payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)strlen(json_payload));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      wctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,     (long)s_ai.cfg.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "AI-Speaker/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,   CURL_HTTP_VERSION_1_1);

    /* 允许重定向 */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      3L);

    /* SSL 跳过 peer 验证（嵌入式环境证书不全时用） */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,  0L);

    /* 禁止连接复用，避免 SSE 流残留数据影响下一次请求 */
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);

    return curl;
}

//ai_client_chat
//流式调用
//核心接口：向 AI 发送问题并获取回答
err_t ai_client_chat(const char *json_payload, ai_stream_cb_t cb, void *user_data)
{
    if (!s_ai.inited || !json_payload) return ERR_INVAL;

    CURLcode rc = CURLE_OK;
    sse_parser_t *parser = NULL;
    CURL *curl = NULL;
    write_ctx_t wctx;
    int attempt;

    for (attempt = 0; attempt < RETRY_MAX; attempt++) {
        parser = sse_parser_create(cb, user_data);
        if (!parser) return ERR_NOMEM;

        memset(&wctx, 0, sizeof(wctx));
        wctx.is_streaming = 1;
        wctx.u.s.parser = parser;

        curl = create_curl_handle(json_payload, &wctx);
        if (!curl) {
            sse_parser_destroy(parser);
            return ERR_NOMEM;
        }

        rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) break;  /* 成功，退出重试 */

        LOG_WARN("AI chat attempt %d/%d failed: %s",
                 attempt + 1, RETRY_MAX, curl_easy_strerror(rc));

        curl_easy_cleanup(curl);
        curl_slist_free_all(wctx.headers);
        sse_parser_destroy(parser);
        curl = NULL;
        parser = NULL;

        if (attempt + 1 < RETRY_MAX)
            usleep(RETRY_DELAY_MS * 1000);
    }

    /* 发送 done 信号（无论成功还是最后一次失败，都通知调用方流结束） */
    if (parser) {
        if (parser->accumulated && parser->acclen > 0 && cb) {
            char *full = strdup(parser->accumulated);
            if (full) {
                cb(full, 1, user_data);
                free(full);
            }
        } else if (cb) {
            cb(NULL, 1, user_data);
        }
        curl_easy_cleanup(curl);
        curl_slist_free_all(wctx.headers);
        sse_parser_destroy(parser);
    }

    if (rc != CURLE_OK) {
        LOG_ERROR("AI chat request failed after %d attempts", RETRY_MAX);
    }

    return (rc == CURLE_OK) ? ERR_OK : ERR_GENERAL;
}

//ai_client_chat_sync
err_t ai_client_chat_sync(const char *json_payload, char **out_response)
{
    if (!s_ai.inited || !json_payload || !out_response) return ERR_INVAL;

    CURLcode rc = CURLE_OK;
    int attempt;

    for (attempt = 0; attempt < RETRY_MAX; attempt++) {
        write_ctx_t wctx;
        memset(&wctx, 0, sizeof(wctx));
        wctx.is_streaming = 0;

        CURL *curl = create_curl_handle(json_payload, &wctx);
        if (!curl) return ERR_NOMEM;

        rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code != 200) {
                LOG_WARN("AI sync attempt %d/%d failed: HTTP %ld",
                         attempt + 1, RETRY_MAX, http_code);
                free(wctx.u.u.buf); wctx.u.u.buf = NULL;
                curl_easy_cleanup(curl);
                curl_slist_free_all(wctx.headers);
                if (attempt + 1 < RETRY_MAX) usleep(RETRY_DELAY_MS * 1000);
                continue;
            }
            char *raw = wctx.u.u.buf ? wctx.u.u.buf : strdup("");
            /* 从 JSON 响应中提取 content 字段 */
            char *content = extract_content(raw);
            free(raw);
            if (content && content[0]) {
                *out_response = content;
            } else {
                free(content);
                LOG_WARN("AI sync response has no content (HTTP 200, try: ask again)");
                *out_response = strdup("");
                curl_easy_cleanup(curl);
                curl_slist_free_all(wctx.headers);
                return ERR_GENERAL;
            }
            curl_easy_cleanup(curl);
            curl_slist_free_all(wctx.headers);
            return ERR_OK;
        }

        LOG_WARN("AI sync attempt %d/%d failed: %s",
                 attempt + 1, RETRY_MAX, curl_easy_strerror(rc));

        free(wctx.u.u.buf);
        curl_easy_cleanup(curl);
        curl_slist_free_all(wctx.headers);

        if (attempt + 1 < RETRY_MAX)
            usleep(RETRY_DELAY_MS * 1000);
    }

    *out_response = strdup("");
    LOG_ERROR("AI sync request failed after %d attempts", RETRY_MAX);
    return ERR_GENERAL;
}

