#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "common.h"
/* 任务函数类型：你要线程池干什么事 */
typedef void (*task_func_t)(void *arg);

err_t thread_pool_init(int num_workers, size_t queue_size);
void thread_pool_deinit(void);

  /* 提交一个任务，让线程池在后台执行 */
task_id_t thread_pool_submit(task_func_t func, void *arg, const char *name);

#endif /* THREAD_POOL_H */