  #include "ai_client.h"        // AI 客户端的类型和函数
  #include "logger.h"           // 日志


  #include <stdio.h>             // sprintf
  #include <stdlib.h>            // malloc/free
  #include <string.h>            // strdup

  /*
   * 对话管理器 — AI 模块的核心编排器
   *
 * 职责：
 *  1. 维护多轮对话历史（保留 system prompt）
 *  2. 构建 API JSON payload
 *  3. 自动裁剪超长历史
   */

   //全局状态 s_conv

static struct {
    bool          inited;
    conv_entry_t  history[CONV_MAX_TURNS + 1];  /* [0]=system, [1..]=user/assistant */
    int           count;                         /* 当前消息数 */
    char         *system_prompt;
} s_conv = {0};



    /* ==================================================================
   * 对话历史管理
   * ================================================================ */
  //ai_conv_init
err_t ai_conv_init(const char *system_prompt)
{
    if (s_conv.inited) return ERR_OK;

    s_conv.system_prompt = strdup(system_prompt ? system_prompt :
        "你是一个智能音箱助手。请用简洁、清晰的中文回答问题。"
        "保持回答在100字以内，除非用户要求详细说明。");
    if (!s_conv.system_prompt) return ERR_NOMEM;

    s_conv.history[0].role    = strdup("system");
    s_conv.history[0].content = strdup(s_conv.system_prompt);
    s_conv.count = 1;

    s_conv.inited = true;
    LOG_INFO("Conv manager initialized");
    return ERR_OK;
}

  //ai_conv_deinit

  void ai_conv_deinit(void){
    if(!s_conv.inited)
        return;
    for(int i=0;i<s_conv.count;i++){
        free(s_conv.history[i].role);
        free(s_conv.history[i].content);
        //释放历史中每条消息的 role 和 content
        //history[0].role — 在 ai_conv_init 里 strdup("system") 分配的
        //history[0].content — 在 ai_conv_init 里 strdup(system_prompt) 分配的
    }
    s_conv.count=0;
    free(s_conv.system_prompt);
    s_conv.system_prompt=NULL;
    //置 NULL 防止 double free——如果别人无意中再次调 deinit，free(NULL) 是安全的
    //释放累积缓冲区。一样置 NULL、置零。
    s_conv.inited=false;
    LOG_INFO("Conversation manager deinitialized");
  }

  //ai_conv_append

  err_t ai_conv_append(const char *role,const char *content){
    if(!s_conv.inited||!role||!content)
        return ERR_INVAL;
    //裁减超长内容
    if(strlen(content)>CONV_MAX_CONTENT){
        //截断
        char *trunc =strdup(content);
        if(!trunc) 
            return ERR_NOMEM;
        trunc[CONV_MAX_CONTENT - 4] = '.';
        trunc[CONV_MAX_CONTENT - 3] = '.';
        trunc[CONV_MAX_CONTENT - 2] = '.';
        trunc[CONV_MAX_CONTENT - 1] = '\0';
        content = trunc;
    }else{
        content=strdup(content);
        if(!content) return ERR_NOMEM;
    }
    //  关键设计：不管是截断还是直接复制，content 都被重新赋值为 strdup 的返回值。这意味着：
    //1. 调用方传进来的原始字符串不会被修改（因为我们在副本上操作）
    //2. 后面我们 free 时，free 的是副本，不影响调用方

    //历史满了，删除最旧的user+assistant对
    if(s_conv.count>=CONV_MAX_TURNS+1){
        //保留 system prompt (index 0)删除index 1和2;
        for(int i=1;i<=2&& i < s_conv.count;i++){
            free(s_conv.history[i].role);
            free(s_conv.history[i].content);
        }
        //将后面的项向前移动一位
        int shift=2;
        for(int i=1;i+shift<s_conv.count;i++){
            s_conv.history[i]=s_conv.history[i+shift];
        }
        s_conv.count-=shift;
        //为什么删一对而不是一条？因为对话历史是成对出现的（一问一答），删一条会导致 "user → assistant → user" 的交替顺序被打乱。删一对保证历史始终是完整的user/assistant 对。
    }
    //追加
  int idx = s_conv.count;
  char *role_copy = strdup(role);
  if (!role_copy) {
      free((void*)content);  // 回滚 content 的 strdup
      return ERR_NOMEM;
  }
  s_conv.history[idx].role = role_copy;
  s_conv.history[idx].content = (char*)content;  // 用 content_copy
  s_conv.count++;  // 所有分配成功后才自增

    LOG_DEBUG("Conv append [%s]: %s", role, content);
    return ERR_OK;
  }

  //ai_conv_bulid_payload
  //核心函数，把对话历史拼成发给 AI 的 JSON 字符串。
  char *ai_conv_build_payload(void){
    if(!s_conv.inited||s_conv.count<1) return NULL;
    //估算大小：固定结构+每条消息
    size_t est =256;
    //JSON 的固定结构骨架（{"model":"...","stream":true,...,"messages":[）大约占这么多
    for(int i=0;i<s_conv.count;i++){
        est+=strlen(s_conv.history[i].role)+strlen(s_conv.history[i].content)+64;
        //每条消息的内容 + role 字符串长度，再加 64 字节的 JSON 包装（{"role":"...","content":"..."},\n）
    }
    char *buf=malloc(est);
    if(!buf) return NULL;

    int pos = snprintf(buf, est,
        "{\n"
        "  \"model\": \"deepseek-v4-flash\",\n"
        "  \"stream\": false,\n"
        "  \"temperature\": 0.7,\n"
        "  \"max_tokens\": 2048,\n"
        "  \"messages\": [\n");
        for(int i=0;i<s_conv.count;i++){
            int remaining=(int)est-pos;
            if(remaining<128){
                est*=2;
                char *new_buf=realloc(buf,est);
                if(!new_buf){
                    free(buf);
                    return NULL;
                }
                ///* realloc 保留旧内容，pos 字节偏移不变 */
                buf=new_buf;
                remaining=(int)est-pos;
            }
            //JSON 转义 content 中的 " 和 \ *
            //这里的 " 会被解析器当成 JSON 字符串的结束符，导致 JSON 格式错误
            //如果是 " 或 \，先写一个 \ 再写原字符（在 JSON 里变成 \" 或 \\） 其他字符直接写
            char *escaped=NULL;
            size_t esc_len=0;
            FILE*esc_stream= open_memstream(&escaped, &esc_len);
            // open_memstream 是什么？ 一个 GNU 扩展函数，它：
            //1. 创建一个内存流（类似文件，但数据存在内存里）
            //2. 通过 fputc、fprintf 等 FILE 函数写入
            //3. 关闭流时，自动 malloc 一块内存存所有写入的数据
            //4. 把地址写到 escaped 指针，长度写到 esc_len
            if(esc_stream){
                const char *src=s_conv.history[i].content;
                while(*src){
                    if(*src=='"' || *src == '\\') {
                        fputc('\\', esc_stream);
                        fputc(*src, esc_stream);
                    } else if (*src == '\n') {
                        fputs("\\n", esc_stream);
                    } else if (*src == '\r') {
                        fputs("\\r", esc_stream);
                    } else if (*src == '\t') {
                        fputs("\\t", esc_stream);
                    } else if ((unsigned char)*src < 0x20) {
                        /* 其他控制字符转义为 \u00xx */
                        char hex[8];
                        snprintf(hex, sizeof(hex), "\\u%04x", (unsigned char)*src);
                        fputs(hex, esc_stream);
                    } else {
                        fputc(*src, esc_stream);
                    }
                    src++;
                }
                fclose(esc_stream);
            }
            pos+=snprintf(buf+pos,remaining,"    {\"role\": \"%s\", \"content\": \"%s\"}%s\n",
              s_conv.history[i].role,
              escaped ? escaped : s_conv.history[i].content,
              (i < s_conv.count - 1) ? "," : "");

          free(escaped);
        }
            pos += snprintf(buf + pos, (size_t)(est - pos),
        "  ]\n"
        "}\n");

    LOG_DEBUG("Built payload (%d bytes, %d messages)", pos, s_conv.count);
    return buf;
  }

  //ai_conv_clear
  void ai_conv_clear(void){
    if(!s_conv.inited) return;
    for(int i=1;i<s_conv.count;i++){
        free(s_conv.history[i].role);
        free(s_conv.history[i].content);
    }
    s_conv.count=1;//只保留system prompt
    LOG_INFO("Conversation history cleared");
  }
   
