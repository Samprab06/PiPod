/* Read-only SD and capacitive-touch bring-up. No UI or flash cache writes. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "pipod_pins.h"
#include "display.h"
#include "gfx.h"
#include "fonts/fonts.h"
#include "input_mpr121.h"
#include "input_clickwheel.h"
#include "media_meta.h"
#include "storage.h"
#include "storage_fs.h"
#include "sd_card.h"
#include "hw_config.h"

#define BG GFX_RGB(12, 18, 28)
#define FG GFX_RGB(235, 240, 245)
#define HIGHLIGHT GFX_RGB(0, 145, 190)

/* A small scratch canvas, never a full-screen framebuffer. Each blit is
 * completed before reusing the buffer. All coordinates are screen-relative. */
static uint16_t s_pixels[480 * 32];
static uint8_t s_scratch[4096];
static char s_songs[5][256];
static uint32_t s_mp3_count;
static unsigned s_song_count;
static input_clickwheel_t s_wheel;
static int32_t s_ticks;
static char s_event[40] = "none";
static bool s_was_hold;

static void wheel_ticks(int16_t ticks, void *user) {
    (void)user;
    s_ticks += ticks;
    snprintf(s_event, sizeof(s_event), "scroll %+d", ticks);
}
static void wheel_button(input_clickwheel_button_t button, bool down, void *user) {
    (void)user;
    static const char *names[] = {"none", "MENU", "NEXT", "PLAY/PAUSE", "PREV", "SELECT"};
    if ((unsigned)button >= count_of(names)) return;
    if (down) s_was_hold = s_wheel.held;
    snprintf(s_event, sizeof(s_event), "%s %s", names[button],
             down ? (s_was_hold ? "hold" : "down") : (s_was_hold ? "up (hold)" : "click"));
}

static gfx_canvas_t canvas(int x, int y, int w, int h) {
    const gfx_rect_t rect = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
    const gfx_canvas_t c = {s_pixels, rect, rect};
    return c;
}

static void flush(const gfx_canvas_t *c) {
    display_blit_async((uint16_t)c->area.x, (uint16_t)c->area.y,
                       (uint16_t)c->area.w, (uint16_t)c->area.h, c->buf);
    display_wait_idle();
}

static void line(int y, const char *text) {
    gfx_canvas_t c = canvas(0, y, 252, 18);
    gfx_fill(&c, BG);
    gfx_draw_text_clipped(&c, &font_inter_12, 8, y + 14, 244, text, FG);
    flush(&c);
}

static void box(unsigned electrode, bool touched) {
    const float rad = (float)electrode * 6.28318530718f / 11.0f;
    const int x = 354 + (electrode == 11 ? 0 : (int)lroundf(70 * sinf(rad)));
    const int y = 204 - (electrode == 11 ? 0 : (int)lroundf(70 * cosf(rad)));
    gfx_canvas_t c = canvas(x, y, 32, 22);
    gfx_fill(&c, touched ? HIGHLIGHT : BG);
    gfx_draw_round_rect(&c, x, y, 32, 22, 4, FG);
    char text[8];
    snprintf(text, sizeof(text), "%u", electrode);
    gfx_draw_text(&c, &font_inter_16sb, x + 5, y + 17, text, FG);
    flush(&c);
}

static void pressed_line(uint16_t mask) {
    char text[80] = "Pressed: ";
    size_t used = strlen(text);
    bool first = true;
    for (unsigned i = 0; i < 12; ++i) {
        if ((mask & (1u << i)) != 0) {
            used += (size_t)snprintf(text + used, sizeof(text) - used,
                                     "%s%u", first ? "" : ", ", i);
            first = false;
        }
    }
    if (first) {
        snprintf(text + used, sizeof(text) - used, "none");
    }
    line(284, text);
}

static void touch_status(void) {
    char text[100];
    if (input_mpr121_present()) {
        snprintf(text, sizeof(text), "found at 0x%02X; I2C error %d",
                 input_mpr121_found_address(), input_mpr121_last_i2c_error());
    } else {
        snprintf(text, sizeof(text), "not found/configured; I2C error %d",
                 input_mpr121_last_i2c_error());
    }
    line(180, text);
}

static void touch_event(uint8_t electrode, bool pressed, void *user) {
    (void)user;
    printf("E%u %s\n", electrode, pressed ? "pressed" : "released");
}

static int32_t song_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    return storage_fs_read((storage_fs_file_t *)ctx, off, dst, len);
}

static bool scan_visit(void *ctx, const storage_fs_entry_t *entry, bool is_dir) {
    (void)ctx;
    if (is_dir) {
        return true;
    }
    const size_t n = strlen(entry->name);
    if (n < 4 || entry->name[n - 4] != '.' ||
        (entry->name[n - 3] != 'm' && entry->name[n - 3] != 'M') ||
        (entry->name[n - 2] != 'p' && entry->name[n - 2] != 'P') ||
        entry->name[n - 1] != '3') {
        return true;
    }
    ++s_mp3_count;
    if (s_song_count >= count_of(s_songs)) {
        return true; /* still count every MP3, not just the display list */
    }
    char *song = s_songs[s_song_count++];
    snprintf(song, sizeof(s_songs[0]), "%s", entry->name);
    storage_fs_file_t *file = storage_fs_open(entry->path);
    if (file != NULL) {
        const media_source_t src = {
            .read = song_read, .size = storage_fs_size(file),
            .xip_base = NULL, .ctx = file,
        };
        media_meta_t meta;
        const media_status_t rc = media_meta_parse(&src, entry->name, &meta,
                                                   s_scratch, sizeof(s_scratch));
        if (rc == MEDIA_OK && meta.title[0] != '\0') {
            snprintf(song, sizeof(s_songs[0]), "%.120s - %.120s", meta.title,
                     meta.artist[0] != '\0' ? meta.artist : "(no artist)");
        }
        storage_fs_close(file);
    }
    return true;
}

static void scan_sd(void) {
    line(0, "SD card");
    line(18, "Mounting / scanning...");
    const uint64_t start = time_us_64();
    const bool ready = storage_init();
    const bool mounted = ready && storage_mount();
    const sd_card_t *card = ready ? sd_get_by_num(0) : NULL;
    const char *result = mounted ? "OK" : !ready ? "storage driver init error" :
        card == NULL || card->state.card_type == SDCARD_NONE ? "no card" :
        "mount error (filesystem?)";
    int walk_result = 0;
    if (mounted) {
        walk_result = storage_fs_walk(STORAGE_ROOT, scan_visit, NULL);
    }
    const uint32_t elapsed = (uint32_t)((time_us_64() - start) / 1000u);
    char text[160];
    if (mounted) {
        snprintf(text, sizeof(text), "Mount: OK at %lu Hz",
                 (unsigned long)storage_info()->spi_hz);
    } else {
        snprintf(text, sizeof(text), "Mount: %s [%s]", result,
                 storage_fr_str(storage_last_fr()));
    }
    line(18, text);
    printf("SD card: %s\n", text);
    const storage_info_t *info = storage_info();
    snprintf(text, sizeof(text), "%s; %llu MiB", storage_card_type_str(info->card_type),
             (unsigned long long)(info->capacity_bytes / (1024u * 1024u)));
    line(36, text);
    printf("%s\n", text);
    snprintf(text, sizeof(text), "%lu .mp3 files; scan %lu ms; walk %s (%d)",
             (unsigned long)s_mp3_count, (unsigned long)elapsed,
             mounted ? (walk_result == 0 ? "OK" : "ERROR / partial") : "not run",
             walk_result);
    line(54, text);
    printf("%s\n", text);
    for (unsigned i = 0; i < s_song_count; ++i) {
        line(72 + (int)i * 17, s_songs[i]);
        printf("%u. %s\n", i + 1, s_songs[i]);
    }
}

int main(void) {
    stdio_init_all();
    display_config_t display;
    display_default_config(&display);
    const bool display_ok = display_init(&display);
    printf("hw_test: ST7796 480x320 init %s; UART0 115200; d=touch debug\n",
           display_ok ? "OK" : "FAIL");
    for (int y = 0; y < 320; y += 32) {
        gfx_canvas_t c = canvas(0, y, 480, 32);
        gfx_fill(&c, BG);
        flush(&c);
    }
    input_mpr121_config_t touch;
    input_mpr121_default_config(&touch);
    touch.i2c = PIPOD_MPR121_I2C;
    touch.sda_gpio = PIPOD_PIN_MPR121_SDA;
    touch.scl_gpio = PIPOD_PIN_MPR121_SCL;
    touch.irq_gpio = PIPOD_PIN_MPR121_IRQ;
    touch.address = PIPOD_MPR121_ADDRESS;
    touch.poll_interval_us = 5000;
    touch.event_cb = touch_event;
    (void)input_mpr121_init(&touch);
    input_clickwheel_init(&s_wheel, NULL, wheel_ticks, wheel_button, NULL);
    printf("MPR121: %s at 0x%02X, I2C error %d\n",
           input_mpr121_present() ? "found" : "not found/configured",
           input_mpr121_found_address(), input_mpr121_last_i2c_error());
    line(162, "MPR121");
    touch_status();
    for (unsigned i = 0; i < 12; ++i) {
        box(i, false);
    }
    pressed_line(0);
    scan_sd();
    printf("[clocks] spi_get_baudrate: SPI0 display %lu Hz; SPI1 SD %lu Hz "
           "(target 25000000; mount fallback allowed)\n",
           (unsigned long)spi_get_baudrate(spi0),
           (unsigned long)spi_get_baudrate(spi1));

    uint16_t previous = 0;
    int last_error = input_mpr121_last_i2c_error();
    uint64_t last_debug = time_us_64();
    uint64_t last_draw = 0;
    for (;;) {
        const uint64_t poll_start = time_us_64();
        input_mpr121_poll();
        const uint16_t mask = input_mpr121_touched_mask();
        uint16_t delta[12] = {0};
        bool valid = input_mpr121_present();
        for (uint8_t i = 0; valid && i < 12; ++i) {
            uint16_t baseline, filtered;
            valid = input_mpr121_baseline(i, &baseline) &&
                    input_mpr121_filtered_data(i, &filtered);
            if (valid) delta[i] = baseline > filtered ? baseline - filtered : 0;
        }
        input_clickwheel_feed(&s_wheel, delta, valid ? mask : 0,
                              (uint32_t)(time_us_64() / 1000u));
        const uint16_t changed = mask ^ previous;
        for (unsigned i = 0; i < 12; ++i) {
            if ((changed & (1u << i)) != 0) {
                box(i, (mask & (1u << i)) != 0);
            }
        }
        if (changed != 0) {
            pressed_line(mask);
            previous = mask;
        }
        const int error = input_mpr121_last_i2c_error();
        if (error != last_error) {
            touch_status();
            last_error = error;
        }
        const uint64_t now = time_us_64();
        if (now - last_draw >= 50000u) {
            char text[100];
            char angle[16] = "off";
            if (s_wheel.angle_deg10 >= 0)
                snprintf(angle, sizeof(angle), "%ld.%ld", (long)(s_wheel.angle_deg10 / 10),
                         (long)(s_wheel.angle_deg10 % 10));
            snprintf(text, sizeof(text), "angle %s  ticks %ld  %s", angle, (long)s_ticks, s_event);
            gfx_canvas_t c = canvas(0, 302, 480, 18);
            gfx_fill(&c, BG);
            gfx_draw_text_clipped(&c, &font_inter_12, 8, 316, 464, text, FG);
            flush(&c);
            last_draw = now;
        }
        if (getchar_timeout_us(0) == 'd' || (mask != 0 && now - last_debug >= 2000000u)) {
            input_mpr121_debug_print();
            last_debug = now;
        }
        const uint64_t elapsed = time_us_64() - poll_start;
        if (elapsed < 5000u) sleep_us(5000u - elapsed);
    }
}
