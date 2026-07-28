/*
 * 线程池单元测试
 *
 * 编译：
 *   gcc -I../inc -D_GNU_SOURCE test_thread_pool.c ../src/core/thread_pool.c \
 *       ../src/utils/logger.c -lpthread -o test_thread_pool
 *
 * 运行：
 *   ./test_thread_pool
 */

#include "thread_pool.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
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

/* ======================== 辅助函数 ======================== */

static volatile int g_task_count     = 0;
static volatile int g_task_sum       = 0;
static volatile int g_task_a_called  = 0;
static volatile int g_task_b_called  = 0;

static void reset_counters(void)
{
    g_task_count    = 0;
    g_task_sum      = 0;
    g_task_a_called = 0;
    g_task_b_called = 0;
}

/* ======================== 任务函数 ======================== */

static void count_task(void *arg)
{
    (void)arg;
    __sync_fetch_and_add(&g_task_count, 1);
}

static void sum_task(void *arg)
{
    int n = *(int *)arg;
    __sync_fetch_and_add(&g_task_sum, n);
}

static void named_task_a(void *arg) { (void)arg; g_task_a_called = 1; }
static void named_task_b(void *arg) { (void)arg; g_task_b_called = 1; }

/* ======================== 测试用例 ======================== */

static void test_init_deinit(void)
{
    TEST("init and deinit");
    err_t err = thread_pool_init(2, 8);
    if (IS_ERR(err)) { FAIL("init failed"); return; }
    /* 什么都不做，直接 deinit */
    thread_pool_deinit();
    PASS();
}

static void test_submit_one_task(void)
{
    TEST("submit and execute one task");
    reset_counters();
    thread_pool_init(2, 8);

    task_id_t tid = thread_pool_submit(count_task, NULL, "count");
    if (!tid) { FAIL("submit returned NULL"); goto cleanup; }

    usleep(100000); /* 等线程执行 */
    if (g_task_count == 1) { PASS(); goto cleanup; }
    FAIL("task not executed");

cleanup:
    thread_pool_deinit();
}

static void test_multiple_tasks(void)
{
    TEST("submit multiple tasks, all execute");
    reset_counters();
    int n = 10;
    thread_pool_init(4, 16);

    for (int i = 0; i < n; i++) {
        task_id_t tid = thread_pool_submit(count_task, NULL, "count");
        if (!tid) { FAIL("submit returned NULL"); goto cleanup; }
    }

    usleep(200000); /* 等所有任务执行 */
    if (g_task_count == n) { PASS(); goto cleanup; }
    FAIL("not all tasks executed");

cleanup:
    thread_pool_deinit();
}

static void test_task_argument(void)
{
    TEST("task receives correct argument");
    reset_counters();
    thread_pool_init(2, 8);

    int val = 42;
    task_id_t tid = thread_pool_submit(sum_task, &val, "sum");
    if (!tid) { FAIL("submit failed"); goto cleanup; }

    usleep(100000);
    if (g_task_sum == 42) { PASS(); goto cleanup; }
    FAIL("wrong argument value");

cleanup:
    thread_pool_deinit();
}

static void test_task_name(void)
{
    TEST("named tasks don't interfere with each other");
    reset_counters();
    thread_pool_init(2, 8);

    task_id_t ta = thread_pool_submit(named_task_a, NULL, "task_a");
    task_id_t tb = thread_pool_submit(named_task_b, NULL, "task_b");
    if (!ta || !tb) { FAIL("submit failed"); goto cleanup; }

    usleep(100000);
    if (g_task_a_called && g_task_b_called) { PASS(); goto cleanup; }
    FAIL("not all named tasks executed");

cleanup:
    thread_pool_deinit();
}

static void test_submit_after_deinit(void)
{
    TEST("submit after deinit returns NULL or doesn't crash");
    thread_pool_init(1, 4);
    thread_pool_deinit();

    task_id_t tid = thread_pool_submit(count_task, NULL, "late");
    if (tid) {
        /* 有的实现可能在关闭后还接受任务但不执行 */
        usleep(50000);
    }
    PASS();
}

static void test_reinit(void)
{
    TEST("reinit after deinit works");
    err_t e1 = thread_pool_init(2, 8);
    thread_pool_deinit();
    err_t e2 = thread_pool_init(2, 8);
    if (IS_OK(e1) && IS_OK(e2)) { PASS(); goto cleanup; }
    FAIL("reinit failed");
cleanup:
    thread_pool_deinit();
}

static void test_concurrent_tasks(void)
{
    TEST("concurrent tasks with multiple workers");
    reset_counters();
    int n = 50;
    thread_pool_init(4, 64);

    for (int i = 1; i <= n; i++) {
        int *arg = malloc(sizeof(int));
        *arg = i;
        task_id_t tid = thread_pool_submit(sum_task, arg, "sum");
        if (!tid) { free(arg); FAIL("submit failed"); goto cleanup; }
    }

    usleep(500000);
    /* 1+2+...+n = n*(n+1)/2 */
    int expected = n * (n + 1) / 2;
    if (g_task_sum == expected) { PASS(); goto cleanup; }
    FAIL("sum mismatch");

cleanup:
    thread_pool_deinit();
}

/* ======================== 主函数 ======================== */

int main(void)
{
    logger_init(LOG_ERROR, NULL);

    printf("\n=== Thread Pool Unit Tests ===\n\n");

    test_init_deinit();
    test_submit_one_task();
    test_multiple_tasks();
    test_task_argument();
    test_task_name();
    test_submit_after_deinit();
    test_reinit();
    test_concurrent_tasks();

    printf("\n=== Results: %d passed, %d failed ===\n\n",
           g_tests_passed, g_tests_failed);

    logger_deinit();
    return g_tests_failed ? 1 : 0;
}
