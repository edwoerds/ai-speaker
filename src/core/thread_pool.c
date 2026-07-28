#include "thread_pool.h"
#include "module.h"
#include "logger.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

//单个任务节点
typedef struct task_node{
    struct task_node *next;
    task_id_t         id;
    task_func_t       func;
    void             *arg;
    char              name[32];
}task_node_t;
/* 任务队列 */
typedef struct {
    task_node_t   *head;
    task_node_t   *tail;
    size_t         count;
    size_t         capacity;
} task_queue_t;
//全局状态
static struct {
    pthread_t       *threads;
    int              num_workers;
    bool             running;

    task_queue_t     queue;
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    pthread_cond_t   empty_cond;

    int              active_count;
    task_id_t        next_id;
} s_pool = {0};

//队列操作
static void queue_init(task_queue_t *q, size_t capacity)
  {
    q->head = q->tail = NULL;
    q->count = 0;
    q->capacity = capacity;
  }

static err_t queue_push(task_queue_t *q, task_node_t *node)
  {
      if (q->count >= q->capacity) return ERR_FULL;
      node->next = NULL;
      if (q->tail)
          q->tail->next = node;
      else
          q->head = node;
      q->tail = node;
      q->count++;
      return ERR_OK;
  }

static task_node_t *queue_pop(task_queue_t *q)
  {
      if (!q->head) return NULL;
      task_node_t *node = q->head;
      q->head = node->next;
      if (!q->head) q->tail = NULL;
      q->count--;
      return node;
  }

  static void *worker(void *arg)
  {
    (void)arg;
    while(1){
        pthread_mutex_lock(&s_pool.lock);   
        while(s_pool.queue.count==0&&s_pool.running){
            pthread_cond_wait(&s_pool.cond, &s_pool.lock);
        }
        if(s_pool.running==false) {
            pthread_mutex_unlock(&s_pool.lock);
            return NULL;
        }
        task_node_t *task = queue_pop(&s_pool.queue);
        s_pool.active_count++;
        pthread_mutex_unlock(&s_pool.lock);

        task->func(task->arg);

        free(task);
        pthread_mutex_lock(&s_pool.lock);
        s_pool.active_count--;
        
        pthread_cond_signal(&s_pool.empty_cond);
        pthread_mutex_unlock(&s_pool.lock);
        
    }
    return NULL;
  }

//接口实现
err_t thread_pool_init(int num_workers,size_t  queue_size){
    if(num_workers<=0) num_workers = 2;
    if(queue_size==0) queue_size = 32;
    memset(&s_pool, 0, sizeof(s_pool));
    s_pool.num_workers = num_workers;
    s_pool.running=true;
    s_pool.next_id=1;

    queue_init(&s_pool.queue, queue_size);
    pthread_mutex_init(&s_pool.lock, NULL);
    pthread_cond_init(&s_pool.cond, NULL);
    pthread_cond_init(&s_pool.empty_cond, NULL);

    s_pool.threads =calloc(num_workers, sizeof(pthread_t));
    if(!s_pool.threads) return ERR_NOMEM;
    for(int i=0; i<num_workers; i++){
        pthread_create(&s_pool.threads[i], NULL, worker, NULL);
    }
    return ERR_OK;
}

void thread_pool_deinit(void){
    if(!s_pool.running) return;
    pthread_mutex_lock(&s_pool.lock);
    while(s_pool.queue.count>0||s_pool.active_count>0){
        pthread_cond_wait(&s_pool.empty_cond, &s_pool.lock);
    }
    s_pool.running=false;
    pthread_cond_broadcast(&s_pool.cond);
    pthread_mutex_unlock(&s_pool.lock);

    for(int i=0;i<s_pool.num_workers; i++){
        pthread_join(s_pool.threads[i], NULL);
    }
    free(s_pool.threads);
    pthread_mutex_destroy(&s_pool.lock);
    pthread_cond_destroy(&s_pool.cond);
    pthread_cond_destroy(&s_pool.empty_cond);
}

task_id_t thread_pool_submit(task_func_t func,void *arg, const char *name){
    if(!func) return 0; 
    task_node_t *node = calloc(1, sizeof(task_node_t));
    if(!node) return 0;

    pthread_mutex_lock(&s_pool.lock);
    if(!s_pool.running){
        free(node);
        pthread_mutex_unlock(&s_pool.lock);
        return 0;
    }
    node->id = s_pool.next_id++;
    node->func = func;
    node->arg = arg;
    if(name) strncpy(node->name, name, sizeof(node->name)-1);
    err_t err = queue_push(&s_pool.queue, node);
    if(IS_ERR(err)){
        free(node);
        pthread_mutex_unlock(&s_pool.lock);
        return 0;
    }
    pthread_cond_signal(&s_pool.cond);
    pthread_mutex_unlock(&s_pool.lock);
    return node->id;
}

/* ---- MODULE_DEFINE ---- */
static err_t tpool_module_init(void)   { return thread_pool_init(2, 32); }
static void   tpool_module_deinit(void){ thread_pool_deinit(); }

MODULE_DEFINE(thread_pool, tpool_module_init, tpool_module_deinit,
              NULL, MODULE_PRIO_FRAMEWORK);
