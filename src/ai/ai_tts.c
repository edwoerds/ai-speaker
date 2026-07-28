#include "ai_client.h"
#include "logger.h"

#include <curl/curl.h>

/* OpenSSL 3.x 标记 HMAC() 为 deprecated，用宏压制 -Werror 拦截 */
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * TTS 客户端 — 腾讯云语音合成（HTTP API，无外部进程依赖）
 *
 * 流程：
 *   ai_tts_speak(text):
 *     1. 构建 JSON 请求体（含转义）
 *     2. 计算 TC3-HMAC-SHA256 签名
 *     3. libcurl POST -> tts.tencentcloudapi.com
 *     4. 解析响应 JSON，提取 base64 编码的音频数据
 *     5. base64 解码 -> 写入 WAV 文件
 *
 * 依赖：
 *   - libcurl（已有）
 *   - libssl / libcrypto（OpenSSL，Debian 标配）
 */

#define TTS_SERVICE   "tts"
#define TTS_VERSION   "2019-08-23"
#define TTS_ENDPOINT  "tts.tencentcloudapi.com"
#define TTS_REGION    "ap-guangzhou"

/* ==================================================================
 * 全局状态
 * ================================================================ */

static struct {
    ai_tts_config_t cfg;
    bool inited;
} s_tts = {0};

static int s_seq = 0;

/* ==================================================================
 * Base64 编码/解码
 * ================================================================ */

static int b64_char_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *base64_decode(const char *in, size_t *out_len)
{
    size_t in_len = strlen(in);
    if (in_len == 0) { *out_len = 0; return NULL; }

    size_t pad = 0;
    if (in_len >= 1 && in[in_len-1] == '=') pad++;
    if (in_len >= 2 && in[in_len-2] == '=') pad++;

    size_t out_sz = in_len / 4 * 3 - pad;
    unsigned char *out = malloc(out_sz + 1);
    if (!out) { *out_len = 0; return NULL; }

    size_t i, j = 0;
    for (i = 0; i + 4 <= in_len; i += 4) {
        int a = b64_char_val(in[i]);
        int b = b64_char_val(in[i+1]);
        int c = b64_char_val(in[i+2]);
        int d = b64_char_val(in[i+3]);
        if (a < 0 || b < 0) break;
        unsigned int val = ((unsigned int)a << 18)
                         | ((unsigned int)b << 12)
                         | ((c >= 0) ? ((unsigned int)c << 6) : 0)
                         | ((d >= 0) ? (unsigned int)d : 0);
        out[j++] = (unsigned char)(val >> 16);
        if (c >= 0) out[j++] = (unsigned char)(val >> 8);
        if (d >= 0) out[j++] = (unsigned char)val;
    }
    *out_len = j;
    return out;
}

/* ==================================================================
 * SHA256 工具
 * ================================================================ */

static char *sha256_hex(const char *data, size_t len)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)data, len, hash);

    static const char hex[] = "0123456789abcdef";
    char *out = malloc(65);
    if (!out) return NULL;
    for (int i = 0; i < 32; i++) {
        out[i*2]     = hex[hash[i] >> 4];
        out[i*2 + 1] = hex[hash[i] & 0xf];
    }
    out[64] = '\0';
    return out;
}

static void hmac_sha256(const unsigned char *key, size_t key_len,
                        const unsigned char *data, size_t data_len,
                        unsigned char *out)
{
    unsigned int out_len = SHA256_DIGEST_LENGTH;
    HMAC(EVP_sha256(), key, (int)key_len,
         data, (int)data_len, out, &out_len);
}

/* ==================================================================
 * TC3-HMAC-SHA256 签名
 * ================================================================ */

static char *tc3_sign(const char *secret_id, const char *secret_key,
                      const char *date_str, const char *timestamp_str,
                      const char *payload)
{
    /* 1. CanonicalRequest */
    char *hashed_payload = sha256_hex(payload, strlen(payload));
    if (!hashed_payload) return NULL;

    char canonical_headers[512];
    snprintf(canonical_headers, sizeof(canonical_headers),
             "content-type:application/json\nhost:%s\n", TTS_ENDPOINT);

    const char *signed_headers = "content-type;host";

    char canonical_request[8192];
    snprintf(canonical_request, sizeof(canonical_request),
             "POST\n/\n\n%s\n%s\n%s",
             canonical_headers, signed_headers, hashed_payload);
    free(hashed_payload);

    /* 2. StringToSign */
    char *hashed_cr = sha256_hex(canonical_request, strlen(canonical_request));
    if (!hashed_cr) return NULL;

    char credential_scope[256];
    snprintf(credential_scope, sizeof(credential_scope),
             "%s/%s/tc3_request", date_str, TTS_SERVICE);

    char string_to_sign[8192];
    snprintf(string_to_sign, sizeof(string_to_sign),
             "TC3-HMAC-SHA256\n%s\n%s\n%s",
             timestamp_str, credential_scope, hashed_cr);
    free(hashed_cr);

    /* 3. SigningKey: HMAC(HMAC(HMAC("TC3"+sk, date), service), "tc3_request") */
    unsigned char hmac_date[32], hmac_service[32], signing_key[32];

    char tc3_key[256];
    snprintf(tc3_key, sizeof(tc3_key), "TC3%s", secret_key);

    hmac_sha256((unsigned char *)tc3_key, strlen(tc3_key),
                (unsigned char *)date_str, strlen(date_str), hmac_date);
    hmac_sha256(hmac_date, 32,
                (unsigned char *)TTS_SERVICE, strlen(TTS_SERVICE), hmac_service);
    hmac_sha256(hmac_service, 32,
                (unsigned char *)"tc3_request", 11, signing_key);

    /* 4. Signature */
    unsigned char signature[32];
    hmac_sha256(signing_key, 32,
                (unsigned char *)string_to_sign, strlen(string_to_sign), signature);

    /* 5. Hex encode signature */
    char sig_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(sig_hex + i*2, 3, "%02x", signature[i]);

    /* 6. Authorization header */
    char *auth = malloc(2048);
    if (!auth) return NULL;
    snprintf(auth, 2048,
        "TC3-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s",
        secret_id, credential_scope, signed_headers, sig_hex);
    return auth;
}

/* ==================================================================
 * HTTP 响应缓存
 * ================================================================ */

struct resp_buf {
    char  *data;
    size_t len;
    size_t cap;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct resp_buf *buf = (struct resp_buf *)userdata;
    size_t total = size * nmemb;

    if (buf->len + total + 1 > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 4096;
        while (buf->len + total + 1 > new_cap)
            new_cap *= 2;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* ==================================================================
 * JSON 响应解析：提取 "Audio" 字段的 base64 数据
 * ================================================================ */

static char *extract_audio_b64(const char *json)
{
    const char *key = "\"Audio\":\"";
    const char *start = strstr(json, key);
    if (!start) return NULL;
    start += strlen(key);

    const char *end = start;
    while (*end && *end != '"') end++;
    if (end <= start) return NULL;

    size_t len = (size_t)(end - start);
    char *b64 = malloc(len + 1);
    if (!b64) return NULL;
    memcpy(b64, start, len);
    b64[len] = '\0';
    return b64;
}

/* ==================================================================
 * JSON 字符串转义
 * ================================================================ */

/* 把字符串转义成可嵌入 JSON "..." 的形式，返回 malloc 的缓冲区 */
static char *json_escape(const char *src)
{
    if (!src) return strdup("");

    /* 最多每个字符变成 6 字节（\uXXXX），预分配 */
    size_t cap = strlen(src) * 6 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;

    size_t j = 0;
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  out[j++] = '\\'; out[j++] = '"';  break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
            case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
            case '\t': out[j++] = '\\'; out[j++] = 't';  break;
            default:
                if (c < 0x20) {
                    /* 其他控制字符 → \u00xx */
                    int n = snprintf(out + j, cap - j, "\\u%04x", c);
                    if (n > 0) j += (size_t)n;
                } else {
                    out[j++] = c;
                }
                break;
        }
    }
    out[j] = '\0';
    return out;
}

/* ==================================================================
 * 接口实现
 * ================================================================ */

err_t ai_tts_init(const ai_tts_config_t *cfg)
{
    if (!cfg) return ERR_INVAL;
    s_tts.cfg = *cfg;
    if (!s_tts.cfg.output_dir) s_tts.cfg.output_dir = "/tmp/speaker";

    /* 确保输出目录存在 */
    mkdir(s_tts.cfg.output_dir, 0755);

    s_tts.inited = true;
    LOG_INFO("TTS initialized (voice_type=%d)", s_tts.cfg.voice_type);
    return ERR_OK;
}

void ai_tts_deinit(void)
{
    s_tts.inited = false;
    LOG_INFO("TTS deinitialized");
}

err_t ai_tts_speak(const char *text, char **out_path)
{
    if (!s_tts.inited || !text || !out_path) return ERR_INVAL;
    if (text[0] == '\0') return ERR_INVAL;

    /* 构建请求体 JSON */
    int seq = __sync_fetch_and_add(&s_seq, 1);

    char *escaped_text = json_escape(text);
    if (!escaped_text) return ERR_NOMEM;

    char session_id[64];
    snprintf(session_id, sizeof(session_id), "speaker-%d-%d",
             (int)time(NULL), seq);

    char payload[4096];
    int payload_len = snprintf(payload, sizeof(payload),
        "{\"Text\":\"%s\","
        "\"SessionId\":\"%s\","
        "\"ModelType\":1,"
        "\"VoiceType\":%d,"
        "\"Codec\":\"wav\","
        "\"Volume\":5,"
        "\"Speed\":1.0,"
        "\"ProjectId\":0}",
        escaped_text, session_id, s_tts.cfg.voice_type);
    free(escaped_text);

    if (payload_len >= (int)sizeof(payload) - 1) {
        LOG_ERROR("TTS: text too long (%d chars)", (int)strlen(text));
        return ERR_INVAL;
    }

    /* 时间戳（UTC） */
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    char timestamp_str[16], date_str[16];
    snprintf(timestamp_str, sizeof(timestamp_str), "%lld", (long long)now);
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", gmt);

    /* 计算 TC3 签名 */
    char *auth_header = tc3_sign(s_tts.cfg.secret_id, s_tts.cfg.secret_key,
                                 date_str, timestamp_str, payload);
    if (!auth_header) {
        LOG_ERROR("TTS: signature generation failed");
        return ERR_GENERAL;
    }

    /* 构建 URL */
    char url[256];
    snprintf(url, sizeof(url), "https://%s", TTS_ENDPOINT);

    /* HTTP 请求 */
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("TTS: curl_easy_init failed");
        free(auth_header);
        return ERR_NOMEM;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    char auth_hdr[2048];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: %s", auth_header);
    headers = curl_slist_append(headers, auth_hdr);

    char xtc_ts[64], xtc_ver[64], xtc_reg[64];
    snprintf(xtc_ts, sizeof(xtc_ts), "X-TC-Timestamp: %s", timestamp_str);
    snprintf(xtc_ver, sizeof(xtc_ver), "X-TC-Version: %s", TTS_VERSION);
    snprintf(xtc_reg, sizeof(xtc_reg), "X-TC-Region: %s", TTS_REGION);
    headers = curl_slist_append(headers, "X-TC-Action: TextToVoice");
    headers = curl_slist_append(headers, xtc_ts);
    headers = curl_slist_append(headers, xtc_ver);
    headers = curl_slist_append(headers, xtc_reg);

    struct resp_buf resp = {0};
    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)strlen(payload));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,     30000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "AI-Speaker/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    LOG_INFO("TTS: requesting Tencent Cloud...");
    CURLcode rc = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    free(auth_header);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || http_code != 200) {
        LOG_ERROR("TTS: HTTP %ld, curl=%d, resp=%.512s",
                  http_code, rc, resp.data ? resp.data : "(null)");
        free(resp.data);
        return ERR_GENERAL;
    }

    /* 解析响应 */
    char *audio_b64 = extract_audio_b64(resp.data ? resp.data : "");
    free(resp.data);

    if (!audio_b64) {
        LOG_ERROR("TTS: no Audio field in response");
        return ERR_GENERAL;
    }

    /* Base64 解码 */
    size_t audio_len = 0;
    unsigned char *audio_data = base64_decode(audio_b64, &audio_len);
    free(audio_b64);

    if (!audio_data || audio_len == 0) {
        LOG_ERROR("TTS: base64 decode failed");
        free(audio_data);
        return ERR_GENERAL;
    }

    /* 写入 WAV 文件 */
    char wav_path[1024];
    snprintf(wav_path, sizeof(wav_path), "%s/tts_%06d.wav",
             s_tts.cfg.output_dir, seq);

    FILE *fp = fopen(wav_path, "wb");
    if (!fp) {
        LOG_ERROR("TTS: cannot write %s", wav_path);
        free(audio_data);
        return ERR_NODEV;
    }

    size_t written = fwrite(audio_data, 1, audio_len, fp);
    fclose(fp);

    if (written != audio_len) {
        LOG_ERROR("TTS: wrote %zu/%zu bytes", written, audio_len);
        unlink(wav_path);
        free(audio_data);
        return ERR_GENERAL;
    }
    free(audio_data);

    LOG_INFO("TTS audio saved: %s (%zu bytes)", wav_path, audio_len);
    *out_path = strdup(wav_path);
    return ERR_OK;
}
