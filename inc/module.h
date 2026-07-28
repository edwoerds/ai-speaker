#ifndef MODULE_H
#define MODULE_H

#include "common.h"
//模块优先级
typedef enum{
      MODULE_PRIO_FRAMEWORK   = 0,   // event_bus, thread_pool
      MODULE_PRIO_SERVICE     = 10,  // bt, audio, ai
      MODULE_PRIO_APPLICATION = 20,  // 主状态机
}module_prio_t;
//模块定义结构
typedef struct {
    const char* name;
    err_t    (*init)(void);
    void     (*deinit)(void);
    void     (*shutdown)(void);
    int       priority;
} module_def_t;

err_t module_init_all(void);
void module_shutdown_all(void);
void module_deinit_all(void);
void module_register(const module_def_t *def);
  /* 宏：在源文件中定义并注册一个模块 */
#define MODULE_DEFINE(_name, _init, _deinit, _shutdown, _prio)           \
    static void __attribute__((constructor)) _reg_##_name(void)          \
    {                                                                    \
        static const module_def_t _def = {                               \
            .name     = #_name,                                          \
            .init     = _init,                                           \
            .deinit   = _deinit,                                         \
            .shutdown = _shutdown,                                       \
            .priority = _prio,                                           \
        };                                                               \
        module_register(&_def);                                          \
    }

#endif /* MODULE_H */