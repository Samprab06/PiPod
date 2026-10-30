/**
 * @file input_uart.c
 * @brief Non-blocking UART keyboard adapter for the debug probe console.
 */

#include "input_uart.h"

#include <stdint.h>
#include <stdio.h>

#include "pico/stdio.h"
#include "pico/time.h"
#include "ui.h"

#define ESC_TIMEOUT_US 20000u /**< lone ESC becomes MENU after this */

typedef enum {
    PARSER_IDLE = 0,
    PARSER_ESC,  /**< ESC seen, waiting for '[' (or 'O') */
    PARSER_CSI,  /**< ESC [ seen, waiting for the final letter */
} parser_state_t;

static parser_state_t s_state;
static uint64_t s_esc_us;
static input_uart_perf_hook_t s_perf_hook;

static void click(ui_button_t button) {
    (void)ui_post_button(button, true);
    (void)ui_post_button(button, false);
}

static void wheel(int16_t ticks) {
    (void)ui_post_wheel(ticks);
}

static void handle_key(int c) {
    switch (c) {
        case 'w':
        case 'k':
            wheel(-1);
            break;
        case 's':
        case 'j':
            wheel(+1);
            break;
        case '\r':
        case '\n':
        case 'e':
            click(UI_BTN_SELECT);
            break;
        case 'm':
        case 0x7F: /* DEL, most terminals' Backspace */
        case 0x08: /* BS */
            click(UI_BTN_MENU);
            break;
        case ' ':
            click(UI_BTN_PLAY_PAUSE);
            break;
        case 'n':
            click(UI_BTN_NEXT);
            break;
        case 'p':
            click(UI_BTN_PREV);
            break;
        case 'h':
            ui_go_home();
            break;
        case 'P':
            if (s_perf_hook != NULL) {
                s_perf_hook();
            } else {
                printf("[input] perf: no report hook installed\n");
            }
            break;
        default:
            break; /* unknown keys are ignored */
    }
}

static void handle_csi_final(int c) {
    switch (c) {
        case 'A': /* Up */
            wheel(-1);
            break;
        case 'B': /* Down */
            wheel(+1);
            break;
        case 'C': /* Right */
            click(UI_BTN_NEXT);
            break;
        case 'D': /* Left */
            click(UI_BTN_PREV);
            break;
        default:
            break;
    }
}

static void feed(int c) {
    switch (s_state) {
        case PARSER_IDLE:
            if (c == 0x1B) {
                s_state = PARSER_ESC;
                s_esc_us = time_us_64();
            } else {
                handle_key(c);
            }
            break;

        case PARSER_ESC:
            if (c == '[' || c == 'O') { /* CSI and SS3 both end in a letter */
                s_state = PARSER_CSI;
                s_esc_us = time_us_64();
            } else {
                /* ESC was pressed on its own (MENU), then this key follows. */
                s_state = PARSER_IDLE;
                click(UI_BTN_MENU);
                feed(c);
            }
            break;

        case PARSER_CSI:
            s_state = PARSER_IDLE;
            handle_csi_final(c);
            break;

        default:
            s_state = PARSER_IDLE;
            break;
    }
}

void input_uart_init(void) {
    s_state = PARSER_IDLE;
    s_esc_us = 0;
}

void input_uart_set_perf_hook(input_uart_perf_hook_t hook) {
    s_perf_hook = hook;
}

void input_uart_poll(void) {
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        feed(c);
    }

    if (s_state != PARSER_IDLE && (time_us_64() - s_esc_us) > ESC_TIMEOUT_US) {
        /* A lone ESC is MENU; an incomplete CSI sequence is discarded so it
         * cannot swallow the next real key. */
        const parser_state_t state = s_state;
        s_state = PARSER_IDLE;
        if (state == PARSER_ESC) {
            click(UI_BTN_MENU);
        }
    }
}
