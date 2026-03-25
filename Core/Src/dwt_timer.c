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
