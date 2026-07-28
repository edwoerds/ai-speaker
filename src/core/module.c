#include "module.h"
#include "logger.h"
#include <stdlib.h>

#define MAX_MODULES 32

static struct{
    const module_def_t *modules[MAX_MODULES];
    int  count;
    bool sorted;
} s_reg={0};

static int cmp_prio(const void *a,const void *b){
    const module_def_t *ma=*(const module_def_t**)a;
    const module_def_t *mb=*(const module_def_t**)b;
    return ma->priority-mb->priority;
}

void module_register(const module_def_t *def){
    if(!def || s_reg.count >= MAX_MODULES) return;
    s_reg.modules[s_reg.count++]=def;
    s_reg.sorted=false;
}

err_t module_init_all(void){
    if(!s_reg.sorted){
        qsort(s_reg.modules,s_reg.count,sizeof(s_reg.modules[0]),cmp_prio);
        s_reg.sorted=true;
    }
    LOG_INFO("--- Module init start ---");
    for (int i = 0; i < s_reg.count; i++) {
          const module_def_t *m = s_reg.modules[i];
          if (!m->init) continue;
          LOG_INFO("Init: %s (prio=%d)", m->name, m->priority);
          err_t err = m->init();
          if (IS_ERR(err)) {
              LOG_ERROR("Module init FAILED: %s", m->name);
              for (int j = i - 1; j >= 0; j--)
                  if (s_reg.modules[j]->deinit)
                      s_reg.modules[j]->deinit();
              return err;
          }
      }
    LOG_INFO("--- Module init done (%d) ---", s_reg.count);
    return ERR_OK;
}
  void module_shutdown_all(void)
  {
      LOG_INFO("--- Module shutdown ---");
      if (!s_reg.sorted) {
          qsort(s_reg.modules, s_reg.count, sizeof(s_reg.modules[0]), cmp_prio);
      }
      for (int i = s_reg.count - 1; i >= 0; i--) {
          const module_def_t *m = s_reg.modules[i];
          if (m->shutdown) m->shutdown();
      }
  }

  void module_deinit_all(void)
  {
      LOG_INFO("--- Module deinit ---");
      if (!s_reg.sorted) {
          qsort(s_reg.modules, s_reg.count, sizeof(s_reg.modules[0]), cmp_prio);
      }
      for (int i = s_reg.count - 1; i >= 0; i--) {
          const module_def_t *m = s_reg.modules[i];
          if (m->deinit) m->deinit();
      }
  }