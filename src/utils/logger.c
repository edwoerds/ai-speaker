  #include "logger.h"       // 自己的接口
  #include <pthread.h>       // mutex
  #include <stdarg.h>        // va_list, va_start, va_end
  #include <stdio.h>         // FILE, fprintf, stderr
  #include <string.h>        // strrchr
  #include <time.h>          // localtime_r, strftime
  #include <sys/time.h>      // gettimeofday

static struct {
    FILE           *fp;       // 文件指针，NULL=只输出stderr
    log_level_t     level;    // 当前日志级别，低于这个的不输出
    pthread_mutex_t lock;     // 多线程写日志时互斥
    bool            inited;   // 是否已初始化
} s_log = {0};

  /* 级别标签 */
static const char *s_level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};
/* 颜色（ANSI 转义码） */
static const char *s_color[] = {
    "\033[90m",   /* DEBUG */
    "\033[32m",   /* INFO  */
    "\033[33m",   /* WARN  */
    "\033[31m",   /* ERROR */
};

err_t  logger_init(log_level_t level, const char *file_path)
{
    if(s_log.inited) return ERR_OK;
    pthread_mutex_init(&s_log.lock,NULL);
    s_log.level = level;
    if(file_path) s_log.fp=fopen(file_path,"a");
    s_log.inited=true;
    return ERR_OK;
}

void logger_set_level(log_level_t level)
{
    pthread_mutex_lock(&s_log.lock);
    s_log.level = level;
    pthread_mutex_unlock(&s_log.lock);
}

void logger_deinit(void){
    if(!s_log.inited) return;
    s_log.inited=false;
    if(s_log.fp) {fclose(s_log.fp); s_log.fp=NULL;}
    pthread_mutex_destroy(&s_log.lock);
}

void logger_log(log_level_t level,const char *file,int line,const char *func,const char *fmt,...)
{
    if(!s_log.inited || level<s_log.level) return;
    struct timeval tv;
    struct tm tm;
    char timebuf[20];
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec,&tm);
    strftime(timebuf,sizeof(timebuf),"%H:%M:%S", &tm);
    const char *base=file ? strrchr(file, '/') : NULL;
    base =base?base+1:(file ? file :"?");
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr,"%s%s [%s] %s:%d|%s: %s\033[0m\n",s_color[level], timebuf, s_level_str[level],base, line, func, msg);
    if(s_log.fp){fprintf(s_log.fp, "%s [%s] %s:%d|%s: %s\n", timebuf, s_level_str[level], base, line, func, msg);
        fflush(s_log.fp);
    }
}