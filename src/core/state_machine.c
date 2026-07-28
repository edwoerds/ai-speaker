#include "state_machine.h"
#include <pthread.h>
#include <string.h>

static struct{
    const sm_transition_t *table;
    size_t  table_size;
    int current_state;
    pthread_mutex_t lock;
    bool inited;
} s_sm={0};

//写 sm_init——接收用户定义的转移表，设置初始状态
void sm_init(const sm_transition_t *table,size_t table_size,int init_state)
{
    memset(&s_sm,0,sizeof(s_sm));
    s_sm.table=table;
    s_sm.table_size=table_size;
    s_sm.current_state=init_state;
    pthread_mutex_init(&s_sm.lock, NULL);
    s_sm.inited=true;
}

//核心函数 sm_dispatch——查表找到匹配的转移并执行
err_t sm_dispatch(int event,void *data){
    if(!s_sm.inited) return ERR_GENERAL;
    pthread_mutex_lock(&s_sm.lock);
    int cur=s_sm.current_state;
    for(size_t i=0; i<s_sm.table_size; i++){
        const sm_transition_t *t=&s_sm.table[i];
        if((t->from_state==cur||t->from_state==-1) && t->event==event){
            s_sm.current_state=t->to_state;
            pthread_mutex_unlock(&s_sm.lock);
            if(t->action) t->action(cur,t->to_state,event,data);
            return ERR_OK;
        }
    }
    pthread_mutex_unlock(&s_sm.lock);
    return ERR_STATE_INV;
}

//辅助函数
int sm_current_state(void){
    pthread_mutex_lock(&s_sm.lock);
    int state=s_sm.current_state;
    pthread_mutex_unlock(&s_sm.lock);
    return state;
}
//手动把状态机重置到某个状态
void sm_reset(int state){
    pthread_mutex_lock(&s_sm.lock);
    s_sm.current_state=state;
    pthread_mutex_unlock(&s_sm.lock);
}