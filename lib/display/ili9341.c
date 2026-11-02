/*
 * ILI9341 240x320 TFT driver (4-wire SPI + DMA) for the Purdue Proton.
 *
 * See ili9341.h for the public API and docs/design.md for the wiring and the
 * RAM/DMA budget. All state is static; nothing is allocated after init.
 *
 * Transaction model:
 *   - CS is driven by SIO and stays low for a whole command+data transaction
 *     (CASET, PASET and RAMWR are sent in one CS-low window).
 *   - Commands/parameters use 8-bit SPI frames, pixels use 16-bit frames.
 *   - The data size, DC and CS only change when the SPI is idle.
 *   - Pixels go out through one DMA channel fed by the SPI TX DREQ; completion
 *     is polled, no interrupt is used.
 *
 * Flash path (ili9341_blit_flash_async):
 *   - The RP2350 XIP stream unit reads flash words into the XIP_AUX FIFO
 *     without touching the CPU or the code cache. One DMA channel moves those
 *     32-bit words into one half of a 2 x 1 KB RAM ring while a second SPI
 *     channel clocks the other half out.
 *   - A 32-bit little-endian flash word holds pixel 0 in bits 15:0 and pixel 1
 *     in bits 31:16; the DMA stores it little-endian, so halfword 0 of RAM is
 *     pixel 0 and halfword 1 is pixel 1. Both go out as 16-bit SPI frames MSB
 *     first, exactly like ili9341_blit_async() sends a uint16_t pixel.
 *   - The SPI channel is only started after the stream channel has finished
 *     filling its half (and the next half is filled while the SPI runs), so
 *     the SPI can never read unfilled data.
 */

#include "ili9341.h"

#include <string.h>

#include "flashstream.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/regs/dreq.h"
#include "hardware/structs/xip_aux.h"
#include "hardware/structs/xip_ctrl.h"
#include "pico/stdlib.h"

/* Panel command set (only the commands this driver needs). */
#define ILI9341_CMD_SLPIN 0x10u
#define ILI9341_CMD_SLPOUT 0x11u
#define ILI9341_CMD_NORON 0x13u
#define ILI9341_CMD_INVOFF 0x20u
#define ILI9341_CMD_INVON 0x21u
#define ILI9341_CMD_DISPOFF 0x28u
#define ILI9341_CMD_DISPON 0x29u
#define ILI9341_CMD_CASET 0x2Au
#define ILI9341_CMD_PASET 0x2Bu
#define ILI9341_CMD_RAMWR 0x2Cu
#define ILI9341_CMD_RAMRD 0x2Eu
#define ILI9341_CMD_MADCTL 0x36u
#define ILI9341_CMD_VSCRDEF 0x33u
#define ILI9341_CMD_VSCRSADD 0x37u
#define ILI9341_CMD_COLMOD 0x3Au
#define ILI9341_CMD_RDDID 0x04u
#define ILI9341_CMD_RDDPM 0x0Au
#define ILI9341_CMD_RDMADCTL 0x0Bu
#define ILI9341_CMD_RDDCOLMOD 0x0Cu

/* MADCTL bits. */
#define ILI9341_MADCTL_MY 0x80u
#define ILI9341_MADCTL_MX 0x40u
#define ILI9341_MADCTL_MV 0x20u
#define ILI9341_MADCTL_BGR 0x08u

/* Values match the four field-proven ILI9341 rotations (MADCTL_MOD
 * transforms), all with the BGR bit set for the Proton's panel. */
#define ILI9341_MADCTL_LANDSCAPE (ILI9341_MADCTL_MV | ILI9341_MADCTL_BGR)
#define ILI9341_MADCTL_LANDSCAPE_FLIPPED \
    (ILI9341_MADCTL_MV | ILI9341_MADCTL_MX | ILI9341_MADCTL_MY | ILI9341_MADCTL_BGR)
#define ILI9341_MADCTL_PORTRAIT (ILI9341_MADCTL_MX | ILI9341_MADCTL_BGR)
#define ILI9341_MADCTL_PORTRAIT_FLIPPED (ILI9341_MADCTL_MY | ILI9341_MADCTL_BGR)

/* Register readbacks (RDDID and the status registers) must be clocked at
 * 6 MHz or less. */
#define ILI9341_READ_HZ 6000000u

/* RP2350 DMA transfer counts are limited to 2^28 - 1. */
#define ILI9341_DMA_MAX_TRANSFER 0x0FFFFFFFu

/* XIP flash window of the 16 MB QSPI chip (addressmap.h XIP_BASE..+16 MB). */
#define ILI9341_XIP_BASE 0x10000000u
#define ILI9341_XIP_END 0x11000000u

/* Flash stream path: two 1 KB ring halves (512 32-bit words = 1024 pixels
 * each) and a 8 KB pixel chunk between audio yield points. */
#define ILI9341_FLASH_HALF_WORDS 256u
#define ILI9341_FLASH_HALF_PIXELS (ILI9341_FLASH_HALF_WORDS * 2u)
#define ILI9341_FLASH_CHUNK_PIXELS 4096u
#define ILI9341_FLASH_YIELD_US 20000u /* bounded wait to re-claim after a yield */

/* Landscape uses the configured panel size; portrait swaps the axes. */
#define ILI9341_LANDSCAPE_WIDTH ((uint16_t)ILI9341_WIDTH)
#define ILI9341_LANDSCAPE_HEIGHT ((uint16_t)ILI9341_HEIGHT)
#define ILI9341_PORTRAIT_WIDTH ((uint16_t)ILI9341_HEIGHT)
#define ILI9341_PORTRAIT_HEIGHT ((uint16_t)ILI9341_WIDTH)

typedef struct {
    ili9341_config_t cfg;
    uint dma_chan;
    uint dma_stream;      /* XIP stream FIFO -> RAM ring (32-bit). */
    uint dma_spi_flash;   /* RAM ring -> SPI DR (16-bit). */
    uint32_t spi_hz;
    uint16_t width;
    uint16_t height;
    uint16_t fill_color; /* DMA source for ili9341_fill_rect, must stay static. */
    ili9341_rotation_t rotation;
    bool dma_claimed;
    bool dma_pending;
    bool flash_claimed;   /* dma_stream + dma_spi_flash are ours. */
    bool initialised;
    bool asleep;
    bool read_dummy_bit; /* 8-bit register reads carry one leading dummy bit. */
    bool selftest_done;  /* ili9341_self_test() has run at least once. */
    bool miso_alive;     /* last self-test saw a driven MISO. */
    uint8_t madctl;      /* MADCTL value last programmed into the panel. */
    uint8_t data_bits;
    uint64_t dma_start_us;
    ili9341_stats_t stats;
} ili9341_state_t;

static ili9341_state_t s;

/* Ping-pong ring for the flash stream path: each half is 1 KB (256 words =
 * 512 pixels), 4-byte aligned for 32-bit DMA. */
static uint32_t s_flash_ring[2][ILI9341_FLASH_HALF_WORDS];

_Static_assert(sizeof(s_flash_ring[0]) == 1024u, "flash ring half must be 1 KB");

/* Pixel readback: the panel answers RAMRD with three bytes per pixel (18-bit
 * colour), so one 320-pixel row needs 960 bytes. Reading in 8-row strips keeps
 * the scratch buffer at 7.7 KB while a whole region still travels in one
 * uninterrupted read per strip. */
#define ILI9341_READ_STRIP_ROWS 8u
#define ILI9341_READ_MAX_PIXELS ((size_t)ILI9341_WIDTH * (size_t)ILI9341_READ_STRIP_ROWS)
static uint8_t s_read_buf[1u + 3u * ILI9341_READ_MAX_PIXELS + 1u];

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[15];
} ili9341_init_cmd_t;

/* Power, VCOM, frame rate, pixel format and gamma setup. */
static const ili9341_init_cmd_t k_init_seq[] = {
    {0xEFu, 3, {0x03u, 0x80u, 0x02u}},                                           /* extended command set */
    {0xCFu, 3, {0x00u, 0xC1u, 0x30u}},                                           /* power control B */
    {0xEDu, 4, {0x64u, 0x03u, 0x12u, 0x81u}},                                    /* power-on sequence */
    {0xE8u, 3, {0x85u, 0x00u, 0x78u}},                                           /* driver timing A */
    {0xCBu, 5, {0x39u, 0x2Cu, 0x00u, 0x34u, 0x02u}},                             /* power control A */
    {0xF7u, 1, {0x20u}},                                                         /* pump ratio */
    {0xEAu, 2, {0x00u, 0x00u}},                                                  /* driver timing B */
    {0xC0u, 1, {0x23u}},                                                         /* power control 1 */
    {0xC1u, 1, {0x10u}},                                                         /* power control 2 */
    {0xC5u, 2, {0x3Eu, 0x28u}},                                                  /* VCOM control 1 */
    {0xC7u, 1, {0x86u}},                                                         /* VCOM control 2 */
    {ILI9341_CMD_MADCTL, 1, {ILI9341_MADCTL_LANDSCAPE}},                         /* landscape, BGR */
    {ILI9341_CMD_COLMOD, 1, {0x55u}},                                            /* 16 bit RGB565 */
    {0xB1u, 2, {0x00u, 0x18u}},                                                  /* frame rate ~79 Hz */
    {0xB6u, 3, {0x08u, 0x82u, 0x27u}},                                           /* display function */
    {0x26u, 1, {0x01u}},                                                         /* gamma curve 1 */
    {0xE0u, 15, {0x0Fu, 0x31u, 0x2Bu, 0x0Cu, 0x0Eu, 0x08u, 0x4Eu, 0xF1u,
                 0x37u, 0x07u, 0x10u, 0x03u, 0x0Eu, 0x09u, 0x00u}},             /* positive gamma */
    {0xE1u, 15, {0x00u, 0x0Eu, 0x14u, 0x03u, 0x11u, 0x07u, 0x31u, 0xC1u,
                 0x48u, 0x08u, 0x0Fu, 0x0Cu, 0x31u, 0x36u, 0x0Fu}},             /* negative gamma */
};

/* ------------------------------------------------------------------------- */
/* Low-level helpers                                                         */
/* ------------------------------------------------------------------------- */

static void hw_wait_spi_idle(void) {
    while (spi_is_busy(s.cfg.spi)) {
        tight_loop_contents();
    }
}

/* The SPI RX FIFO fills up during DMA pixel writes (it overruns after eight
 * frames); drain it and clear the overrun/timeout flags so reads work later. */
static void drain_rx_fifo(void) {
    while (spi_is_readable(s.cfg.spi)) {
        (void)spi_get_hw(s.cfg.spi)->dr;
    }
    spi_get_hw(s.cfg.spi)->icr = SPI_SSPICR_RORIC_BITS | SPI_SSPICR_RTIC_BITS;
}

static void set_data_bits(uint bits) {
    if (s.data_bits == (uint8_t)bits) {
        return;
    }
    hw_wait_spi_idle();
    spi_set_format(s.cfg.spi, bits, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    s.data_bits = (uint8_t)bits;
}

/* Start a command transfer: CS low, DC low, 8 command bits shifted out.
 * Leaves CS low so the caller can append parameter/pixel data. */
static void write_cmd_byte(uint8_t cmd) {
    hw_wait_spi_idle();
    set_data_bits(8);
    gpio_put(s.cfg.pin_cs, 0);
    gpio_put(s.cfg.pin_dc, 0);
    spi_write_blocking(s.cfg.spi, &cmd, 1);
    hw_wait_spi_idle();
    gpio_put(s.cfg.pin_dc, 1);
}

/* Complete command+parameter transaction, then release CS. */
static void panel_cmd(uint8_t cmd, const uint8_t *params, size_t len) {
    ili9341_wait_idle();
    write_cmd_byte(cmd);
    if (params != NULL && len > 0u) {
        spi_write_blocking(s.cfg.spi, params, len);
    }
    hw_wait_spi_idle();
    gpio_put(s.cfg.pin_cs, 1);
}

static void dma_start(const volatile void *src, uint32_t count, bool increment_read) {
    dma_channel_config cfg = dma_channel_get_default_config(s.dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_dreq(&cfg, spi_get_dreq(s.cfg.spi, true));
    channel_config_set_read_increment(&cfg, increment_read);
    channel_config_set_write_increment(&cfg, false);

    s.dma_start_us = time_us_64();
    s.dma_pending = true;
    s.stats.transfers++;
    s.stats.bytes_sent += (uint64_t)count * 2u;

    dma_channel_configure(s.dma_chan, &cfg, &spi_get_hw(s.cfg.spi)->dr, src,
                          dma_encode_transfer_count(count), true);
}

/* Leave the panel in 16-bit pixel-write mode (CS must already be low). */
static void begin_pixel_write(void) {
    hw_wait_spi_idle();
    gpio_put(s.cfg.pin_dc, 1);
    set_data_bits(16);
}

/* ------------------------------------------------------------------------- */
/* XIP stream path (flash -> RAM ring -> SPI)                                */
/* ------------------------------------------------------------------------- */

/* Halt any stream, discard the in-flight read and drain the 2-entry FIFO so a
 * new STREAM_ADDR/STREAM_CTR pair can be programmed safely. */
static void xip_stream_reset(void) {
    xip_ctrl_hw->stream_ctr = 0;
    while ((xip_ctrl_hw->stat & XIP_STAT_FIFO_EMPTY_BITS) == 0u) {
        (void)xip_ctrl_hw->stream_fifo;
    }
}

/* Start streaming \p words 32-bit flash words from \p addr (4-byte aligned). */
static void xip_stream_start(const void *addr, uint32_t words) {
    xip_ctrl_hw->stream_addr = (uint32_t)(uintptr_t)addr;
    xip_ctrl_hw->stream_ctr = words;
}

/* Fill one ring half from flash: program the stream unit and let the stream
 * DMA drain the XIP_AUX FIFO. Blocks until the half is complete. */
static void flash_ring_fill(uint32_t half, const uint16_t *src, uint32_t pixels) {
    const uint32_t words = (pixels + 1u) >> 1u; /* odd pixel counts round up */
    if (words > ILI9341_FLASH_HALF_WORDS) {
        return; /* callers never exceed the half size */
    }

    xip_stream_reset();
    xip_stream_start(src, words);

    dma_channel_config cfg = dma_channel_get_default_config(s.dma_stream);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_dreq(&cfg, DREQ_XIP_STREAM);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    dma_channel_configure(s.dma_stream, &cfg, s_flash_ring[half], &xip_aux_hw->stream, words,
                          true);

    while (dma_channel_is_busy(s.dma_stream)) {
        tight_loop_contents();
    }
}

/* Clock one ring half out through the SPI TX DREQ (16-bit frames). */
static void flash_ring_send(uint32_t half, uint32_t pixels) {
    dma_channel_config cfg = dma_channel_get_default_config(s.dma_spi_flash);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_dreq(&cfg, spi_get_dreq(s.cfg.spi, true));
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    dma_channel_configure(s.dma_spi_flash, &cfg, &spi_get_hw(s.cfg.spi)->dr, s_flash_ring[half],
                          dma_encode_transfer_count(pixels), true);
}

/* Send \p pixels contiguous pixels from flash through the ping-pong ring.
 * Half k is filled, then sent while half k+1 is being filled, so the SPI
 * channel never reads a half that has not been completed. */
static void flash_send_pixels(const uint16_t *src, uint32_t pixels) {
    uint32_t done = 0;
    uint32_t len = (pixels > ILI9341_FLASH_HALF_PIXELS) ? ILI9341_FLASH_HALF_PIXELS : pixels;
    uint32_t half = 0;

    flash_ring_fill(half, src, len);

    for (;;) {
        flash_ring_send(half, len);

        const uint32_t next_done = done + len;
        uint32_t next_len = 0;
        if (next_done < pixels) {
            const uint32_t left = pixels - next_done;
            next_len = (left > ILI9341_FLASH_HALF_PIXELS) ? ILI9341_FLASH_HALF_PIXELS : left;
            flash_ring_fill(half ^ 1u, src + next_done, next_len);
        }

        while (dma_channel_is_busy(s.dma_spi_flash)) {
            tight_loop_contents();
        }

        done = next_done;
        if (done >= pixels) {
            break;
        }
        len = next_len;
        half ^= 1u;
    }
}

/* Pad drive for the display control lines. MISO keeps the default settings
 * because the panel drives it. */
static void apply_pin_drive(bool fast) {
    const enum gpio_slew_rate slew = fast ? GPIO_SLEW_RATE_FAST : GPIO_SLEW_RATE_SLOW;
    const enum gpio_drive_strength drive =
        fast ? GPIO_DRIVE_STRENGTH_12MA : GPIO_DRIVE_STRENGTH_4MA;
    const uint pins[] = {s.cfg.pin_sck, s.cfg.pin_mosi, s.cfg.pin_cs, s.cfg.pin_dc};

    for (size_t i = 0; i < count_of(pins); i++) {
        gpio_set_slew_rate(pins[i], slew);
        gpio_set_drive_strength(pins[i], drive);
    }
}

static void backlight_init(void) {
    gpio_set_function(s.cfg.pin_bl, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(s.cfg.pin_bl);
    pwm_config cfg = pwm_get_default_config();
    /* 150 MHz / (29 * 256) ~= 20.2 kHz: above the audible range, slow enough
     * for the backlight transistor drivers on these modules. */
    pwm_config_set_clkdiv_int(&cfg, 29);
    pwm_config_set_wrap(&cfg, 255);
    pwm_init(slice, &cfg, true);
    pwm_set_gpio_level(s.cfg.pin_bl, 0);
}

static uint8_t high_byte(uint16_t v) {
    return (uint8_t)(v >> 8);
}

static uint8_t low_byte(uint16_t v) {
    return (uint8_t)(v & 0xFFu);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void ili9341_default_config(ili9341_config_t *cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->spi = ILI9341_SPI;
    cfg->pin_miso = ILI9341_PIN_MISO;
    cfg->pin_cs = ILI9341_PIN_CS;
    cfg->pin_sck = ILI9341_PIN_SCK;
    cfg->pin_mosi = ILI9341_PIN_MOSI;
    cfg->pin_dc = ILI9341_PIN_DC;
    cfg->pin_rst = ILI9341_PIN_RST;
    cfg->pin_bl = ILI9341_PIN_BL;
    cfg->spi_hz = ILI9341_SPI_HZ;
    cfg->fast_pins = (ILI9341_FAST_PINS != 0);
}

bool ili9341_init(const ili9341_config_t *cfg) {
    if (cfg == NULL || cfg->spi == NULL) {
        return false;
    }

    if (s.initialised) {
        ili9341_wait_idle();
    }

    s.cfg = *cfg;
    if (s.cfg.spi_hz == 0u) {
        s.cfg.spi_hz = ILI9341_SPI_HZ;
    }
    s.rotation = ILI9341_LANDSCAPE;
    s.width = (uint16_t)ILI9341_LANDSCAPE_WIDTH;
    s.height = (uint16_t)ILI9341_LANDSCAPE_HEIGHT;
    s.data_bits = 0;
    s.dma_pending = false;
    s.asleep = false;
    s.read_dummy_bit = false;
    s.selftest_done = false;
    s.miso_alive = false;
    s.madctl = 0;
    ili9341_reset_stats();

    s.initialised = false;

    if (!s.dma_claimed) {
        s.dma_chan = (uint)dma_claim_unused_channel(true);
        s.dma_claimed = true;
    }

    /* Two more channels for the flash path. If the pool is exhausted the
     * driver still works: ili9341_blit_flash_async() falls back to the plain
     * DMA path. */
    if (!s.flash_claimed) {
        const int stream = dma_claim_unused_channel(false);
        const int spi = dma_claim_unused_channel(false);
        if (stream >= 0 && spi >= 0) {
            s.dma_stream = (uint)stream;
            s.dma_spi_flash = (uint)spi;
            s.flash_claimed = true;
        } else {
            if (stream >= 0) {
                dma_channel_unclaim((uint)stream);
            }
            if (spi >= 0) {
                dma_channel_unclaim((uint)spi);
            }
        }
    }
    flashstream_init();

    /* Control pins first: CS high, DC high, reset released. Latch the output
     * value before enabling the driver so CS cannot glitch low. */
    gpio_init(s.cfg.pin_cs);
    gpio_put(s.cfg.pin_cs, 1);
    gpio_set_dir(s.cfg.pin_cs, GPIO_OUT);

    gpio_init(s.cfg.pin_dc);
    gpio_put(s.cfg.pin_dc, 1);
    gpio_set_dir(s.cfg.pin_dc, GPIO_OUT);

    gpio_init(s.cfg.pin_rst);
    gpio_put(s.cfg.pin_rst, 1);
    gpio_set_dir(s.cfg.pin_rst, GPIO_OUT);

    gpio_set_function(s.cfg.pin_sck, GPIO_FUNC_SPI);
    gpio_set_function(s.cfg.pin_mosi, GPIO_FUNC_SPI);
    gpio_set_function(s.cfg.pin_miso, GPIO_FUNC_SPI);

    /* Configure the pads before the first high-speed clock edge. */
    apply_pin_drive(s.cfg.fast_pins);

    backlight_init();

    /* Hardware reset. */
    gpio_put(s.cfg.pin_rst, 1);
    sleep_ms(5);
    gpio_put(s.cfg.pin_rst, 0);
    sleep_ms(20);
    gpio_put(s.cfg.pin_rst, 1);
    sleep_ms(120);

    s.spi_hz = (uint32_t)spi_init(s.cfg.spi, s.cfg.spi_hz);
    spi_set_format(s.cfg.spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    s.data_bits = 8;
    s.initialised = true;

    for (size_t i = 0; i < count_of(k_init_seq); i++) {
        panel_cmd(k_init_seq[i].cmd, k_init_seq[i].data, k_init_seq[i].len);
    }

    panel_cmd(ILI9341_CMD_SLPOUT, NULL, 0);
    sleep_ms(120);
    panel_cmd(ILI9341_CMD_NORON, NULL, 0);
    panel_cmd(ILI9341_CMD_DISPON, NULL, 0);
    sleep_ms(20);

    ili9341_set_rotation(ILI9341_LANDSCAPE);
    ili9341_set_backlight(100);

    /* RDDID is not a reliable liveness check (many modules ship unprogrammed
     * ID bytes), so probe the status registers instead. A false result can
     * simply mean MISO/SDO is not wired; the panel still works write-only. */
    ili9341_selftest_t st;
    return ili9341_self_test(&st);
}

uint32_t ili9341_set_spi_clock(uint32_t hz) {
    if (!s.initialised || hz == 0u) {
        return s.spi_hz;
    }
    ili9341_wait_idle();
    s.spi_hz = (uint32_t)spi_set_baudrate(s.cfg.spi, hz);
    return s.spi_hz;
}

void ili9341_set_pin_drive(bool fast) {
    if (!s.initialised) {
        return;
    }
    apply_pin_drive(fast);
}

uint32_t ili9341_get_spi_clock(void) {
    return s.spi_hz;
}

void ili9341_set_rotation(ili9341_rotation_t r) {
    if (!s.initialised) {
        return;
    }

    uint8_t madctl;
    uint16_t width;
    uint16_t height;

    switch (r) {
        case ILI9341_LANDSCAPE:
            madctl = ILI9341_MADCTL_LANDSCAPE;
            width = (uint16_t)ILI9341_LANDSCAPE_WIDTH;
            height = (uint16_t)ILI9341_LANDSCAPE_HEIGHT;
            break;
        case ILI9341_LANDSCAPE_FLIPPED:
            madctl = ILI9341_MADCTL_LANDSCAPE_FLIPPED;
            width = (uint16_t)ILI9341_LANDSCAPE_WIDTH;
            height = (uint16_t)ILI9341_LANDSCAPE_HEIGHT;
            break;
        case ILI9341_PORTRAIT:
            madctl = ILI9341_MADCTL_PORTRAIT;
            width = (uint16_t)ILI9341_PORTRAIT_WIDTH;
            height = (uint16_t)ILI9341_PORTRAIT_HEIGHT;
            break;
        case ILI9341_PORTRAIT_FLIPPED:
            madctl = ILI9341_MADCTL_PORTRAIT_FLIPPED;
            width = (uint16_t)ILI9341_PORTRAIT_WIDTH;
            height = (uint16_t)ILI9341_PORTRAIT_HEIGHT;
            break;
        default:
            return;
    }

    panel_cmd(ILI9341_CMD_MADCTL, &madctl, 1);

    s.madctl = madctl;
    s.rotation = r;
    s.width = width;
    s.height = height;
}

uint16_t ili9341_width(void) {
    return s.initialised ? s.width : (uint16_t)ILI9341_LANDSCAPE_WIDTH;
}

uint16_t ili9341_height(void) {
    return s.initialised ? s.height : (uint16_t)ILI9341_LANDSCAPE_HEIGHT;
}

void ili9341_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!s.initialised || w == 0u || h == 0u) {
        return;
    }

    /* Close any pending transfer and release CS before opening a new window. */
    ili9341_wait_idle();

    uint16_t x1 = (uint16_t)(x + w - 1u);
    uint16_t y1 = (uint16_t)(y + h - 1u);
    uint8_t col[4] = {high_byte(x), low_byte(x), high_byte(x1), low_byte(x1)};
    uint8_t row[4] = {high_byte(y), low_byte(y), high_byte(y1), low_byte(y1)};

    write_cmd_byte(ILI9341_CMD_CASET);
    spi_write_blocking(s.cfg.spi, col, sizeof col);

    write_cmd_byte(ILI9341_CMD_PASET);
    spi_write_blocking(s.cfg.spi, row, sizeof row);

    write_cmd_byte(ILI9341_CMD_RAMWR);
}

void ili9341_blit_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px) {
    if (!s.initialised || px == NULL || w == 0u || h == 0u) {
        return;
    }

    uint32_t count = (uint32_t)w * (uint32_t)h;
    if (count > ILI9341_DMA_MAX_TRANSFER) {
        return;
    }

    ili9341_set_window(x, y, w, h);
    begin_pixel_write();
    dma_start(px, count, true);
}

bool ili9341_blit_flash_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              const uint16_t *px) {
    if (!s.initialised || px == NULL || w == 0u || h == 0u) {
        return false;
    }

    const uint32_t count = (uint32_t)w * (uint32_t)h;
    if (count > ILI9341_DMA_MAX_TRANSFER) {
        return false;
    }

    const uintptr_t addr = (uintptr_t)px;
    const bool in_flash = addr >= ILI9341_XIP_BASE && addr < ILI9341_XIP_END;

    /* RAM, or an unaligned flash pointer the stream unit cannot address:
     * plain 16-bit DMA either way (correct, just not cache-friendly). */
    if (!in_flash || (addr & 3u) != 0u || !s.flash_claimed) {
        ili9341_blit_async(x, y, w, h, px);
        return true;
    }

    if (!flashstream_try_claim(FLASHSTREAM_OWNER_DISPLAY)) {
        return false;
    }

    ili9341_set_window(x, y, w, h); /* waits for the previous transfer */
    begin_pixel_write();

    const uint16_t *src = px;
    uint32_t remaining = count;
    while (remaining > 0u) {
        const uint32_t chunk =
            (remaining > ILI9341_FLASH_CHUNK_PIXELS) ? ILI9341_FLASH_CHUNK_PIXELS : remaining;

        flash_send_pixels(src, chunk);
        s.stats.flash_bytes += (uint64_t)chunk * 2u;
        s.stats.flash_blits++;
        s.stats.bytes_sent += (uint64_t)chunk * 2u;
        s.stats.transfers++;

        src += chunk;
        remaining -= chunk;

        /* Audio priority: let a waiting audio user take the unit between
         * chunks, then re-claim. Give up after a bounded wait so the caller
         * can repaint with the strip path instead of blocking the panel. */
        if (remaining > 0u && flashstream_audio_pending()) {
            flashstream_release(FLASHSTREAM_OWNER_DISPLAY);
            const uint32_t t0 = time_us_32();
            while (!flashstream_try_claim(FLASHSTREAM_OWNER_DISPLAY)) {
                if ((uint32_t)(time_us_32() - t0) > ILI9341_FLASH_YIELD_US) {
                    ili9341_wait_idle();
                    return false;
                }
                tight_loop_contents();
            }
        }
    }

    ili9341_wait_idle();
    flashstream_release(FLASHSTREAM_OWNER_DISPLAY);
    return true;
}

void ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {    if (!s.initialised || w == 0u || h == 0u) {
        return;
    }

    uint32_t count = (uint32_t)w * (uint32_t)h;
    if (count > ILI9341_DMA_MAX_TRANSFER) {
        return;
    }

    /* Start the window first: it waits for any previous DMA that may still be
     * reading s.fill_color, so the new colour can be latched safely. */
    ili9341_set_window(x, y, w, h);

    /* The DMA engine keeps reading this value until the transfer completes. */
    s.fill_color = color;

    begin_pixel_write();
    dma_start(&s.fill_color, count, false);
}

void ili9341_wait_idle(void) {
    if (!s.initialised) {
        return;
    }

    if (s.dma_pending) {
        while (dma_channel_is_busy(s.dma_chan)) {
            tight_loop_contents();
        }
        hw_wait_spi_idle();
        drain_rx_fifo();

        uint64_t now = time_us_64();
        s.stats.dma_busy_us += now - s.dma_start_us;
        s.dma_pending = false;
    } else {
        hw_wait_spi_idle();
    }

    gpio_put(s.cfg.pin_cs, 1);
}

bool ili9341_is_busy(void) {
    if (!s.initialised) {
        return false;
    }
    return s.dma_pending || spi_is_busy(s.cfg.spi);
}

/* Command a read and clock \p len bytes back from the panel at <= 6 MHz.
 * write_cmd_byte() drains the RX FIFO while sending the command, so rx[0]
 * here is the first byte the panel drives onto MISO after the command frame. */
static void read_cmd_bytes(uint8_t cmd, uint8_t *rx, size_t len) {
    memset(rx, 0, len);
    if (!s.initialised) {
        return;
    }

    ili9341_wait_idle();

    /* Command writes leave their received (dummy) byte in the RX FIFO; clear
     * everything first so nothing stale leaks into the read below. */
    drain_rx_fifo();

    uint32_t restore_hz = s.spi_hz;
    uint32_t read_hz = restore_hz < ILI9341_READ_HZ ? restore_hz : ILI9341_READ_HZ;
    (void)spi_set_baudrate(s.cfg.spi, read_hz);

    uint8_t tx[4];
    memset(tx, 0xFF, sizeof tx);

    write_cmd_byte(cmd);
    spi_write_read_blocking(s.cfg.spi, tx, rx, len);
    hw_wait_spi_idle();
    gpio_put(s.cfg.pin_cs, 1);
    drain_rx_fifo();

    (void)spi_set_baudrate(s.cfg.spi, restore_hz);
}

/* Read a single-parameter register as a raw 16-bit response. */
static uint16_t read_reg_raw16(uint8_t cmd) {
    uint8_t rx[2];
    read_cmd_bytes(cmd, rx, sizeof rx);
    return ((uint16_t)rx[0] << 8) | (uint16_t)rx[1];
}

/* Start a RAMRD transaction for a window: CS low, CASET, PASET, RAMRD. Unlike
 * ili9341_set_window() this does not issue RAMWR, because the panel would then
 * be in write mode and RAMRD would read from an undefined position. */
static void read_window_begin(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t x1 = (uint16_t)(x + w - 1u);
    uint16_t y1 = (uint16_t)(y + h - 1u);
    uint8_t col[4] = {high_byte(x), low_byte(x), high_byte(x1), low_byte(x1)};
    uint8_t row[4] = {high_byte(y), low_byte(y), high_byte(y1), low_byte(y1)};

    write_cmd_byte(ILI9341_CMD_CASET);
    spi_write_blocking(s.cfg.spi, col, sizeof col);

    write_cmd_byte(ILI9341_CMD_PASET);
    spi_write_blocking(s.cfg.spi, row, sizeof row);

    write_cmd_byte(ILI9341_CMD_RAMRD);
}

/* Extract one data byte from a received stream that may be delayed by the one
 * dummy bit this module inserts (see s.read_dummy_bit). */
static uint8_t pixel_byte(const uint8_t *src, size_t idx) {
    if (!s.read_dummy_bit) {
        return src[idx];
    }
    return (uint8_t)(((uint16_t)src[idx] << 1) | (uint16_t)(src[idx + 1] >> 7));
}

/* Decode a raw 16-bit register read with the detected dummy-bit alignment:
 * no dummy -> rx[0] = D7..D0; one dummy -> rx[0] = [dummy, D7..D1] and
 * rx[1] bit 7 = D0. */
static uint8_t decode_reg8(uint16_t raw) {
    if (s.read_dummy_bit) {
        return (uint8_t)((raw >> 7) & 0xFFu);
    }
    return (uint8_t)(raw >> 8);
}

static bool raw16_alive(uint16_t raw) {
    return raw != 0x0000u && raw != 0xFFFFu;
}

uint32_t ili9341_read_id(void) {
    if (!s.initialised) {
        return 0;
    }

    /* RDDID sends 24 bits MSB first after one dummy bit, so the ID straddles
     * byte boundaries: rx[0] = [dummy, D23..D17], rx[1] = D16..D9,
     * rx[2] = D8..D1, rx[3] = [D0, ...]. */
    uint8_t rx[4];
    read_cmd_bytes(ILI9341_CMD_RDDID, rx, sizeof rx);

    return ((uint32_t)(rx[0] & 0x7Fu) << 17) | ((uint32_t)rx[1] << 9) |
           ((uint32_t)rx[2] << 1) | ((uint32_t)rx[3] >> 7);
}

uint8_t ili9341_read_reg8(uint8_t cmd) {
    if (!s.initialised) {
        return 0;
    }
    return decode_reg8(read_reg_raw16(cmd));
}

bool ili9341_read_pixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *out) {
    /* The scratch buffer holds ILI9341_READ_STRIP_ROWS rows of the panel width;
     * wider windows would overflow it and cannot be valid on this panel. */
    if (!s.initialised || out == NULL || w == 0u || h == 0u || w > (uint16_t)ILI9341_WIDTH) {
        return false;
    }

    /* The dummy-bit alignment is shared with the register reads; probe once if
     * the caller has not done so already. */
    if (!s.selftest_done) {
        ili9341_selftest_t st;
        (void)ili9341_self_test(&st);
    }
    if (!s.miso_alive) {
        return false;
    }

    uint32_t restore_hz = s.spi_hz;
    uint32_t read_hz = restore_hz < ILI9341_READ_HZ ? restore_hz : ILI9341_READ_HZ;

    ili9341_wait_idle();
    drain_rx_fifo();
    (void)spi_set_baudrate(s.cfg.spi, read_hz);

    bool live = false;
    uint16_t row = 0;

    while (row < h) {
        uint16_t rows = (uint16_t)(((uint32_t)h - row) > ILI9341_READ_STRIP_ROWS
                                       ? ILI9341_READ_STRIP_ROWS
                                       : (uint32_t)h - row);
        uint32_t npx = (uint32_t)w * (uint32_t)rows;
        size_t bytes = 1u + (size_t)npx * 3u + (s.read_dummy_bit ? 1u : 0u);

        read_window_begin(x, (uint16_t)(y + row), w, rows);

        memset(s_read_buf, 0, bytes);
        (void)spi_read_blocking(s.cfg.spi, 0xFF, s_read_buf, bytes);
        hw_wait_spi_idle();
        gpio_put(s.cfg.pin_cs, 1);
        drain_rx_fifo();

        /* s_read_buf[0] is the RAMRD dummy byte. */
        const uint8_t *src = &s_read_buf[1];
        for (uint32_t i = 0; i < npx; i++) {
            uint8_t r = pixel_byte(src, (size_t)i * 3u + 0u);
            uint8_t g = pixel_byte(src, (size_t)i * 3u + 1u);
            uint8_t b = pixel_byte(src, (size_t)i * 3u + 2u);
            out[(size_t)row * (size_t)w + (size_t)i] =
                (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) |
                           (uint16_t)(b >> 3));
        }

        if (!live) {
            for (size_t i = 0; i < bytes; i++) {
                if (s_read_buf[i] != 0x00u && s_read_buf[i] != 0xFFu) {
                    live = true;
                    break;
                }
            }
        }

        row = (uint16_t)(row + rows);
    }

    (void)spi_set_baudrate(s.cfg.spi, restore_hz);
    return live;
}

bool ili9341_self_test(ili9341_selftest_t *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!s.initialised) {
        return false;
    }

    out->rddid = ili9341_read_id();

    /* RDMADCTL echoes the byte programmed through MADCTL. Try both possible
     * alignments once to learn whether 8-bit reads carry a leading dummy bit. */
    uint16_t raw_madctl = read_reg_raw16(ILI9341_CMD_RDMADCTL);
    if ((uint8_t)(raw_madctl >> 8) == s.madctl) {
        s.read_dummy_bit = false;
        out->madctl_ok = true;
    } else if ((uint8_t)((raw_madctl >> 7) & 0xFFu) == s.madctl) {
        s.read_dummy_bit = true;
        out->madctl_ok = true;
    }
    out->dummy_bit = s.read_dummy_bit;

    uint16_t raw_pixfmt = read_reg_raw16(ILI9341_CMD_RDDCOLMOD);
    out->pixfmt = decode_reg8(raw_pixfmt);
    /* 0x55 or 0x05: DBI 16 bpp (RGB565). */
    out->pixfmt_ok = (out->pixfmt & 0x0Fu) == 0x05u;

    uint16_t raw_power = read_reg_raw16(ILI9341_CMD_RDDPM);
    out->power_mode = decode_reg8(raw_power);
    /* bit 4 = sleep out, bit 2 = display on. */
    out->display_on = (out->power_mode & 0x10u) != 0u && (out->power_mode & 0x04u) != 0u;

    /* A floating MISO reads back as all 0x00 or all 0xFF. */
    out->miso_alive = (out->rddid != 0x000000u && out->rddid != 0xFFFFFFu) ||
                      raw16_alive(raw_madctl) || raw16_alive(raw_pixfmt) ||
                      raw16_alive(raw_power);

    s.selftest_done = true;
    s.miso_alive = out->miso_alive;

    return out->madctl_ok && out->pixfmt_ok && out->display_on;
}

void ili9341_set_backlight(uint8_t percent) {
    if (!s.initialised) {
        return;
    }
    uint32_t p = percent > 100u ? 100u : percent;
    /* wrap is 255; level 256 means permanently on. */
    uint16_t level = (uint16_t)((p * 256u) / 100u);
    pwm_set_gpio_level(s.cfg.pin_bl, level);
}

void ili9341_sleep(bool sleep) {
    if (!s.initialised) {
        return;
    }

    if (sleep) {
        if (s.asleep) {
            return;
        }
        panel_cmd(ILI9341_CMD_DISPOFF, NULL, 0);
        panel_cmd(ILI9341_CMD_SLPIN, NULL, 0);
        sleep_ms(120);
        s.asleep = true;
    } else {
        if (!s.asleep) {
            return;
        }
        panel_cmd(ILI9341_CMD_SLPOUT, NULL, 0);
        sleep_ms(120);
        panel_cmd(ILI9341_CMD_DISPON, NULL, 0);
        sleep_ms(20);
        s.asleep = false;
    }
}

void ili9341_invert(bool on) {
    if (!s.initialised) {
        return;
    }
    panel_cmd(on ? ILI9341_CMD_INVON : ILI9341_CMD_INVOFF, NULL, 0);
}

void ili9341_set_vertical_scroll(uint16_t top_fixed, uint16_t scroll_area, uint16_t bottom_fixed) {
    if (!s.initialised) {
        return;
    }
    uint8_t data[6] = {
        high_byte(top_fixed), low_byte(top_fixed),
        high_byte(scroll_area), low_byte(scroll_area),
        high_byte(bottom_fixed), low_byte(bottom_fixed),
    };
    panel_cmd(ILI9341_CMD_VSCRDEF, data, sizeof data);
}

void ili9341_scroll_to(uint16_t line) {
    if (!s.initialised) {
        return;
    }
    uint8_t data[2] = {high_byte(line), low_byte(line)};
    panel_cmd(ILI9341_CMD_VSCRSADD, data, sizeof data);
}

void ili9341_get_stats(ili9341_stats_t *out) {
    if (out == NULL) {
        return;
    }
    *out = s.stats;
}

void ili9341_reset_stats(void) {
    memset(&s.stats, 0, sizeof s.stats);
}
