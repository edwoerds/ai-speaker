  #include "common.h"
  #include "event_bus.h"
  #include "state_machine.h"
  #include "thread_pool.h"
  #include "module.h"
  #include "logger.h"
  #include "config.h"
  
  #include <signal.h>
  #include <stdio.h>
  #include <stdlib.h>
  #include <string.h>
  #include <unistd.h>
  #include <pthread.h>

  config_t *g_config=NULL;  /* 非 static，供 voice_agent 等模块 extern 引用 */
  static pthread_t g_event_thread={0};
  static volatile sig_atomic_t g_running=1;

  static void signal_handler(int sig){
    (void)sig;
    g_running=0;
  }
  static void setup_signal_handlers(void){
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler=signal_handler;   
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE,SIG_IGN);
  }

  static void *event_loop_thread(void *arg){
    (void)arg;
    //线程初始化
    LOG_INFO("Event loop thread started");
    event_dispatch_loop();
    LOG_INFO("Event loop thread exited");
    return NULL;
  }

  static const char *parse_args(int argc, char *argv[]){
    const char *cfg_path="/etc/speaker.conf";
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i], "-c") == 0 && i+1 < argc){
            cfg_path=argv[++i];
        }else if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0){
            printf("Usage: %s [-c config_file]\n", argv[0]);
            exit(0);}
        else if(strcmp(argv[i],"-v")==0||strcmp(argv[i], "--version")==0){
            printf("%s %s\n", PROJECT_NAME, PROJECT_VERSION);
            exit(0);
        }
    }
    return cfg_path;
  }

  /* stdin 输入线程 — 用于调试，模拟蓝牙 SPP 消息 */
  static void *stdin_thread(void *arg) {
      (void)arg;
      char line[1024];
      while (fgets(line, sizeof(line), stdin)) {
          size_t len = strlen(line);
          while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
              line[--len] = '\0';
          if (len == 0) continue;
          LOG_INFO("[stdin] %s", line);
          /* 调试命令 */
          extern err_t alsa_capture_start(void);
          extern void alsa_capture_stop(void);
          if (strcmp(line, "record") == 0) { alsa_capture_start(); continue; }
          if (strcmp(line, "stop") == 0)   { alsa_capture_stop();  continue; }
          event_publish_string(EV_BT_DATA_RECEIVED, line);
      }
      g_running = false;
      return NULL;
  }
int main(int argc,char *argv[]){
    //1.命令行解析
    const char *cfg_path=parse_args(argc,argv);
    //2.初始化日至
    logger_init(LOG_DEBUG,NULL);
    LOG_INFO("%s v%s starting...", PROJECT_NAME, PROJECT_VERSION);
    //3.加载配置
    g_config=config_load(cfg_path);
    if(g_config){
        const char *level = config_get_str(g_config, "log", "level", "info");
        if      (strcmp(level, "debug") == 0) logger_set_level(LOG_DEBUG);
        else if (strcmp(level, "info")  == 0) logger_set_level(LOG_INFO);
        else if (strcmp(level, "warn")  == 0) logger_set_level(LOG_WARN);
        else if (strcmp(level, "error") == 0) logger_set_level(LOG_ERROR);
    }
    //4.信号处理
    setup_signal_handlers();
    //5.模块初始化
    err_t err=module_init_all();
    if(IS_ERR(err)){
        LOG_ERROR("Module init failed");
        config_free(g_config);
        logger_deinit();
        return EXIT_FAILURE;
    }
    //6.启动事件循环线程
    pthread_create(&g_event_thread, NULL, event_loop_thread, NULL);
    //7.启动 stdin 输入线程（调试用）
    pthread_t stdin_tid;
    pthread_create(&stdin_tid, NULL, stdin_thread, NULL);
    //8.主循环等待信号
    LOG_INFO("%s running. Press Ctrl+C to stop.", PROJECT_NAME);
    while (g_running) pause();
    //9.优雅关闭
    LOG_INFO("Shutting down...");
    module_shutdown_all();
    event_bus_stop();                  // 唤醒事件循环 → 处理完队列后退出
    pthread_join(g_event_thread, NULL);// 等事件线程安全退出
    event_bus_deinit();                // 再释放事件总线资源
    module_deinit_all();
    config_free(g_config);
    logger_deinit();

    printf("\n%s stopped.\n", PROJECT_NAME);
    return EXIT_SUCCESS;
}