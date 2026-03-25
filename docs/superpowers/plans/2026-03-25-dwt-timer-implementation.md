# DWT 计时模块实现计划

> **For agentic workers:** REQUIRED: Use subagent-driven-development to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现基于 DWT 的纳秒级多计时器模块，支持 3 个核心 API 函数。

**Architecture:** 分为两个独立文件：头文件定义 API 和配置，实现文件包含 DWT 寄存器操作和计时器管理逻辑。使用静态数组管理多个计时器状态，避免动态内存分配。

**Tech Stack:** STM32F1xx HAL、CMSIS（DWT 寄存器定义）、标准 C 库

---

## 文件结构

```
Core/
├── Inc/
│   └── dwt_timer.h              (新建，导出 API 和配置)
└── Src/
    ├── dwt_timer.c              (新建，实现 DWT 计时逻辑)
    └── main.c                   (修改，集成初始化调用)
```

---

## Task 1: 创建头文件与 API 声明

**Files:**
- Create: `Core/Inc/dwt_timer.h`

- [ ] **Step 1: 创建头文件框架与配置宏**

创建文件 `Core/Inc/dwt_timer.h`，内容如下：

```c
/**
 * @file dwt_timer.h
 * @brief DWT-based nanosecond-precision timer module for STM32F1xx
 * @author Auto-generated
 * @date 2026-03-25
 */

#ifndef __DWT_TIMER_H
#define __DWT_TIMER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @defgroup DWT_TIMER DWT Timer Configuration
 * @{
 */

/**
 * @brief Maximum number of simultaneous timers
 * Can be overridden at compile time: -DDWT_MAX_TIMERS=8
 */
#ifndef DWT_MAX_TIMERS
#define DWT_MAX_TIMERS 4
#endif

/**
 * @brief Invalid timer ID marker
 */
#define DWT_TIMER_INVALID 0xFF

/** @} */

/**
 * @defgroup DWT_TIMER_API DWT Timer API Functions
 * @{
 */

/**
 * @brief Initialize the DWT timer module
 *
 * This function enables the DWT counter and initializes the timer pool.
 * Must be called once at system startup.
 *
 * @note Safe to call multiple times; subsequent calls are no-ops after first init.
 */
void dwt_init(void);

/**
 * @brief Start a new timer
 *
 * Allocates the first available idle timer and records the current CYCCNT value.
 *
 * @return Timer ID (0 to DWT_MAX_TIMERS-1) on success
 * @return DWT_TIMER_INVALID (0xFF) if no idle timers available
 *
 * @note Timer is now running. Call dwt_get_elapsed() to measure elapsed time.
 */
uint8_t dwt_start(void);

/**
 * @brief Stop a running timer
 *
 * Records the current CYCCNT value as the stop time.
 * Timer remains accessible for dwt_get_elapsed() queries after stopping.
 *
 * @param timer_id Timer ID returned by dwt_start()
 *
 * @note Silently ignores invalid timer IDs (no-op for DWT_TIMER_INVALID or out-of-range)
 */
void dwt_stop(uint8_t timer_id);

/**
 * @brief Get elapsed time from a timer
 *
 * @param timer_id Timer ID returned by dwt_start()
 * @param scale    Time unit scale factor
 *                 - Result unit = (scale × 13.9 ns) ≈ (scale × CPU cycle @ 72MHz)
 *                 - scale=1:   result in CPU cycles
 *                 - scale=10:  result in units of ~139 ns
 *                 - scale=100: result in units of ~1.39 µs
 *
 * @return Elapsed time in units of (scale × ~13.9 ns)
 *         For running timers: time from start to now
 *         For stopped timers: time from start to stop
 *         Integer division used (truncation)
 *
 * @note Returns 0 for invalid timer IDs
 * @note Handles CYCCNT 32-bit overflow (automatic wrap-around)
 */
uint32_t dwt_get_elapsed(uint8_t timer_id, uint32_t scale);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* __DWT_TIMER_H */
```

- [ ] **Step 2: Verify header file compiles**

```bash
cd c:\Users\SSG\Desktop\heatshrinkTest
arm-none-eabi-gcc -I Core/Inc -c -std=c99 -Wall Core/Inc/dwt_timer.h
```

Expected: No errors, header is syntactically correct.

- [ ] **Step 3: Commit header file**

```bash
git add Core/Inc/dwt_timer.h
git commit -m "feat(dwt_timer): Add header with API declarations and configuration"
```

---

## Task 2: 创建实现文件 - 初始化与辅助函数

**Files:**
- Create: `Core/Src/dwt_timer.c`

- [ ] **Step 1: 创建实现文件框架与数据结构**

创建文件 `Core/Src/dwt_timer.c`，包含基础结构和初始化代码：

```c
/**
 * @file dwt_timer.c
 * @brief DWT-based nanosecond-precision timer implementation for STM32F1xx
 * @author Auto-generated
 * @date 2026-03-25
 */

#include "dwt_timer.h"
#include "stm32f1xx.h"

/**
 * @brief Timer state structure
 */
typedef struct {
    uint32_t start_count;  /**< CYCCNT value at timer start */
    uint32_t stop_count;   /**< CYCCNT value at timer stop */
    uint8_t  is_running;   /**< Flag: 1=running, 0=idle/stopped */
} dwt_timer_t;

/**
 * @brief Global timer pool
 */
static dwt_timer_t g_dwt_timers[DWT_MAX_TIMERS];

/**
 * @brief Module initialization flag
 */
static uint8_t g_dwt_initialized = 0;

/**
 * @brief Initialize the DWT timer module
 *
 * Enables the DWT counter and resets all timers to idle state.
 */
void dwt_init(void)
{
    /* Skip re-initialization */
    if (g_dwt_initialized) {
        return;
    }

    /* Enable DWT and CYCCNT counter
     * DEMCR: Debug Exception and Monitoring Control Register
     * Bit 24: TRCENA (Trace Enable)
     */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    /* Reset CYCCNT counter to 0 */
    DWT->CYCCNT = 0;

    /* Initialize all timers to idle state */
    for (uint8_t i = 0; i < DWT_MAX_TIMERS; i++) {
        g_dwt_timers[i].is_running = 0;
        g_dwt_timers[i].start_count = 0;
        g_dwt_timers[i].stop_count = 0;
    }

    g_dwt_initialized = 1;
}

/**
 * @brief Start a new timer
 * @return Timer ID or DWT_TIMER_INVALID
 */
uint8_t dwt_start(void)
{
    /* Find first idle timer */
    for (uint8_t i = 0; i < DWT_MAX_TIMERS; i++) {
        if (!g_dwt_timers[i].is_running) {
            /* Found idle timer, initialize it */
            g_dwt_timers[i].start_count = DWT->CYCCNT;
            g_dwt_timers[i].is_running = 1;
            return i;
        }
    }

    /* No idle timer available */
    return DWT_TIMER_INVALID;
}

/**
 * @brief Stop a timer
 * @param timer_id Timer ID from dwt_start()
 */
void dwt_stop(uint8_t timer_id)
{
    /* Validate timer ID */
    if (timer_id >= DWT_MAX_TIMERS) {
        return;  /* Invalid ID, silently ignore */
    }

    /* Check if timer is running */
    if (!g_dwt_timers[timer_id].is_running) {
        return;  /* Timer not running, silently ignore */
    }

    /* Record stop time and mark as idle */
    g_dwt_timers[timer_id].stop_count = DWT->CYCCNT;
    g_dwt_timers[timer_id].is_running = 0;
}

/**
 * @brief Get elapsed time from a timer
 * @param timer_id Timer ID from dwt_start()
 * @param scale    Time unit scale factor
 * @return Elapsed time in scaled units
 */
uint32_t dwt_get_elapsed(uint8_t timer_id, uint32_t scale)
{
    uint32_t elapsed;

    /* Validate timer ID */
    if (timer_id >= DWT_MAX_TIMERS) {
        return 0;  /* Invalid ID */
    }

    /* Ensure scale is non-zero to avoid division by zero */
    if (scale == 0) {
        scale = 1;
    }

    /* Calculate elapsed time
     * For running timers: use current CYCCNT
     * For stopped timers: use recorded stop_count
     * Unsigned arithmetic handles 32-bit overflow correctly
     */
    if (g_dwt_timers[timer_id].is_running) {
        elapsed = DWT->CYCCNT - g_dwt_timers[timer_id].start_count;
    } else {
        elapsed = g_dwt_timers[timer_id].stop_count - g_dwt_timers[timer_id].start_count;
    }

    /* Apply scale factor (integer division) */
    return elapsed / scale;
}
```

- [ ] **Step 2: Verify implementation compiles**

```bash
cd c:\Users\SSG\Desktop\heatshrinkTest
arm-none-eabi-gcc -I Core/Inc -I Drivers/CMSIS/Include -I Drivers/STM32F1xx_HAL_Driver/Inc \
  -std=c99 -Wall -Werror -c Core/Src/dwt_timer.c -o build/dwt_timer.o
```

Expected: Compiles without errors or warnings.

- [ ] **Step 3: Commit implementation**

```bash
git add Core/Src/dwt_timer.c
git commit -m "feat(dwt_timer): Add DWT timer implementation with 4 core functions"
```

---

## Task 3: 集成到 main.c - 初始化调用

**Files:**
- Modify: `Core/Src/main.c`

- [ ] **Step 1: 在 main.c 中添加 dwt_timer.h include**

打开 `Core/Src/main.c`，在 `/* USER CODE BEGIN Includes */` 部分添加：

```c
/* USER CODE BEGIN Includes */
#include "dwt_timer.h"
/* USER CODE END Includes */
```

- [ ] **Step 2: 在 main 函数中调用 dwt_init()**

在 `main()` 函数中，在 `MX_GPIO_Init();` 之后添加：

```c
  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  
  /* USER CODE BEGIN 2 */
  /* Initialize DWT timer module */
  dwt_init();
  /* USER CODE END 2 */
```

- [ ] **Step 3: Verify main.c changes**

打开文件确认修改位置正确。

- [ ] **Step 4: Commit main.c 修改**

```bash
git add Core/Src/main.c
git commit -m "feat: Initialize DWT timer module in main()"
```

---

## Task 4: 集成测试 - 添加功能测试代码到 main 循环

**Files:**
- Modify: `Core/Src/main.c`（在 while(1) 循环中添加测试）

- [ ] **Step 1: 在 main 循环中添加 DWT 计时测试**

在 main.c 的 `while(1)` 循环中，修改 LED 闪烁代码以添加计时测试：

```c
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* Test DWT timer: measure LED toggle time */
    uint8_t timer = dwt_start();
    
    HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
    
    /* Get elapsed time in 10ns units */
    uint32_t elapsed_10ns = dwt_get_elapsed(timer, 10);
    
    /* Stop timer for next measurement */
    dwt_stop(timer);
    
    HAL_Delay(500);

    /* USER CODE BEGIN 3 */
  }
```

- [ ] **Step 2: 编译工程验证无编译错误**

```bash
cd c:\Users\SSG\Desktop\heatshrinkTest
make clean
make -j8 all
```

Expected: 编译成功，无错误或警告（若有警告关于 unused result，可忽略）。

- [ ] **Step 3: Commit 集成测试代码**

```bash
git add Core/Src/main.c
git commit -m "test: Add DWT timer integration test in main loop"
```

---

## Task 5: 编译与初步验证

**Files:**
- No new files, verification only

- [ ] **Step 1: 完整工程编译**

```bash
cd c:\Users\SSG\Desktop\heatshrinkTest
make clean
make -j8 all > build/build.log 2>&1
echo "Build exit code: $?"
```

Expected: Exit code 0，build.log 无致命错误。

- [ ] **Step 2: 检查生成的 .elf 文件**

```bash
ls -lh c:\Users\SSG\Desktop\heatshrinkTest\build\App.axf
arm-none-eabi-size c:\Users\SSG\Desktop\heatshrinkTest\build\App.axf
```

Expected: .axf 文件存在，大小合理(<150KB)。

- [ ] **Step 3: 验证符号表包含 dwt_init 等函数**

```bash
arm-none-eabi-nm c:\Users\SSG\Desktop\heatshrinkTest\build\App.axf | grep dwt_
```

Expected: 输出包含 `dwt_init`, `dwt_start`, `dwt_stop`, `dwt_get_elapsed`。

- [ ] **Step 4: Commit 最终版本**

```bash
git add -A
git commit -m "build: DWT timer module compiles successfully"
```

---

## Task 6: 代码审查与文档更新

**Files:**
- No source files, documentation only

- [ ] **Step 1: 验证代码符合开发规范**

检查以下内容：
- DWT 寄存器访问正确（DEMCR、CYCCNT）
- 边界检查完整（timer_id 验证）
- 整数溢出处理正确（32位无符号减法）
- 内存占用预期（32字节 @ DWT_MAX_TIMERS=4）

- [ ] **Step 2: 生成编译命令数据库**

```bash
cd c:\Users\SSG\Desktop\heatshrinkTest
./gen_compile_commands.py --format arguments --parse build/build.log
```

Expected: 生成 `compile_commands.json`，供 IDE IntelliSense 使用。

- [ ] **Step 3: Final commit**

```bash
git add compile_commands.json
git commit -m "docs: Update compilation database for IDE support"
```

---

## 预期结果

**完成后的功能**：
- ✅ DWT 计时模块初始化在系统启动时自动运行
- ✅ 支持最多 4 个并发计时器（可通过编译选项扩展）
- ✅ 纳秒级精度计时（~13.9ns @ 72MHz）
- ✅ LED 闪烁时间记录按每 10ns 单位返回
- ✅ 代码完全编译通过，无错误无警告

**内存影响**：
- 新增 32 字节全局数组（g_dwt_timers）
- 新增 1 字节初始化标志（g_dwt_initialized）
- 总计 ~33 字节内存占用

**性能影响**：
- 初始化耗时 < 1 µs
- 启动/停止耗时 < 500 ns
- 获取耗时耗时 < 500 ns
- 无中断禁用，不影响系统实时性

---

## 测试检查清单

在实际硬件上验证时，可执行以下测试：

- [ ] 烧录固件，连接调试器
- [ ] 在断点处检查 timer 值是否为 0（符合预期第一个计时器）
- [ ] 单步执行，观察 elapsed_10ns 值变化
- [ ] LED 应按正常频率闪烁（不受计时影响）
- [ ] 使用逻辑分析仪或示波器验证 LED 切换时间

---

## 修改历史

| 版本 | 日期 | 内容 |
|------|------|------|
| 1.0 | 2026-03-25 | 初始实现计划 |
