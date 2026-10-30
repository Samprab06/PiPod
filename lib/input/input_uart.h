/**
 * @file input_uart.h
 * @brief Keyboard control over the stdio UART (the on-board debug probe).
 *
 * Keys (case matters where noted):
 *   w / k / Up          wheel counter-clockwise (-1 tick)
 *   s / j / Down        wheel clockwise (+1 tick)
 *   Enter / e           SELECT
 *   m / Backspace / ESC alone   MENU
 *   space               PLAY_PAUSE
 *   n / Right           NEXT
 *   p / Left            PREV
 *   h                   go to the home screen (ui_go_home)
 *   P (shift-p)         performance report (see input_uart_set_perf_hook)
 *
 * A key press posts the matching button down and up back-to-back: the UI
 * derives clicks from those edges, exactly as with a real button.
 * Escape sequences split across polls (ESC then '[' then 'A') are handled;
 * a lone ESC is reported as MENU after ESC_TIMEOUT_US without a follow-up.
 *
 * Threading: call input_uart_poll() from one core (core 0 in the demo). The
 * application must have initialised stdio (stdio_init_all) first.
 */

#ifndef INPUT_UART_H
#define INPUT_UART_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Called when 'P' is pressed. Typically prints gathered performance data.
 *  NULL = an informational line is printed instead. */
typedef void (*input_uart_perf_hook_t)(void);

/** @brief Reset the escape-sequence state. Safe to call before stdio is up. */
void input_uart_init(void);

/** @brief Install the handler for the 'P' key (may be NULL). */
void input_uart_set_perf_hook(input_uart_perf_hook_t hook);

/** @brief Drain available UART bytes and post abstract input events.
 *  Non-blocking; call from the main loop. */
void input_uart_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_UART_H */
