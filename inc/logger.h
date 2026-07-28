#ifndef LOGGER_H
#define LOGGER_H
#include "common.h"

err_t logger_init(log_level_t level, const char *file_path);
void logger_deinit(void);
void logger_set_level(log_level_t level);
void logger_log(log_level_t level, const char *file, int line,const char *func, const char *fmt, ...);

  #define LOG_DEBUG(fmt, ...)  logger_log(LOG_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
  #define LOG_INFO(fmt, ...)   logger_log(LOG_INFO,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
  #define LOG_WARN(fmt, ...)   logger_log(LOG_WARN,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
  #define LOG_ERROR(fmt, ...)  logger_log(LOG_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#endif // LOGGER_H