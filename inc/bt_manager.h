  #ifndef BT_MANAGER_H
  #define BT_MANAGER_H

  #include "common.h"

  /*
   * 蓝牙管理器 — 命令行+RFCOMM socket 方案
   *
   * init:   打开蓝牙 → 设名称 → 可发现/可连接
   * deinit: 关闭蓝牙
   * 状态变化通过事件总线通知
   */

  typedef struct {
      const char *device_name;           /* 蓝牙设备名 */
      int         discoverable_timeout;  /* 可发现超时（秒） */
      bool        auto_reconnect;        /* 断线自动重连 */
  } bt_manager_config_t;

  err_t bt_manager_init(const bt_manager_config_t *cfg);// 初始化蓝牙
  void bt_manager_deinit(void);//关闭蓝牙，清理资源
  void bt_manager_shutdown(void);//信号上下文调用，不能阻塞、不能调 system()

  bool bt_manager_is_connected(void);//查询当前是否有手机连接

  #endif /* BT_MANAGER_H */