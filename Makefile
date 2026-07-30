  # 项目名称和版本
  PROJECT := speaker
  VERSION := 1.0.0

  # ?= 的意思是：如果环境变量没设 CC，就用 gcc
  # 这样以后想交叉编译时，make CC=aarch64-linux-gnu-gcc 就行
  CC      ?= gcc

  # CFLAGS 编译标志
  #   -Iinc           → 头文件搜索路径（找 #include "xxx.h"）
  #   -Wall -Wextra   → 开启所有警告
  #   -Werror         → 把警告当错误（强迫你写干净代码）
  #   -std=c11        → C11 标准
  #   -D_GNU_SOURCE   → 定义 _GNU_SOURCE 宏（让 sigaction、strdup 等可用）
  CFLAGS  := -Iinc -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE

  # 链接库：pthread（线程）和 m（数学库）
  LDFLAGS := -lpthread -lm -lbluetooth -lcurl -lasound -lsystemd -lssl -lcrypto
  # 源代码目录
  SRC_DIR   := src
  # 编译中间产物（.o, .d）
  BUILD_DIR := build
  # 最终可执行文件输出目录
  OUT_DIR   := output

  # 所有源文件列表（\ 是续行符）
  SRCS := \
      $(SRC_DIR)/main.c \
      $(SRC_DIR)/core/event_bus.c \
      $(SRC_DIR)/core/state_machine.c \
      $(SRC_DIR)/core/thread_pool.c \
      $(SRC_DIR)/core/voice_agent.c \
      $(SRC_DIR)/core/module.c \
      $(SRC_DIR)/utils/logger.c \
      $(SRC_DIR)/utils/config.c \
      $(SRC_DIR)/bluetooth/bt_manager.c\
      $(SRC_DIR)/bluetooth/bt_a2dp.c \
      $(SRC_DIR)/ai/ai_client.c \
      $(SRC_DIR)/ai/ai_conv.c \
      $(SRC_DIR)/ai/ai_tts.c \
      $(SRC_DIR)/ai/ai_stt.c \
      $(SRC_DIR)/audio/audio_player.c\
      $(SRC_DIR)/audio/alsa_capture.c \
      $(SRC_DIR)/audio/audio_mixer.c \
      $(SRC_DIR)/audio/audio_pipeline.c

  # 把 src/xxx.c → build/xxx.o
  # patsubst = pattern substitution（模式替换）
  OBJS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

  # 自动生成的依赖文件（.d），记录每个 .o 依赖哪些头文件
  DEPS := $(OBJS:.o=.d)

  TARGET := $(OUT_DIR)/$(PROJECT)   # 最终产物：output/speaker

  # .PHONY 声明"伪目标"——all 和 clean 不是文件名，是命令
  .PHONY: all clean test

  # 默认目标（因为排第一个）
all: $(TARGET)

  # 测试目标：编译并运行单元测试（pure logic, no hw dependency）
  TEST_SRC := tests/test_event_bus.c tests/test_state_machine.c tests/test_thread_pool.c
  TEST_BIN := $(TEST_SRC:.c=)

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do \
	    echo "Running $$t..."; \
	    ./$$t; \
	    echo; \
	done

  # 编译测试
tests/test_event_bus: tests/test_event_bus.c $(SRC_DIR)/core/event_bus.c $(SRC_DIR)/core/module.c $(SRC_DIR)/utils/logger.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

tests/test_state_machine: tests/test_state_machine.c $(SRC_DIR)/core/state_machine.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

tests/test_thread_pool: tests/test_thread_pool.c $(SRC_DIR)/core/thread_pool.c $(SRC_DIR)/core/module.c $(SRC_DIR)/utils/logger.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

  # 把依赖文件包含进来（- 表示文件不存在也不报错）
  -include $(DEPS)

  # 通用的编译规则：
  # 把 src/ 下的 .c 编译成 build/ 下的 .o
  #   $<  = 第一个依赖（.c 文件）
  #   $@  = 目标（.o 文件）
  #   -MMD -MP = 自动生成头文件依赖关系到 .d 文件
  $(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)              # 创建目标目录（比如 build/core/）
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

  # 链接所有 .o 成可执行文件
  $(TARGET): $(OBJS)
	@mkdir -p $(OUT_DIR)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

  # 删除构建产物
clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
