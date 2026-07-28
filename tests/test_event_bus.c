/*
 * 事件总线单元测试
 *
 * 编译：
 *   gcc -I../inc -D_GNU_SOURCE test_event_bus.c ../src/core/event_bus.c \
 *       ../src/utils/logger.c -lpthread -o test_event_bus
 *
 * 运行：
 *   ./test_event_bus
 */

#include "event_bus.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

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

/* ======================== 测试辅助 ======================== */

/* 在独立线程中启动事件分发 */
static pthread_t g_dispatch_thread = {0};

static void *dispatch_loop(void *arg)
{
    (void)arg;
    event_dispatch_loop();
    return NULL;
}

static void start_dispatch(void)
{
    pthread_create(&g_dispatch_thread, NULL, dispatch_loop, NULL);
    usleep(50000); /* 等线程启动 */
}

static void stop_dispatch(void)
{
    event_bus_stop();
    pthread_join(g_dispatch_thread, NULL);
    memset(&g_dispatch_thread, 0, sizeof(g_dispatch_thread));
}

/* 有 user_data 的处理器 */
static void test_handler_ud(const event_t *ev, void *user_data)
{
    (void)ev;
    if (user_data)
        *(int *)user_data = 1;
}

/* 记录收到的最后一个事件 */
static struct {
    event_id_t      id;
    int             call_count;
    int             string_match;  /* 用于 test_publish_string */
} s_last_event = {0};

static void reset_last_event(void)
{
    memset(&s_last_event, 0, sizeof(s_last_event));
}

static const char *g_expected_string = NULL;

static void test_handler(const event_t *ev, void *user_data)
{
    (void)user_data;
    s_last_event.id = ev->id;
    s_last_event.call_count++;
    /* 检查字符串匹配 */
    if (g_expected_string && ev->data && ev->data_size > 0) {
        s_last_event.string_match =
            (strcmp((const char *)ev->data, g_expected_string) == 0);
    }
}


/* ======================== 测试用例 ======================== */

static void test_init_deinit(void)
{
    TEST("init and deinit");
    err_t err = event_bus_init(16);
    if (IS_ERR(err)) { FAIL("init failed"); return; }
    event_bus_stop();
    event_bus_deinit();
    PASS();
}

static void test_subscribe_unsubscribe(void)
{
    TEST("subscribe and unsubscribe");
    event_bus_init(16);
    reset_last_event();

    subscription_t *sub = event_subscribe(EV_SYS_SHUTDOWN, test_handler, NULL);
    if (!sub) { FAIL("subscribe returned NULL"); goto cleanup; }

    event_unsubscribe(sub);
    PASS();
cleanup:
    event_bus_stop();
    event_bus_deinit();
}

static void test_publish_and_dispatch(void)
{
    TEST("publish -> dispatch -> handler called");
    event_bus_init(16);
    reset_last_event();
    start_dispatch();

    subscription_t *sub = event_subscribe(EV_SYS_SHUTDOWN, test_handler, NULL);
    assert(sub);

    event_publish_simple(EV_SYS_SHUTDOWN);
    usleep(10000);
    stop_dispatch();
    event_bus_deinit();

    if (s_last_event.call_count != 1) {
        FAIL("handler not called exactly once");
        return;
    }
    if (s_last_event.id != EV_SYS_SHUTDOWN) {
        FAIL("wrong event id received");
        return;
    }
    PASS();
}

static void test_publish_string(void)
{
    TEST("publish string data");
    event_bus_init(16);
    reset_last_event();
    start_dispatch();

    subscription_t *sub = event_subscribe(EV_BT_DATA_RECEIVED, test_handler, NULL);
    assert(sub);

    const char *msg = "hello";
    g_expected_string = msg;
    event_publish_string(EV_BT_DATA_RECEIVED, msg);
    usleep(10000);
    g_expected_string = NULL;
    stop_dispatch();
    event_bus_deinit();

    if (s_last_event.call_count != 1) {
        FAIL("handler not called");
        return;
    }
    if (!s_last_event.string_match) {
        FAIL("string data mismatch");
        return;
    }
    PASS();
}

static void test_multiple_subscribers(void)
{
    TEST("multiple subscribers on same event");
    event_bus_init(16);
    reset_last_event();
    start_dispatch();

    int flag1 = 0, flag2 = 0;
    subscription_t *s1 = event_subscribe(EV_SYS_SHUTDOWN, test_handler_ud, &flag1);
    subscription_t *s2 = event_subscribe(EV_SYS_SHUTDOWN, test_handler_ud, &flag2);
    assert(s1 && s2);

    event_publish_simple(EV_SYS_SHUTDOWN);
    usleep(10000);
    stop_dispatch();
    event_bus_deinit();

    if (flag1 && flag2) { PASS(); return; }
    FAIL("not all handlers called");
}

static void test_event_filtering(void)
{
    TEST("handler only gets subscribed event, not others");
    event_bus_init(16);
    reset_last_event();
    start_dispatch();

    subscription_t *sub = event_subscribe(EV_SYS_SHUTDOWN, test_handler, NULL);
    assert(sub);

    /* 发布一个不同的事件 */
    event_publish_simple(EV_SYS_ERROR);
    usleep(10000);
    stop_dispatch();
    event_bus_deinit();

    if (s_last_event.call_count == 0) {
        PASS();
        return;
    }
    FAIL("handler was called for wrong event");
}

static void test_queue_full(void)
{
    TEST("queue full returns ERR_FULL");
    /* 容量=2 */
    event_bus_init(2);
    reset_last_event();
    start_dispatch();

    subscription_t *sub = event_subscribe(EV_SYS_SHUTDOWN, test_handler, NULL);
    assert(sub);

    /* 填满 */
    err_t e1 = event_publish_simple(EV_SYS_SHUTDOWN);  /* 占位 */
    err_t e2 = event_publish_simple(EV_SYS_SHUTDOWN);  /* 占位 */
    err_t e3 = event_publish_simple(EV_SYS_SHUTDOWN);  /* 应该满 */

    if (IS_OK(e1) && IS_OK(e2) && e3 == ERR_FULL) {
        stop_dispatch();
        event_bus_deinit();
        PASS();
        return;
    }
    FAIL("expected ERR_FULL on 3rd publish");
    stop_dispatch();
    event_bus_deinit();
}

static void test_user_data_passthrough(void)
{
    TEST("user_data passed to handler");
    event_bus_init(16);
    start_dispatch();

    int flag = 0;
    subscription_t *sub = event_subscribe(EV_SYS_SHUTDOWN, test_handler_ud, &flag);
    assert(sub);

    event_publish_simple(EV_SYS_SHUTDOWN);
    usleep(10000);
    stop_dispatch();
    event_bus_deinit();

    if (flag) { PASS(); return; }
    FAIL("user_data not updated by handler");
}

/* ======================== 主函数 ======================== */

int main(void)
{
    /* 日志设到最高级别，不让测试输出被日志淹没 */
    logger_init(LOG_ERROR, NULL);

    printf("\n=== Event Bus Unit Tests ===\n\n");

    test_init_deinit();
    test_subscribe_unsubscribe();
    test_publish_and_dispatch();
    test_publish_string();
    test_multiple_subscribers();
    test_event_filtering();
    test_queue_full();
    test_user_data_passthrough();

    printf("\n=== Results: %d passed, %d failed ===\n\n",
           g_tests_passed, g_tests_failed);

    logger_deinit();
    return g_tests_failed ? 1 : 0;
}
