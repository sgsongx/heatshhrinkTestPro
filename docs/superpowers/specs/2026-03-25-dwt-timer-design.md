# DWT 计时模块设计规范

**日期**: 2026-03-25  
**范围**: STM32F103ZET6 微控制器  
**功能**: 使用 DWT (Data Watchpoint and Trace) 计数器实现纳秒级精确计时

---

## 1. 概述

### 1.1 需求
实现基于 DWT 的计时功能，提供 3 个核心函数：
- 启动计时
- 停止计时  
- 获取经过时间（支持可配置的时间单位缩放）

### 1.2 核心特性
- **多计时器支持**：同时支持多个独立的计时器（基于索引管理）
- **纳秒级精度**：利用 DWT CYCCNT 计数器，在 72MHz 系统频率下精度为 ~13.9ns/cnt
- **灵活的时间单位**：通过缩放因子参数实现任意倍数的时间单位（如 10ns、100ns）
- **低开销**：轻量级实现，最小化内存占用和 CPU 开销

---

## 2. 硬件基础

### 2.1 DWT 计数器原理
- **CYCCNT 寄存器**：CPU 周期计数器，每个 CPU 时钟递增 1
- **系统时钟**：STM32F103 配置为 72MHz
- **计时精度**：1 / 72MHz ≈ 13.9 ns（单个计数周期的时间）
- **计时范围**：32 位计数器，最大计数 2³² ≈ 59.7 秒（@72MHz）

### 2.2 DWT 初始化
- 启用 DEMCR 中的 TRCENA 位（跟踪使能）
- 重置 CYCCNT 计数器为 0
- CYCCNT 自动计数，无需额外配置

---

## 3. API 设计

### 3.1 初始化函数
```c
/**
 * @brief 初始化 DWT 计时模块
 * 
 * 功能：
 *   - 启用 DWT 计数器
 *   - 重置所有计时器状态
 * 
 * 使用时机：系统启动时调用一次
 */
void dwt_init(void);
```

### 3.2 启动计时函数
```c
/**
 * @brief 启动一个新的计时器
 * 
 * 返回值：
 *   - 成功：计时器索引 (0 ~ DWT_MAX_TIMERS-1)
 *   - 失败：返回 DWT_TIMER_INVALID (0xFF)，表示没有可用计时器
 * 
 * 行为：
 *   - 寻找第一个未运行的计时器位置
 *   - 记录当前 CYCCNT 值作为开始时间
 *   - 标记计时器为运行状态
 */
uint8_t dwt_start(void);
```

### 3.3 停止计时函数
```c
/**
 * @brief 停止指定计时器
 * 
 * 参数：
 *   @timer_id: 计时器索引 (从 dwt_start() 获得)
 * 
 * 行为：
 *   - 验证计时器索引有效性
 *   - 记录当前 CYCCNT 值作为停止时间
 *   - 标记计时器为停止状态
 * 
 * 注意：停止计时后仍可调用 dwt_get_elapsed() 获取已记录的时间差
 */
void dwt_stop(uint8_t timer_id);
```

### 3.4 获取经过时间函数
```c
/**
 * @brief 获取计时器的经过时间
 * 
 * 参数：
 *   @timer_id: 计时器索引
 *   @scale:    时间单位缩放因子
 *              - scale = 10  → 返回单位为 10ns 的时间值
 *              - scale = 100 → 返回单位为 100ns 的时间值
 *              - scale = 1   → 返回单位为 1 (=~13.9ns) 的CPU周期数
 * 
 * 返回值：
 *   经过的时间，单位为 (scale × 13.9ns) 或 (scale × CPU周期)
 *   计算公式：(stop_count - start_count) / scale
 * 
 * 行为：
 *   - 对于运行中的计时器：返回从开始至当前时刻的耗时
 *   - 对于已停止的计时器：返回从开始至停止时刻的耗时
 * 
 * 注意：
 *   - 如果 CYCCNT 溢出（32位满），硬件自动回绕至 0
 *   - 若计算结果不能整除 scale，使用整数除法（截断）
 */
uint32_t dwt_get_elapsed(uint8_t timer_id, uint32_t scale);
```

---

## 4. 数据结构

### 4.1 计时器结构体
```c
typedef struct {
    uint32_t start_count;  // CYCCNT 开始值
    uint32_t stop_count;   // CYCCNT 停止值
    uint8_t  is_running;   // 状态标志: 1=运行中, 0=空闲或已停止
} dwt_timer_t;
```

### 4.2 全局计时器池
```c
#define DWT_MAX_TIMERS 4  // 最多支持 4 个计时器
#define DWT_TIMER_INVALID 0xFF  // 无效计时器索引

static dwt_timer_t g_dwt_timers[DWT_MAX_TIMERS];
```

---

## 5. 实现细节

### 5.1 初始化流程
1. 读取 DEMCR 寄存器
2. 设置 TRCENA 位（第 24 位）
3. 重置 CYCCNT 计数器为 0
4. 初始化全局计时器数组：所有 `is_running` 设为 0

### 5.2 启动计时流程
1. 遍历 `g_dwt_timers` 数组，寻找 `is_running == 0` 的项
2. 若找到空闲计时器：
   - 读取当前 CYCCNT 值存入 `start_count`
   - 设置 `is_running = 1`
   - 返回计时器索引
3. 若全部计时器忙碌，返回 `DWT_TIMER_INVALID`

### 5.3 停止计时流程
1. 验证 `timer_id < DWT_MAX_TIMERS`
2. 验证 `g_dwt_timers[timer_id].is_running == 1`
3. 读取当前 CYCCNT 值存入 `stop_count`
4. 设置 `is_running = 0`

### 5.4 获取经过时间流程
1. 验证 `timer_id < DWT_MAX_TIMERS`
2. 若计时器仍运行：`elapsed = 读取当前CYCCNT - start_count`
3. 若计时器已停止：`elapsed = stop_count - start_count`
4. 返回 `elapsed / scale`（整数除法）

### 5.5 CYCCNT 溢出处理
- CYCCNT 是 32 位无符号数，最大值 2³²-1
- 溢出后自动回绕至 0
- 计算时间差用 32 位无符号减法，可自动处理一次溢出
- 若需处理多次溢出，实现可扩展为 64 位虚拟计数器（需权衡复杂度）

---

## 6. 集成方案

### 6.1 文件结构
```
Core/
├── Inc/
│   └── dwt_timer.h          (头文件，导出 API)
└── Src/
    └── dwt_timer.c          (实现文件)
```

### 6.2 依赖
- **系统文件**：`stm32f1xx.h`（CMSIS 定义，包含 DWT 寄存器定义）
- **HAL**：无直接依赖，纯寄存器操作

### 6.3 集成到 main.c
在 `main()` 中调用初始化：
```c
int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    
    dwt_init();  // 初始化计时模块
    
    while (1) {
        // 使用示例
        uint8_t timer = dwt_start();
        // ... 执行测量的代码 ...
        uint32_t elapsed = dwt_get_elapsed(timer, 10);  // 单位: 10ns
    }
}
```

---

## 7. 使用示例

### 7.1 基本用法
```c
dwt_init();

// 示例 1: 单个计时器
uint8_t timer1 = dwt_start();
// ... 执行代码 ...
uint32_t elapsed_ticks = dwt_get_elapsed(timer1, 1);     // CPU 周期数

// 示例 2: 时间单位转换
uint32_t time_10ns = dwt_get_elapsed(timer1, 10);   // 单位: 10ns
uint32_t time_100ns = dwt_get_elapsed(timer1, 100); // 单位: 100ns

// 示例 3: 多计时器并行运行
uint8_t timer2 = dwt_start();
uint8_t timer3 = dwt_start();
// ... 代码 ...
uint32_t t2_elapsed = dwt_get_elapsed(timer2, 100);
uint32_t t3_elapsed = dwt_get_elapsed(timer3, 100);

// 示例 4: 停止并读取
dwt_stop(timer1);
uint32_t final_time = dwt_get_elapsed(timer1, 10);  // 必为固定值
```

---

## 8. 测试计划

### 8.1 单元测试项
1. **初始化测试**：验证 DWT 启用且计数器重置
2. **多计时器分配**：验证可同时启动多个计时器
3. **计时精度**：验证计时误差 < 1%(依赖硬件精度)
4. **边界条件**：
   - 计时器满时的处理
   - 无效索引的处理
   - 重复停止同一计时器的行为
5. **CYCCNT 溢出**：验证长时间计时的正确性

### 8.2 集成测试
- 在 LED 闪烁任务中测量执行时间
- 验证输出时间合理性

---

## 9. 性能预期

| 指标 | 值 |
|------|-----|
| 时间精度 | ~13.9 ns (@ 72MHz) |
| 最大计时长度 | ~59.7 秒 (32位@72MHz) |
| 内存占用 | 32 字节 (4×8字节结构体) |
| 初始化耗时 | < 1 µs |
| 启动/停止耗时 | < 500 ns |
| 获取耗时耗时 | < 500 ns |

---

## 10. 修改历史

| 版本 | 日期 | 变更 |
|------|------|------|
| 1.0 | 2026-03-25 | 初始设计规范 |
