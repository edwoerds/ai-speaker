  #include "config.h"
  #include "logger.h"
  #include <ctype.h>
  #include <stdio.h>
  #include <stdlib.h>
  #include <string.h>

  /* ======================== 内部数据结构 ======================== */

  /* 一个键值对：key = value */
  typedef struct {
      char *key;
      char *value;
  } kv_pair_t;

  /* 一个配置节：[section] 下面的所有键值对 */
  typedef struct {
      char       *name;
      kv_pair_t  *pairs;
      int         count;
      int         capacity;
  } section_t;

  /* config_t 的真实结构 */
  struct config {
      section_t  *sections;    /* 所有配置节的数组 */
      int         count;       /* 当前节数 */
      int         capacity;    /* 数组容量 */
      char       *file_path;   /* 配置文件的路径，用于重新加载 */
  };

  /* ======================== 内部工具函数 ======================== */

  /* 去掉字符串两端的空白字符（空格、tab、换行） */
  static char *trim(char *s)
  {
      while (isspace((unsigned char)*s)) s++;
      if (*s == '\0') return s;
      char *end = s + strlen(s) - 1;
      while (end > s && isspace((unsigned char)*end)) end--;
      *(end + 1) = '\0';
      return s;
  }

  /* 在已有配置中按名字查找 section，找不到返回 NULL */
  static section_t *find_section(config_t *cfg, const char *name)
  {
      for (int i = 0; i < cfg->count; i++)
          if (strcmp(cfg->sections[i].name, name) == 0)
              return &cfg->sections[i];
      return NULL;
  }

  /* 新增一个 section（容量不够时自动扩容） */
  static section_t *add_section(config_t *cfg, const char *name)
  {
      if (cfg->count >= cfg->capacity) {
          cfg->capacity = cfg->capacity ? cfg->capacity * 2 : 8;
          cfg->sections = realloc(cfg->sections,
                                  cfg->capacity * sizeof(section_t));
      }
      section_t *sec = &cfg->sections[cfg->count++];
      sec->name     = strdup(name);
      sec->pairs    = NULL;
      sec->count    = 0;
      sec->capacity = 0;
      return sec;
  }

  /* 在 section 中添加一个键值对 */
  static void add_pair(section_t *sec, const char *key, const char *val)
  {
      if (sec->count >= sec->capacity) {
          sec->capacity = sec->capacity ? sec->capacity * 2 : 8;
          sec->pairs = realloc(sec->pairs, sec->capacity * sizeof(kv_pair_t));
      }
      sec->pairs[sec->count].key   = strdup(key);
      sec->pairs[sec->count].value = strdup(val);
      sec->count++;
  }

  /* ======================== 接口实现 ======================== */

  config_t *config_load(const char *path)
  {
      config_t *cfg = calloc(1, sizeof(config_t));
      cfg->file_path = strdup(path);

      FILE *fp = fopen(path, "r");
      if (!fp) {
          LOG_WARN("Config not found: %s (using defaults)", path);
          return cfg;    /* 文件不存在不崩溃，所有 get_* 走默认值 */
      }

      char line[512];
      section_t *cur = NULL;    /* 当前正在处理的 section */

      while (fgets(line, sizeof(line), fp)) {
          char *s = trim(line);
          if (*s == '\0' || *s == '#') continue;    /* 空行或注释跳过 */

          /* [section] — 切换到新节 */
          if (*s == '[') {
              char *end = strchr(s, ']');
              if (!end) continue;                    /* 格式错误，跳过 */
              *end = '\0';
              char *name = trim(s + 1);
              cur = find_section(cfg, name);
              if (!cur) cur = add_section(cfg, name);
              continue;
          }

          /* key = value — 解析键值对 */
          char *eq = strchr(s, '=');
          if (!eq) continue;                         /* 没有等号，跳过 */
          *eq = '\0';
          char *key   = trim(s);
          char *value = trim(eq + 1);

          if (!cur) cur = add_section(cfg, "global");  /* 没 [section] 时归到 global */
          add_pair(cur, key, value);
      }

      fclose(fp);
      LOG_INFO("Config loaded: %s (%d sections)", path, cfg->count);
      return cfg;
  }

  void config_free(config_t *cfg)
  {
      if (!cfg) return;
      for (int i = 0; i < cfg->count; i++) {
          free(cfg->sections[i].name);
          for (int j = 0; j < cfg->sections[i].count; j++) {
              free(cfg->sections[i].pairs[j].key);
              free(cfg->sections[i].pairs[j].value);
          }
          free(cfg->sections[i].pairs);
      }
      free(cfg->sections);
      free(cfg->file_path);
      free(cfg);
  }

  const char *config_get_str(const config_t *cfg, const char *section,
                             const char *key, const char *def)
  {
      if (!cfg) return def;
      section_t *sec = find_section((config_t *)cfg, section);
      if (!sec) return def;
      for (int i = 0; i < sec->count; i++)
          if (strcmp(sec->pairs[i].key, key) == 0)
              return sec->pairs[i].value;
      return def;
  }

  int config_get_int(const config_t *cfg, const char *section,
                     const char *key, int def)
  {
      const char *s = config_get_str(cfg, section, key, NULL);
      if (!s) return def;
      char *end = NULL;
      long val = strtol(s, &end, 10);
      if (end == s || *end != '\0') return def;    /* 转换失败 */
      return (int)val;
  }

  bool config_get_bool(const config_t *cfg, const char *section,
                       const char *key, bool def)
  {
      const char *s = config_get_str(cfg, section, key, NULL);
      if (!s) return def;
      return (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 ||
              strcmp(s, "1")   == 0 || strcmp(s, "on")  == 0);
  }