#ifndef VOICE_AGENT_H
#define VOICE_AGENT_H

#include "common.h"

/* 状态常量 */
  #define ST_IDLE       0//空闲，啥也没干，等事件
  #define ST_MUSIC      1//正在播蓝牙音乐（A2DP）
  #define ST_PROCESSING 2//在等 AI API 返回结果
  #define ST_SPEAKING   3//TTS 语音正在播放
  #define ST_EXITING    4//收到 Ctrl+C，准备退出

  /*
 * 语音助手 — 应用层主控状态机，项目唯一编排器
 *
 * 职责：
 *  状态表驱动、事件路由、任务调度
 *  只通过 event_bus subscribe/publish 通信
 *  AI 请求通过 thread_pool 异步执行
 */
  err_t voice_agent_init(void);
  // init — 初始化所有子模块 + 订阅事件 + 初始化状态机
  void voice_agent_deinit(void);
  //收到关闭信号时调（当前是空函数，留到后续）
  void voice_agent_shutdown(void);
  //反初始化：退订事件 + 释放子模块
  #endif /* VOICE_AGENT_H */