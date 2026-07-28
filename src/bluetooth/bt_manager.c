#include "bt_manager.h"
#include "event_bus.h"
#include "logger.h"
#include "module.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <systemd/sd-bus.h>

#define SPP_UUID "00001101-0000-1000-8000-00805F9B34FB"
#define SPP_OBJ_PATH "/com/aispeaker/spp"

static struct {
    sd_bus       *bus;
    sd_bus_slot  *vtable_slot;   /* Profile1 vtable slot */
    pthread_t     thread;
    volatile bool running;
    volatile bool connected;
} s_bt = {0};

/* ==================================================================
 * BlueZ D-Bus 属性设置
 * ================================================================ */

static int adapter_set_bool(const char *prop, int value)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = NULL;
    int r = sd_bus_message_new_method_call(s_bt.bus, &msg,
              "org.bluez", "/org/bluez/hci0",
              "org.freedesktop.DBus.Properties", "Set");
    if (r < 0) goto finish;
    r = sd_bus_message_append(msg, "ss", "org.bluez.Adapter1", prop);
    if (r < 0) goto finish;
    r = sd_bus_message_open_container(msg, 'v', "b");
    if (r < 0) goto finish;
    r = sd_bus_message_append(msg, "b", value);
    if (r < 0) goto finish;
    r = sd_bus_message_close_container(msg);
    if (r < 0) goto finish;
    r = sd_bus_call(s_bt.bus, msg, 0, &error, NULL);
    if (r < 0) LOG_ERROR("BT Set(%s) failed: %s", prop, error.message);
finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(msg);
    return r;
}

static int adapter_set_string(const char *prop, const char *value)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = NULL;
    int r = sd_bus_message_new_method_call(s_bt.bus, &msg,
              "org.bluez", "/org/bluez/hci0",
              "org.freedesktop.DBus.Properties", "Set");
    if (r < 0) goto finish;
    r = sd_bus_message_append(msg, "ss", "org.bluez.Adapter1", prop);
    if (r < 0) goto finish;
    r = sd_bus_message_open_container(msg, 'v', "s");
    if (r < 0) goto finish;
    r = sd_bus_message_append(msg, "s", value);
    if (r < 0) goto finish;
    r = sd_bus_message_close_container(msg);
    if (r < 0) goto finish;
    r = sd_bus_call(s_bt.bus, msg, 0, &error, NULL);
    if (r < 0) LOG_ERROR("BT Set(%s) failed: %s", prop, error.message);
finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(msg);
    return r;
}

/* ==================================================================
 * D-Bus Profile1 回调
 * ================================================================ */

/* 读线程：处理 SPP 连接的数据 */
struct spp_conn {
    int fd;
    char peer[32];
};

static void *spp_read_thread(void *arg)
{
    struct spp_conn *c = (struct spp_conn *)arg;
    int fd = c->fd;
    LOG_INFO("SPP reading: %s (fd=%d)", c->peer, fd);

    s_bt.connected = true;
    event_publish_simple(EV_BT_DEVICE_CONN);

    char buf[1024];
    /* 行缓冲，处理蓝牙分包 */
    static char line_buf[4096];
    static int line_off = 0;

    while (s_bt.running) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            break;
        }
        /* 逐字节处理：拼接到行缓冲，遇到换行符就发 */
        for (int i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n' || ch == '\r') {
                if (line_off > 0) {
                    line_buf[line_off] = '\0';
                    LOG_INFO("SPP recv: %s", line_buf);
                    event_publish_string(EV_BT_DATA_RECEIVED, line_buf);
                    line_off = 0;
                }
            } else {
                line_buf[line_off++] = ch;
                if (line_off >= (int)sizeof(line_buf) - 1) {
                    line_buf[line_off] = '\0';
                    LOG_INFO("SPP recv: %s", line_buf);
                    event_publish_string(EV_BT_DATA_RECEIVED, line_buf);
                    line_off = 0;
                }
            }
        }
    }

    LOG_INFO("SPP disconnected: %s", c->peer);
    close(fd);
    s_bt.connected = false;
    event_publish_simple(EV_BT_DEVICE_DISCONN);
    free(c);
    return NULL;
}

/* NewConnection: BlueZ 传给我们已连接的 fd，开线程处理 */
static int spp_new_connection_cb(sd_bus_message *msg, void *ud,
                                  sd_bus_error *ret_error)
{
    (void)ud; (void)ret_error;
    const char *path;
    int fd = -1;
    int r = sd_bus_message_read(msg, "oh", &path, &fd);
    sd_bus_message_skip(msg, "a{sv}");
    if (r < 0 || fd < 0) return 0;

    struct spp_conn *c = calloc(1, sizeof(*c));
    if (!c) { close(fd); return 0; }
    /* 设为阻塞模式，D-Bus 传来的 fd 默认是非阻塞的 */
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    /* dup 一份让线程持有，避免 D-Bus 关闭原 fd */
    c->fd = dup(fd);
    snprintf(c->peer, sizeof(c->peer), "unknown");

    struct sockaddr_rc sa;
    socklen_t slen = sizeof(sa);
    if (getpeername(fd, (struct sockaddr *)&sa, &slen) == 0) {
        ba2str(&sa.rc_bdaddr, c->peer);
    }

    pthread_t tid;
    pthread_create(&tid, NULL, spp_read_thread, c);
    pthread_detach(tid);

    /* 立即回复 BlueZ，不阻塞 */
    return sd_bus_reply_method_return(msg, "");
}

static int spp_release_cb(sd_bus_message *msg, void *ud, sd_bus_error *re)
{
    (void)ud; (void)re;
    return sd_bus_reply_method_return(msg, "");
}

static int spp_request_disconn_cb(sd_bus_message *msg, void *ud, sd_bus_error *re)
{
    (void)ud; (void)re;
    const char *path;
    sd_bus_message_read(msg, "o", &path);
    return sd_bus_reply_method_return(msg, "");
}

static const sd_bus_vtable spp_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("NewConnection", "oha{sv}", "", spp_new_connection_cb, 0),
    SD_BUS_METHOD("RequestDisconnection", "o", "", spp_request_disconn_cb, 0),
    SD_BUS_METHOD("Release", "", "", spp_release_cb, 0),
    SD_BUS_VTABLE_END
};

/* ==================================================================
 * D-Bus 分发线程
 * ================================================================ */

static void *dbus_dispatch_thread(void *arg)
{
    (void)arg;
    LOG_INFO("BT: SPP dispatch started");
    while (s_bt.running && s_bt.bus) {
        while (sd_bus_process(s_bt.bus, NULL) > 0);
        usleep(50000);
    }
    LOG_INFO("BT: SPP dispatch exited");
    return NULL;
}

/* ==================================================================
 * bt_manager 接口实现
 * ================================================================ */

err_t bt_manager_init(const bt_manager_config_t *cfg)
{
    if (!cfg) return ERR_INVAL;

    int r = sd_bus_open_system(&s_bt.bus);
    if (r < 0) {
        LOG_ERROR("BT: D-Bus failed: %s", strerror(-r));
        return ERR_NODEV;
    }

    /* 1. 打开蓝牙 */
    adapter_set_bool("Powered", 1);
    /* 2. 设置名称 */
    adapter_set_string("Alias", cfg->device_name);
    /* 3. 可发现 */
    adapter_set_bool("Discoverable", 1);

    /* 5. 注册 Profile1 vtable */
    r = sd_bus_add_object_vtable(s_bt.bus, &s_bt.vtable_slot,
            SPP_OBJ_PATH, "org.bluez.Profile1", spp_vtable, NULL);
    if (r < 0) {
        LOG_ERROR("BT: vtable failed: %s", strerror(-r));
        sd_bus_unref(s_bt.bus);
        return ERR_NODEV;
    }

    /* 6. 启动 D-Bus 分发线程（处理 vtable 回调） */
    s_bt.running = true;
    pthread_create(&s_bt.thread, NULL, dbus_dispatch_thread, NULL);
    usleep(100000);  /* 等线程启动 */

    /* 7. 注册 SPP Profile */
    {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message *m = NULL;
        r = sd_bus_message_new_method_call(s_bt.bus, &m,
                "org.bluez", "/org/bluez",
                "org.bluez.ProfileManager1", "RegisterProfile");
        if (r >= 0) {
            sd_bus_message_append(m, "o", SPP_OBJ_PATH);
            sd_bus_message_append(m, "s", SPP_UUID);
            sd_bus_message_open_container(m, 'a', "{sv}");
            sd_bus_message_open_container(m, 'e', "sv");
            sd_bus_message_append(m, "s", "Channel");
            sd_bus_message_open_container(m, 'v', "q");
            sd_bus_message_append(m, "q", (uint16_t)1);
            sd_bus_message_close_container(m);
            sd_bus_message_close_container(m);
            sd_bus_message_open_container(m, 'e', "sv");
            sd_bus_message_append(m, "s", "AutoConnect");
            sd_bus_message_open_container(m, 'v', "b");
            sd_bus_message_append(m, "b", 1);
            sd_bus_message_close_container(m);
            sd_bus_message_close_container(m);
            sd_bus_message_close_container(m);

            r = sd_bus_call(s_bt.bus, m, 5000, &err, NULL);
            if (r < 0) {
                LOG_WARN("BT: RegisterProfile failed: %s", err.message);
            } else {
                LOG_INFO("BT: SPP profile registered");
            }
        }
        sd_bus_error_free(&err);
        sd_bus_message_unref(m);
    }

    LOG_INFO("BT initialized (%s)", cfg->device_name);
    return ERR_OK;
}

void bt_manager_shutdown(void)
{
    s_bt.running = false;
}

void bt_manager_deinit(void)
{
    bt_manager_shutdown();
    if (s_bt.thread) { pthread_join(s_bt.thread, NULL); s_bt.thread = 0; }

    if (s_bt.bus) {
        adapter_set_bool("Powered", 0);
        sd_bus_unref(s_bt.bus);
        s_bt.bus = NULL;
    }
    s_bt.vtable_slot = NULL;
    s_bt.connected = false;
    LOG_INFO("BT deinitialized");
}

bool bt_manager_is_connected(void)
{
    return s_bt.connected;
}

/* ==================================================================
 * 模块注册
 * ================================================================ */

static err_t bt_init(void)
{
    static bt_manager_config_t s_cfg = {
        .device_name = "AI-Speaker",
        .discoverable_timeout = 300,
        .auto_reconnect = true,
    };
    err_t err = bt_manager_init(&s_cfg);
    if (IS_ERR(err)) {
        LOG_WARN("BT hardware unavailable, continuing without Bluetooth");
        return ERR_OK;
    }
    return ERR_OK;
}

static void bt_deinit(void) { bt_manager_deinit(); }
static void bt_shutdown(void) { bt_manager_shutdown(); }

MODULE_DEFINE(bt, bt_init, bt_deinit, bt_shutdown, MODULE_PRIO_SERVICE);
