/**
 * @file audio_load_sim.c
 * @brief Fake I2S interrupt jitter meter + duty-cycled core-0 busy work.
 */

#include "audio_load_sim.h"

#include <stdio.h>

#include "pico/time.h"

/* Timer ISR state. Only the callback writes these; the main loop reads them
 * for the report. 32-bit reads/writes are atomic on RP2350, and diagnostics
 * tolerate a torn 64-bit sum. */
static volatile uint32_t s_callbacks;
static volatile uint32_t s_jitter_sum_us;
static volatile uint32_t s_jitter_max_us;
static volatile uint32_t s_missed;
static volatile uint32_t s_over_limit;
static uint64_t s_expected_us;

static repeating_timer_t s_timer;
static bool s_timer_started;

static volatile uint8_t s_busy_pct = AUDIO_SIM_DEFAULT_BUSY_PCT;
static uint64_t s_last_wall_us;
static uint64_t s_busy_debt_us;
static uint32_t s_mix = 0x5EED1234u;

static void reset_counters(void) {
    s_callbacks = 0;
    s_jitter_sum_us = 0;
    s_jitter_max_us = 0;
    s_missed = 0;
    s_over_limit = 0;
}

static bool audio_timer_cb(repeating_timer_t *timer) {
    (void)timer;
    uint64_t now = time_us_64();

    s_expected_us += AUDIO_SIM_PERIOD_US;
    int64_t late = (int64_t)now - (int64_t)s_expected_us;
    uint32_t jitter = (uint32_t)(late < 0 ? -late : late);

    s_jitter_sum_us += jitter;
    if (jitter > s_jitter_max_us) {
        s_jitter_max_us = jitter;
    }
    if (jitter > AUDIO_SIM_MISS_US) {
        s_missed++;
    }
    if (jitter > AUDIO_SIM_JITTER_LIMIT_US) {
        s_over_limit++;
    }
    s_callbacks++;

    /* A real DMA-half-complete handler would kick the next buffer here; keep
     * the ISR short so the measurement reflects scheduling, not our work. */
    return true;
}

void audio_sim_init(uint8_t busy_percent) {
    reset_counters();
    s_expected_us = time_us_64() + AUDIO_SIM_PERIOD_US;
    s_timer_started = add_repeating_timer_us(AUDIO_SIM_PERIOD_US, audio_timer_cb, NULL, &s_timer);

    audio_sim_set_busy_percent(busy_percent);
    s_last_wall_us = time_us_64();
    s_busy_debt_us = 0;

    if (!s_timer_started) {
        printf("[audio] WARNING: 1 ms repeating timer could not be started\n");
    }
}

void audio_sim_set_busy_percent(uint8_t percent) {
    if (percent > 95u) {
        percent = 95u;
    }
    s_busy_pct = percent;
}

void audio_sim_busy_work(void) {
    uint8_t pct = s_busy_pct;
    uint64_t now = time_us_64();
    uint64_t elapsed = now - s_last_wall_us;
    s_last_wall_us = now;

    if (pct == 0u) {
        s_busy_debt_us = 0;
        return;
    }

    /* Owed busy time since the last call. Capped so a stall elsewhere in the
     * loop cannot turn into one huge burst. */
    s_busy_debt_us += elapsed * pct / 100u;
    if (s_busy_debt_us > 10000u) {
        s_busy_debt_us = 10000u;
    }

    uint64_t start = time_us_64();
    uint32_t mix = s_mix;
    while ((time_us_64() - start) < s_busy_debt_us) {
        /* A few hundred instructions per 64-bit time check: ~decoder work. */
        for (int i = 0; i < 32; i++) {
            mix = mix * 1664525u + 1013904223u;
        }
    }
    s_mix = mix;

    uint64_t spent = time_us_64() - start;
    s_busy_debt_us = (spent < s_busy_debt_us) ? (s_busy_debt_us - spent) : 0;
}

void audio_sim_get_stats(audio_sim_stats_t *out, bool reset) {
    if (out == NULL) {
        return;
    }
    uint32_t calls = s_callbacks;
    uint32_t sum = s_jitter_sum_us;
    out->callbacks = calls;
    out->jitter_avg_us = (calls != 0u) ? (sum / calls) : 0u;
    out->jitter_max_us = s_jitter_max_us;
    out->missed_deadlines = s_missed;
    out->over_limit = s_over_limit;
    out->busy_percent = s_busy_pct;

    if (reset) {
        reset_counters();
    }
}

void audio_sim_reset(void) {
    reset_counters();
}

void audio_sim_print_report(bool reset) {
    audio_sim_stats_t st;
    audio_sim_get_stats(&st, false);
    printf("[audio] 1ms irq: cb %lu  jitter avg %lu us  max %lu us  missed %lu  "
           "over100us %lu  busy %u%%\n",
           (unsigned long)st.callbacks, (unsigned long)st.jitter_avg_us,
           (unsigned long)st.jitter_max_us, (unsigned long)st.missed_deadlines,
           (unsigned long)st.over_limit, (unsigned)st.busy_percent);
    if (reset) {
        /* Reset after the blocking UART write, so its stall is excluded from
         * the next measurement window. */
        reset_counters();
    }
}
