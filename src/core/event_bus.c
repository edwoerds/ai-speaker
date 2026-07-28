  #include "event_bus.h"
  #include "module.h"
  #include <pthread.h>
  #include <stdlib.h>
  #include <string.h>
  #include <time.h>
  #include "logger.h"
  //连表指针，把订阅者串起来
  typedef struct subscription {
      struct subscription *next;
      event_id_t           event_id;
      event_handler_t      handler;
      void                *user_data;
  } subscription_t;

  //事件队列结构——环形缓冲区：
  typedef struct {
      event_t        *buffer;    /* 环形缓冲区 */
      size_t          capacity;  /* 容量 */
      size_t          head;      /* 读位置 */
      size_t          tail;      /* 写位置 */
      size_t          count;     /* 当前事件数 */
      pthread_mutex_t lock;
      pthread_cond_t  not_empty; /* 队列非空条件变量 */
      pthread_cond_t  not_full;  /* 队列非满条件变量 */
      bool            stopped;   /* 是否已停止 */
  } event_queue_t;

  //全局状态
  static struct {
      subscription_t *subs;        /* 订阅链表 */
      pthread_mutex_t subs_lock;  /* 订阅操作锁 */
      event_queue_t   queue;
      bool            inited;
  } s_ebus = {0};


  //队列

  //初始化队列
  static err_t queue_init(event_queue_t *q, size_t capacity){
      q->buffer=calloc(capacity,sizeof(event_t));
      if(!q->buffer) return ERR_NOMEM;
      q->capacity=capacity;
      q->head=q->tail=q->count=0;
      q->stopped=false;
      pthread_mutex_init(&q->lock, NULL);
      pthread_cond_init(&q->not_empty, NULL);
      pthread_cond_init(&q->not_full, NULL);
      return ERR_OK;
  }

  //摧毁队列
  static void queue_deinit(event_queue_t *q){
      pthread_mutex_lock(&q->lock);
      q->stopped=true;
      pthread_cond_broadcast(&q->not_empty);
      pthread_cond_broadcast(&q->not_full);
      pthread_mutex_unlock(&q->lock);
      //释放所有未处理事件的数据
      while(q->count>0){
          event_t *ev=&q->buffer[q->head];
          if(ev->data && ev->data_destroy)
              ev->data_destroy(ev->data);
          memset(ev, 0, sizeof(event_t));
          q->head=(q->head+1)%q->capacity;
          q->count--;
      }
      free(q->buffer);
      q->buffer=NULL;
      pthread_mutex_destroy(&q->lock);
      pthread_cond_destroy(&q->not_empty);
      pthread_cond_destroy(&q->not_full);
  }

  //入队操作
  static err_t queue_push(event_queue_t *q, const event_t *ev){
      pthread_mutex_lock(&q->lock);
      ///队列满 → 立即返回 ERR_FULL，不阻塞
  if(q->count>=q->capacity||q->stopped){
      bool stopped = q->stopped;
      pthread_mutex_unlock(&q->lock);
      return stopped ? ERR_ABORTED : ERR_FULL;
  }
      q->buffer[q->tail]=*ev;
      q->tail=(q->tail+1)%q->capacity;
      q->count++;

      pthread_cond_signal(&q->not_empty);
      pthread_mutex_unlock(&q->lock);
      return ERR_OK;
  }

  //出队操作
  static err_t queue_pop(event_queue_t *q, event_t *ev){
      pthread_mutex_lock(&q->lock);
      //等待队列非空
      while(q->count==0&&!q->stopped){
          pthread_cond_wait(&q->not_empty, &q->lock);
      }
      if(q->stopped){
          pthread_mutex_unlock(&q->lock);
          return ERR_ABORTED;
      }
      *ev=q->buffer[q->head];
      memset(&q->buffer[q->head], 0, sizeof(event_t));
      q->head=(q->head+1)%q->capacity;
      q->count--;
      pthread_cond_signal(&q->not_full);
      pthread_mutex_unlock(&q->lock);
      return ERR_OK;
  }


  //时间戳获取
  static int64_t now_ms(void){
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
  }

  //事件总线接口实现
  //事件初始化
  err_t event_bus_init(size_t queue_capacity){
      if(s_ebus.inited) return ERR_OK;
      err_t err=queue_init(&s_ebus.queue,queue_capacity);
      if(IS_ERR(err)) return err;
      pthread_mutex_init(&s_ebus.subs_lock, NULL);
      s_ebus.subs=NULL;
      s_ebus.inited=true;

      LOG_INFO("Event bus initialized (queue=%zu)",queue_capacity);
      return ERR_OK;
  }
  //销毁事件总线
  void event_bus_deinit(void){
      if(!s_ebus.inited) return;
      s_ebus.inited=false;
      //停止队列->事件循环线程退出
      queue_deinit(&s_ebus.queue);
      //释放所有订阅
      pthread_mutex_lock(&s_ebus.subs_lock);
      subscription_t *node=s_ebus.subs;
      while(node){
          subscription_t *next=node->next;
          free(node);
          node=next;
      }
      s_ebus.subs=NULL;
      pthread_mutex_unlock(&s_ebus.subs_lock);
      pthread_mutex_destroy(&s_ebus.subs_lock);
      LOG_INFO("Event bus deinitialized");
  }

  //订阅事件
  subscription_t *event_subscribe(event_id_t event_id,event_handler_t handler,void *user_date){
      if(!s_ebus.inited) return NULL;
      subscription_t *node=calloc(1, sizeof(subscription_t));
      if(!node) return NULL;
      node->event_id=event_id;
      node->handler=handler;
      node->user_data=user_date;
      //头差法
      pthread_mutex_lock(&s_ebus.subs_lock);
      node->next=s_ebus.subs;
      s_ebus.subs=node;
      pthread_mutex_unlock(&s_ebus.subs_lock);
      return node;
  }

  //发布事件
  err_t event_publish(event_id_t event_id,void *data,size_t data_size,void(*destroy)(void*)){
      if (!s_ebus.inited) return ERR_GENERAL;

      event_t ev;
      ev.id=event_id;
      ev.timestamp_ms=now_ms();
      ev.data=data;
      ev.data_size=data_size;
      ev.data_destroy=destroy;

      return queue_push(&s_ebus.queue, &ev);
  }

  //事件分发循环
  void event_dispatch_loop(void){
      if(!s_ebus.inited) return;
      while(1){
          event_t ev;
          err_t err=queue_pop(&s_ebus.queue, &ev);
          if(IS_ERR(err)) break;
          /* 先收集匹配的回调，再释放锁后统一调用 */
          #define MAX_DISPATCH_HANDLERS 32
          event_handler_t handlers[MAX_DISPATCH_HANDLERS];
          void *user_datas[MAX_DISPATCH_HANDLERS];
          int n = 0;

          pthread_mutex_lock(&s_ebus.subs_lock);
          subscription_t *node = s_ebus.subs;
          while (node && n < MAX_DISPATCH_HANDLERS) {
              if (node->event_id == ev.id) {
                  handlers[n] = node->handler;
                  user_datas[n] = node->user_data;
                  n++;
              }
              node = node->next;
          }
          pthread_mutex_unlock(&s_ebus.subs_lock);

          for (int i = 0; i < n; i++) {
              handlers[i](&ev, user_datas[i]);
          }

          if(ev.data && ev.data_destroy){
              ev.data_destroy(ev.data);
          }
      }
  }
  void event_unsubscribe(subscription_t *sub){
      if(!s_ebus.inited || !sub) return;

      pthread_mutex_lock(&s_ebus.subs_lock);
      subscription_t **pp=&s_ebus.subs;//二级指针,指向当前指针的next域
      while(*pp){
          if(*pp==sub){
              *pp=sub->next;//从链表中摘除
              free(sub);
              break;
          }
          pp=&((*pp)->next);
      }
      pthread_mutex_unlock(&s_ebus.subs_lock);
  }

  /* 停止事件循环（不释放资源，等待线程退出后再 deinit） */
  void event_bus_stop(void){
      if(!s_ebus.inited) return;
      pthread_mutex_lock(&s_ebus.queue.lock);
      s_ebus.queue.stopped = true;
      pthread_cond_broadcast(&s_ebus.queue.not_empty);
      pthread_mutex_unlock(&s_ebus.queue.lock);
  }

  //辅助函数
  err_t event_publish_string(event_id_t event_id,const char *str){
      if(!str) return ERR_INVAL;
      char *dup=strdup(str);
      if(!dup) return ERR_NOMEM;
      return event_publish(event_id, dup, strlen(dup)+1, free);
  }

  /* ---- MODULE_DEFINE ---- */
  static err_t evbus_module_init(void)   { return event_bus_init(64); }
  static void   evbus_module_deinit(void){ event_bus_deinit(); }

  MODULE_DEFINE(event_bus, evbus_module_init, evbus_module_deinit,
                NULL, MODULE_PRIO_FRAMEWORK);