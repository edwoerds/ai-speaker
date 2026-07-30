#include "ai_stt.h"
#include "logger.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * 百度语音识别 REST API
 *
 * 两步流程：
 *   ① OAuth 获取 access_token（POST, 有效期~30天）
 *   ② 语音识别请求（POST, base64 PCM + JSON 参数）
 *
 * API 文档：https://ai.baidu.com/ai-doc/SPEECH/Vk38lxily
 */

/* ==================================================================
 * 常量
 * ================================================================ */

#define TOKEN_URL \
    "https://aip.baidubce.com/oauth/2.0/token" \
    "?grant_type=client_credentials"

#define ASR_URL \
    "http://vop.baidu.com/server_api"

#define CUID "ai-speaker"          /* 设备标识（任意字符串） */
#define DEV_PID 1537               /* 普通话(中文)短语音识别模型 */
#define MAX_TOKEN_LEN 256          /* access_token 最大长度 */
#define MAX_RESP_BODY 4096         /* HTTP 响应缓冲区 */

/* ==================================================================
 * 全局状态
 * ================================================================ */

static struct {
    bool    inited;               /* 模块已初始化 */
    bool    configured;           /* 用户配置了 api_key */
    char    access_token[MAX_TOKEN_LEN];  /* 百度 access_token */
    int     timeout_ms;           /* HTTP 超时 */
} s_stt = {0};

/* ==================================================================
 * HTTP 响应收集回调（libcurl writeback）
 * ================================================================ */

struct resp_buf {
    char  data[MAX_RESP_BODY];
    size_t len;
};

static size_t on_data(void *ptr, size_t size, size_t nmemb, void *user)
{
    struct resp_buf *rb = (struct resp_buf *)user;
    size_t available = sizeof(rb->data) - rb->len - 1;
    size_t incoming  = size * nmemb;
    if (incoming > available) incoming = available;
    if (incoming == 0) return incoming;
    memcpy(rb->data + rb->len, ptr, incoming);
    rb->len += incoming;
    rb->data[rb->len] = '\0';
    return incoming;
}

/* ==================================================================
 * 简易 JSON 提取器（纯指针扫描，无递归，无反斜杠转义）
 * ================================================================ */

/*
 * 在 JSON 字符串中找 "key":" 后面的值字符串
 * json:  {"err_no":0,"result":["今天天气不错"]}
 * key:   "result"
 * out:   指向 " 今天天气不错"（需要 caller 处理闭引号）
 * out_len: 值字符串长度
 *
 * 返回: 0 找到, -1 未找到
 *
 * 限制：
 *   - 只处理 "key":"value" 形式（不处理数字/布尔/嵌套）
 *   - value 中不能有 \" 转义（百度返回不含）
 */
static int json_extract_string(const char *json, const char *key,
                                char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) return -1;

    /* 构造 "key": 搜索模式（JSON 的 key 带引号） */
    char pattern[128];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof(pattern)) return -1;

    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *q = p + strlen(pattern);
        /* 跳过空白 */
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '"') { p = q; continue; }
        q++; /* 跳过开引号 */
        /* 提取 value 到闭引号 */
        size_t i = 0;
        while (*q && *q != '"' && i < out_size - 1) {
            out[i++] = *q++;
        }
        out[i] = '\0';
        return (i > 0) ? 0 : -1;
    }
    return -1;
}

/* ==================================================================
 * 针对百度 ASR 响应的专用提取器
 *
 * 示例 JSON:
 *   {"err_no":0,"err_msg":"success.","result":["今天天气不错"],"sn":"..."}
 *
 * 标准 json_extract_string() 只处理 "key":"value"，
 * 但 err_no 是数字，result 是数组 → 需要用专用函数
 * ================================================================ */

/*
 * 提取 err_no 数值
 * json:  {"err_no":0,"err_msg":"success.",...}
 * 返回: err_no 值（>=0），-1（未找到）
 */
static int json_extract_errno(const char *json)
{
    /* 找 "err_no": , 然后跳过冒号到数值 */
    const char *p = strstr(json, "\"err_no\"");
    if (!p) return -1;
    p += 8; /* 跳过 "err_no" */
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return -1;
    p++; /* 跳过冒号 */
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

/*
 * 提取 result 数组的第一个字符串元素
 * json:  {"err_no":0,"result":["今天天气不错"]}
 * out:   "今天天气不错"
 */
static int json_extract_first_result(const char *json, char *out, size_t out_size)
{
    /* 找 "result":[" */
    const char *p = strstr(json, "\"result\":[");
    if (!p) return -1;
    p += 10; /* 跳过 "result":[" */
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++; /* 跳过开引号 */
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/* ==================================================================
 * 步骤①：获取 access_token（在 ai_stt_init 中串行完成）
 * ================================================================ */
static err_t do_fetch_token(const char *api_key, const char *secret_key)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("STT: curl_easy_init failed");
        return ERR_NOMEM;
    }

    char url[512];
    snprintf(url, sizeof(url),
             "%s&client_id=%s&client_secret=%s",
             TOKEN_URL, api_key, secret_key);

    struct resp_buf rb = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)s_stt.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  /* 测试环境关闭证书验证 */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR("STT: token HTTP failed: %s", curl_easy_strerror(res));
        return ERR_TIMEOUT;
    }

    if (rb.len == 0) {
        LOG_ERROR("STT: token response empty");
        return ERR_GENERAL;
    }

    /* 解析 access_token */
    char token[MAX_TOKEN_LEN] = "";
    if (json_extract_string(rb.data, "access_token", token, sizeof(token)) < 0) {
        LOG_ERROR("STT: token parse failed, response=%.256s", rb.data);
        return ERR_GENERAL;
    }

    strncpy(s_stt.access_token, token, sizeof(s_stt.access_token) - 1);
    s_stt.access_token[sizeof(s_stt.access_token) - 1] = '\0';
    LOG_INFO("STT: access_token obtained (%.16s...)", token);
    return ERR_OK;
}

/* ==================================================================
 * 简易 Base64 编码器（RFC 4648）
 * 不依赖 OpenSSL，纯自包含
 * ================================================================ */

static const char s_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static char *base64_encode(const uint8_t *in, size_t in_len, size_t *out_len)
{
    size_t out_size = 4 * ((in_len + 2) / 3) + 1;  /* +1 for \0 */
    char *out = (char *)malloc(out_size);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < in_len) {
        uint32_t octets = 0;
        int remaining = (int)(in_len - i);
        int pad = (remaining < 3) ? (3 - remaining) : 0;

        for (int k = 0; k < 3; k++) {
            octets <<= 8;
            if (i < in_len) octets |= in[i++];
        }

        out[j++] = s_b64[(octets >> 18) & 0x3F];
        out[j++] = s_b64[(octets >> 12) & 0x3F];
        out[j++] = (pad >= 2) ? '=' : s_b64[(octets >> 6) & 0x3F];
        out[j++] = (pad >= 1) ? '=' : s_b64[octets & 0x3F];
    }
    out[j] = '\0';
    *out_len = j;
    return out;
}

/* ==================================================================
 * 步骤②：语音识别请求
 *
 * POST http://vop.baidu.com/server_api
 * Body: JSON {"format":"pcm","rate":16000,"channel":1,
 *             "cuid":"ai-speaker","token":"<access_token>",
 *             "dev_pid":1537,
 *             "speech":"<base64_pcm>","len":<bytes>}
 * (JSON 方式下所有参数放 body)
 * ================================================================ */

static err_t do_stt(const int16_t *audio, size_t frames, char **out_text)
{
    if (!audio || frames == 0 || !out_text) return ERR_INVAL;
    if (!s_stt.access_token[0]) return ERR_NOT_FOUND;

    size_t audio_bytes = frames * sizeof(int16_t);

    /* 1. Base64 编码 PCM */
    size_t b64_len = 0;
    char *b64 = base64_encode((const uint8_t *)audio, audio_bytes, &b64_len);
    if (!b64) return ERR_NOMEM;

    /* 2. 构建 JSON body（手动 sprintf 避免引入 cJSON） */
    size_t body_size = b64_len + 512;
    char *body = (char *)malloc(body_size);
    if (!body) { free(b64); return ERR_NOMEM; }

    int n = snprintf(body, body_size,
        "{\"format\":\"pcm\",\"rate\":16000,\"channel\":1,"
        "\"cuid\":\"" CUID "\","
        "\"token\":\"%s\","
        "\"dev_pid\":%d,"
        "\"speech\":\"%s\","
        "\"len\":%zu}",
        s_stt.access_token, DEV_PID, b64, audio_bytes);
    free(b64);

    if (n < 0 || (size_t)n >= body_size) {
        free(body);
        return ERR_GENERAL;
    }

    /* 3. HTTP POST — JSON 方式，所有参数放 body */
    char url[512];
    snprintf(url, sizeof(url), "%s", ASR_URL);

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return ERR_NOMEM; }

    struct resp_buf rb = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(n));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)s_stt.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);

    /* 保存 HTTP 状态码（必须在 cleanup 之前取） */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (res != CURLE_OK) {
        LOG_ERROR("STT: HTTP failed: %s", curl_easy_strerror(res));
        return ERR_TIMEOUT;
    }

    /* 4. 解析响应 */
    if (http_code != 200) {
        LOG_ERROR("STT: HTTP %ld, response=%.256s", http_code, rb.data);
        return ERR_GENERAL;
    }

    /* 检查 err_no（百度 API 即使成功也返回 HTTP 200） */
    {
        int err_no = json_extract_errno(rb.data);
        if (err_no > 0) {
            LOG_ERROR("STT: API error %d, response=%.512s", err_no, rb.data);
            if (err_no == 110) {  /* token 无效或过期 */
                s_stt.access_token[0] = '\0';
            }
            return ERR_GENERAL;
        }
    }

    /* 提取 result[0] 中的文本 */
    {
        char result[1024] = "";
        if (json_extract_first_result(rb.data, result, sizeof(result)) < 0) {
            LOG_ERROR("STT: no result in response: %.256s", rb.data);
            return ERR_GENERAL;
        }
        *out_text = strdup(result);
        if (!*out_text) return ERR_NOMEM;
        LOG_INFO("STT: recognized \"%.64s\"", result);
    }
    return ERR_OK;
}

/* ==================================================================
 * 接口实现
 * ================================================================ */

err_t ai_stt_init(const ai_stt_config_t *cfg)
{
    if (s_stt.inited) return ERR_OK;
    if (!cfg) return ERR_INVAL;

    s_stt.timeout_ms = (cfg->timeout_ms > 0) ? cfg->timeout_ms : 5000;

    /* 没有 API Key → 降级模式 */
    if (!cfg->api_key || cfg->api_key[0] == '\0' ||
        !cfg->secret_key || cfg->secret_key[0] == '\0') {
        LOG_WARN("STT: no api_key configured, voice wake will fallback to greeting");
        s_stt.configured = false;
        s_stt.inited = true;
        return ERR_OK;
    }

    s_stt.configured = true;

    /* 获取 access_token */
    err_t err = do_fetch_token(cfg->api_key, cfg->secret_key);
    if (IS_ERR(err)) {
        LOG_WARN("STT: failed to get access_token, fallback to greeting");
        /* 仍然标记 inited=true，do_fetch_token 失败时 process 会返回错误 */
        s_stt.inited = true;
        return ERR_OK;
    }

    LOG_INFO("STT: initialized (timeout=%dms)", s_stt.timeout_ms);
    s_stt.inited = true;
    return ERR_OK;
}

void ai_stt_deinit(void)
{
    s_stt.inited = false;
    s_stt.configured = false;
    s_stt.access_token[0] = '\0';
    LOG_INFO("STT: deinitialized");
}

err_t ai_stt_process(const int16_t *audio, size_t frames, char **out_text)
{
    if (!s_stt.inited) return ERR_NOT_FOUND;
    if (!s_stt.configured || !s_stt.access_token[0]) {
        LOG_WARN("STT: not configured, skipping recognition");
        return ERR_NOT_FOUND;
    }
    if (!audio || frames == 0 || !out_text) return ERR_INVAL;

    return do_stt(audio, frames, out_text);
}
