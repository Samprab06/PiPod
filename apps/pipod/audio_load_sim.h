/**
 * @file audio_load_sim.h
 * @brief Fake decoder + I2S workload, to show the UI does not disturb audio.
 *
 * Two independent loads on core 0:
 *  - a repeating 1 ms timer standing in for the I2S DMA half-complete IRQ; it
 *    records callback latency/jitter against an ideal 1 ms schedule, counts
 *    deadlines missed by more than AUDIO_SIM_MISS_US, and separately counts
 *    callbacks whose jitter exceeds AUDIO_SIM_JITTER_LIMIT_US (the UI's
 *    isolation pass criterion).
 *  - a busy-work governor that burns a configurable percentage of core 0,
 *    modelling decode/MP3 work.
 *
 * The numbers are printed by audio_sim_print_report() (main loop only; the IRQ
 * never calls printf).
 */

#ifndef AUDIO_LOAD_SIM_H
#define AUDIO_LOAD_SIM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SIM_DEFAULT_BUSY_PCT 50u
#define AUDIO_SIM_PERIOD_US        1000u
#define AUDIO_SIM_MISS_US          500u /**< later than this = missed deadline */
#define AUDIO_SIM_JITTER_LIMIT_US  100u /**< jitter pass criterion (over100us) */

typedef struct {
    uint32_t callbacks;        /**< timer callbacks seen */
    uint32_t jitter_avg_us;    /**< mean |actual - expected| */
    uint32_t jitter_max_us;    /**< worst |actual - expected| */
    uint32_t missed_deadlines; /**< callbacks later than AUDIO_SIM_MISS_US */
    uint32_t over_limit;       /**< callbacks with jitter > AUDIO_SIM_JITTER_LIMIT_US */
    uint8_t  busy_percent;     /**< requested busy load */
} audio_sim_stats_t;

/** Start the 1 ms timer and set the busy-work percentage (0..95). */
void audio_sim_init(uint8_t busy_percent);

/** Change the busy-work percentage at runtime (0..95). */
void audio_sim_set_busy_percent(uint8_t percent);

/** Burn the busy-work share owed since the previous call. Call in the core-0
 *  main loop; it returns after doing the CPU work, never blocks on the timer. */
void audio_sim_busy_work(void);

/** Snapshot counters, optionally zeroing them. */
void audio_sim_get_stats(audio_sim_stats_t *out, bool reset);

/** Zero the jitter/miss counters. */
void audio_sim_reset(void);

/** Print one "[audio] ..." line to stdio, then reset the counters if
 *  @p reset is true. Reset happens after the UART stall so the next window
 *  measures application behaviour, not the report itself. */
void audio_sim_print_report(bool reset);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_LOAD_SIM_H */
