
  #ifndef CONFIG_H
  #define CONFIG_H

  #include"common.h"

  /*
   * 配置文件解析 — INI风格
   *
   * 支持：
   *   [section]
   *   key = value
   *   # 注释
   *   空行自动跳过
   *
   * 用法：
   *   config_t *cfg = config_load("/etc/speaker.conf");
   *   const char *s = config_get_str(cfg, "bluetooth", "device_name", "AI-Speaker");
   *   int n = config_get_int(cfg, "log", "max_size", 10485760);
   *   config_free(cfg);
   */

  /* 不透明类型：使用者只通过指针操作，不关心内部结构 */
  typedef struct config config_t;

  config_t *config_load(const char *path);
  void config_free(config_t *cfg);

  const char *config_get_str(const config_t *cfg, const char *section,
                             const char *key, const char *def);
  int config_get_int(const config_t *cfg, const char *section,
                     const char *key, int def);
  bool config_get_bool(const config_t *cfg, const char *section,
                       const char *key, bool def);

  /* 全局配置实例（定义在 main.c），供各模块读取配置 */
  extern config_t *g_config;

  #endif /* CONFIG_H */