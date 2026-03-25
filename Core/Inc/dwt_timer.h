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
