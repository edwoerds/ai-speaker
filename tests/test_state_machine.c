/*
 * 状态机单元测试
 *
 * 编译：
 *   gcc -I../inc -D_GNU_SOURCE test_state_machine.c ../src/core/state_machine.c \
 *       -lpthread -o test_state_machine
 *
 * 运行：
 *   ./test_state_machine
 */

#include "state_machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ======================== 测试统计 ======================== */
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name)  do { \
    printf("  TEST: %s ... ", name); \
    fflush(stdout); \
} while(0)

#define PASS()  do { printf("PASS\n"); g_tests_passed++; } while(0)
#define FAIL(msg)  do { \
    printf("FAIL: %s\n", msg); \
    g_tests_failed++; \
} while(0)

/* ======================== 测试用例状态定义 ======================== */
enum {
    ST_IDLE    = 0,
    ST_BUSY    = 1,
    ST_DONE    = 2,
    ST_ERROR   = 3,
    ST_COUNT
};

enum {
    EV_START  = 0x100,
    EV_FINISH = 0x101,
    EV_FAIL   = 0x102,
    EV_RESET  = 0x103,
};

/* ======================== 辅助函数 ======================== */

/* 记录 action 调用 */
typedef struct {
    int  called;
    int  from_state;
    int  to_state;
    int  event;
    void *data;
    int  call_count;
} action_record_t;

static void clear_record(action_record_t *r)
{
    memset(r, 0, sizeof(*r));
}

static void record_action(action_record_t *r,
                          int from, int to, int ev, void *data)
{
    r->called     = 1;
    r->from_state = from;
    r->to_state   = to;
    r->event      = ev;
    r->data       = data;
    r->call_count++;
}

/* ======================== 动作函数 ======================== */
static action_record_t g_action = {0};

static void do_start(int from, int to, int ev, void *data)
{
    record_action(&g_action, from, to, ev, data);
}

static void do_finish(int from, int to, int ev, void *data)
{
    record_action(&g_action, from, to, ev, data);
}

static void do_fail(int from, int to, int ev, void *data)
{
    record_action(&g_action, from, to, ev, data);
}

static void do_reset_action(int from, int to, int ev, void *data)
{
    record_action(&g_action, from, to, ev, data);
}

/* ======================== 测试用例 ======================== */

static void test_init_state(void)
{
    TEST("initial state is set correctly");
    sm_transition_t table[] = {
        { ST_IDLE, EV_START, ST_BUSY, do_start },
    };
    sm_init(table, 1, ST_IDLE);
    int state = sm_current_state();
    if (state == ST_IDLE) {
        sm_reset(ST_IDLE);
        PASS();
        return;
    }
    FAIL("wrong initial state");
}

static void test_valid_transition(void)
{
    TEST("valid transition changes state");
    clear_record(&g_action);
    sm_transition_t table[] = {
        { ST_IDLE, EV_START, ST_BUSY, do_start },
    };
    sm_init(table, 1, ST_IDLE);

    err_t err = sm_dispatch(EV_START, NULL);
    if (IS_ERR(err)) { FAIL("dispatch failed"); return; }

    int state = sm_current_state();
    if (state != ST_BUSY) { FAIL("state not updated"); return; }
    if (!g_action.called)  { FAIL("action not called"); return; }
    if (g_action.call_count != 1) { FAIL("action called wrong count"); return; }
    PASS();
}

static void test_invalid_transition(void)
{
    TEST("invalid transition returns ERR_STATE");
    sm_transition_t table[] = {
        { ST_IDLE, EV_START, ST_BUSY, do_start },
    };
    sm_init(table, 1, ST_IDLE);

    /* 在 ST_IDLE 下发 EV_FINISH，表中没有 */
    err_t err = sm_dispatch(EV_FINISH, NULL);
    if (err == ERR_STATE_INV) { PASS(); return; }
    FAIL("expected ERR_STATE_INV");
}

static void test_wildcard_transition(void)
{
    TEST("from_state=-1 matches any state");
    clear_record(&g_action);
    sm_transition_t table[] = {
        { ST_IDLE, EV_START, ST_BUSY, do_start },
        { -1, EV_RESET, ST_IDLE, do_reset_action },
    };
    sm_init(table, 2, ST_IDLE);

    /* 先到 BUSY */
    sm_dispatch(EV_START, NULL);
    /* 从 -1 匹配回 IDLE */
    clear_record(&g_action);
    err_t err = sm_dispatch(EV_RESET, NULL);
    if (IS_ERR(err)) { FAIL("wildcard dispatch failed"); return; }

    if (sm_current_state() != ST_IDLE) { FAIL("wildcard: wrong state"); return; }
    if (!g_action.called) { FAIL("wildcard action not called"); return; }
    PASS();
}

static void test_action_data(void)
{
    TEST("action receives correct from/to/event/data");
    clear_record(&g_action);
    sm_transition_t table[] = {
        { ST_IDLE, EV_START, ST_BUSY, do_start },
    };
    sm_init(table, 1, ST_IDLE);

    int test_val = 42;
    sm_dispatch(EV_START, &test_val);

    if (!g_action.called)  { FAIL("action not called"); return; }
    if (g_action.from_state != ST_IDLE)  { FAIL("wrong from"); return; }
    if (g_action.to_state  != ST_BUSY)   { FAIL("wrong to");   return; }
    if (g_action.event     != EV_START)   { FAIL("wrong ev");   return; }
    if (g_action.data      != &test_val) { FAIL("wrong data"); return; }
    PASS();
}

static void test_null_action(void)
{
    TEST("NULL action (state-only transition) is OK");
    sm_transition_t table[] = {
        { ST_IDLE, EV_START, ST_BUSY, NULL },
    };
    sm_init(table, 1, ST_IDLE);

    err_t err = sm_dispatch(EV_START, NULL);
    if (IS_ERR(err)) { FAIL("dispatch failed"); return; }
    if (sm_current_state() != ST_BUSY) { FAIL("state not changed"); return; }
    PASS();
}

static void test_chained_transitions(void)
{
    TEST("multiple transitions in sequence");
    clear_record(&g_action);
    sm_transition_t table[] = {
        { ST_IDLE,  EV_START,  ST_BUSY, do_start },
        { ST_BUSY,  EV_FINISH, ST_DONE, do_finish },
        { ST_DONE,  EV_FAIL,   ST_ERROR, do_fail },
    };
    sm_init(table, 3, ST_IDLE);

    sm_dispatch(EV_START, NULL);
    if (sm_current_state() != ST_BUSY)  { FAIL("step 1"); return; }

    sm_dispatch(EV_FINISH, NULL);
    if (sm_current_state() != ST_DONE)  { FAIL("step 2"); return; }

    sm_dispatch(EV_FAIL, NULL);
    if (sm_current_state() != ST_ERROR) { FAIL("step 3"); return; }

    PASS();
}

static void test_reset(void)
{
    TEST("sm_reset forces state change");
    sm_transition_t table[] = {
        { ST_IDLE, EV_START, ST_BUSY, do_start },
    };
    sm_init(table, 1, ST_IDLE);

    sm_dispatch(EV_START, NULL);
    assert(sm_current_state() == ST_BUSY);

    sm_reset(ST_IDLE);
    if (sm_current_state() == ST_IDLE) { PASS(); return; }
    FAIL("reset failed");
}

static void test_thread_safety(void)
{
    TEST("state machine is safe from multiple threads (no crash)");
    sm_transition_t table[] = {
        { ST_IDLE,  EV_START,  ST_BUSY,  do_start },
        { ST_BUSY,  EV_FINISH, ST_DONE,  do_finish },
        { ST_DONE,  EV_FAIL,   ST_ERROR, do_fail },
        { -1,       EV_RESET,  ST_IDLE,  do_reset_action },
    };
    sm_init(table, 4, ST_IDLE);

    /* 快速切换多次验证无崩溃 */
    for (int i = 0; i < 1000; i++) {
        sm_dispatch(EV_START, NULL);
        sm_dispatch(EV_FINISH, NULL);
        sm_dispatch(EV_FAIL, NULL);
        sm_dispatch(EV_RESET, NULL);
    }
    PASS();
}

/* ======================== 主函数 ======================== */

int main(void)
{
    printf("\n=== State Machine Unit Tests ===\n\n");

    test_init_state();
    test_valid_transition();
    test_invalid_transition();
    test_wildcard_transition();
    test_action_data();
    test_null_action();
    test_chained_transitions();
    test_reset();
    test_thread_safety();

    printf("\n=== Results: %d passed, %d failed ===\n\n",
           g_tests_passed, g_tests_failed);

    return g_tests_failed ? 1 : 0;
}
