#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include"common.h"
 /* 状态变化时执行的动作函数 */
typedef void (*sm_action_t)(int from_state, int to_state, int event,void *data);
 /* 状态转移表的一行 */
 typedef struct{
    int  from_state;/* 从哪个状态出发 */
    int event;/* 什么事件触发 */
    int to_state;/* 跳到哪个状态 */
    sm_action_t action; /* 跳的时候做什么 */
 } sm_transition_t;

 void sm_init(const sm_transition_t *table,size_t table_size,int init_state);
 err_t sm_dispatch(int event,void *data);
 int sm_current_state(void);
 void sm_reset(int state);
 #endif