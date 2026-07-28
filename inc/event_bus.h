#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include "common.h"
/*
 * 事件总线 — 模块间解耦通信
 *
 * 设计要点：
 * - 发布/订阅模型，发布者不关心谁在订阅
 * - 支持通配订阅（订阅某类事件）
 * - 异步事件队列（发布者不阻塞）
 * - 事件数据由事件总线统一管理生命周期
 *
 * 线程安全：是（内部mutex保护）
 */
typedef struct subscription subscription_t;

err_t event_bus_init(size_t queue_capacity);
void event_bus_deinit(void);
/* 停止事件循环（不释放资源），搭配 pthread_join + event_bus_deinit 使用 */
void event_bus_stop(void);

subscription_t *event_subscribe(event_id_t event_id,event_handler_t handler,void *user_data);

void event_unsubscribe(subscription_t *sub);

err_t event_publish(event_id_t event_id,void *data,size_t data_size,void(*destroy)(void *));
err_t event_publish_string(event_id_t event_id, const char *str);
static inline err_t event_publish_simple(event_id_t id) {
      return event_publish(id, NULL, 0, NULL);
}
void event_dispatch_loop(void);
#endif /* EVENT_BUS_H */