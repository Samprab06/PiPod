/*
 * ST7796S 320x480 TFT driver (4-wire SPI + DMA) for the Purdue Proton.
 *
 * See st7796.h for the public API. The transaction model, DMA approach and
 * flash stream path are the same as in ili9341.c:
 *   - CS is driven by SIO and stays low for a whole command+data transaction
 *     (CASET, PASET and RAMWR are sent in one CS-low window).
 *   - Commands/parameters use 8-bit SPI frames, pixels use 16-bit frames.
 *   - Pixels go out through one DMA channel fed by the SPI TX DREQ; completion
 *     is polled, no interrupt is used.
 *   - st7796_blit_flash_async() streams straight from XIP flash through the
 *     RP2350 stream unit and a ping-pong RAM ring.
 *
 * The module has no MISO: there is no readback path and no self-test beyond
 * reporting "unsupported". BL is wired to 3.3 V, so the backlight call is a
 * no-op.
 */

#include "st7796.h"

#include <string.h>

#include "flashstream.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/regs/dreq.h"
#include "hardware/structs/xip_aux.h"
#include "hardware/structs/xip_ctrl.h"
#include "pico/stdlib.h"

/* Panel command set (only the commands this driver needs). */
#define ST7796_CMD_SLPOUT 0x11u
#define ST7796_CMD_INVOFF 0x20u
#define ST7796_CMD_INVON 0x21u
#define ST7796_CMD_DISPOFF 0x28u
#define ST7796_CMD_DISPON 0x29u
#define ST7796_CMD_CASET 0x2Au
#define ST7796_CMD_PASET 0x2Bu
#define ST7796_CMD_RAMWR 0x2Cu
#define ST7796_CMD_MADCTL 0x36u
#define ST7796_CMD_COLMOD 0x3Au
#define ST7796_CMD_SLPIN 0x10u

/* MADCTL bits (same layout as the ILI9341 for the bits used here). */
#define ST7796_MADCTL_MY 0x80u
#define ST7796_MADCTL_MX 0x40u
#define ST7796_MADCTL_MV 0x20u
#define ST7796_MADCTL_BGR 0x08u

/* Rotations follow the widely used TFT_eSPI ST7796 rotation table:
 * landscape (480x320) = MV, portrait (320x480) = MX. The BGR bit matches the
 * Proton's panels. The default is landscape with the origin top-left.
 *
 * Orientation is unverified on the XIITIA module: if the image is mirrored or
 * upside down, try ST7796_LANDSCAPE_FLIPPED or swap the MX/MY bits. */
#define ST7796_MADCTL_LANDSCAPE (ST7796_MADCTL_MV | ST7796_MADCTL_BGR)
#define ST7796_MADCTL_LANDSCAPE_FLIPPED \
    (ST7796_MADCTL_MV | ST7796_MADCTL_MX | ST7796_MADCTL_MY | ST7796_MADCTL_BGR)
#define ST7796_MADCTL_PORTRAIT (ST7796_MADCTL_MX | ST7796_MADCTL_BGR)
#define ST7796_MADCTL_PORTRAIT_FLIPPED (ST7796_MADCTL_MY | ST7796_MADCTL_BGR)

/* RP2350 DMA transfer counts are limited to 2^28 - 1. */
#define ST7796_DMA_MAX_TRANSFER 0x0FFFFFFFu

/* XIP flash window of the 16 MB QSPI chip (addressmap.h XIP_BASE..+16 MB). */
#define ST7796_XIP_BASE 0x10000000u
#define ST7796_XIP_END 0x11000000u

/* Flash stream path: two 1 KB ring halves (512 32-bit words = 1024 pixels
 * each) and a 8 KB pixel chunk between audio yield points. */
#define ST7796_FLASH_HALF_WORDS 256u
#define ST7796_FLASH_HALF_PIXELS (ST7796_FLASH_HALF_WORDS * 2u)
#define ST7796_FLASH_CHUNK_PIXELS 4096u
#define ST7796_FLASH_YIELD_US 20000u /* bounded wait to re-claim after a yield */

/* Landscape uses the configured panel size; portrait swaps the axes. */
#define ST7796_LANDSCAPE_WIDTH ((uint16_t)ST7796_WIDTH)
#define ST7796_LANDSCAPE_HEIGHT ((uint16_t)ST7796_HEIGHT)
#define ST7796_PORTRAIT_WIDTH ((uint16_t)ST7796_HEIGHT)
#define ST7796_PORTRAIT_HEIGHT ((uint16_t)ST7796_WIDTH)

typedef struct {
    st7796_config_t cfg;
    uint dma_chan;
    uint dma_stream;      /* XIP stream FIFO -> RAM ring (32-bit). */
    uint dma_spi_flash;   /* RAM ring -> SPI DR (16-bit). */
    uint32_t spi_hz;
    uint16_t width;
    uint16_t height;
    uint16_t fill_color; /* DMA source for st7796_fill_rect, must stay static. */
    st7796_rotation_t rotation;
    bool dma_claimed;
    bool dma_pending;
    bool flash_claimed;   /* dma_stream + dma_spi_flash are ours. */
    bool initialised;
    bool asleep;
    uint8_t madctl;      /* MADCTL value last programmed into the panel. */
    uint8_t data_bits;
    uint64_t dma_start_us;
    st7796_stats_t stats;
} st7796_state_t;

static st7796_state_t s;

/* Ping-pong ring for the flash stream path: each half is 1 KB (256 words =
 * 512 pixels), 4-byte aligned for 32-bit DMA. */
static uint32_t s_flash_ring[2][ST7796_FLASH_HALF_WORDS];

_Static_assert(sizeof(s_flash_ring[0]) == 1024u, "flash ring half must be 1 KB");

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[15];
    uint16_t delay_ms; /* pause after the command, 0 = none */
} st7796_init_cmd_t;

/*
 * Power, VCOM, frame-rate and gamma setup.
 *
 * Source: Bodmer/TFT_eSPI, TFT_Drivers/ST7796_Init.h (FreeBSD licence),
 * https://github.com/Bodmer/TFT_eSPI/blob/master/TFT_Drivers/ST7796_Init.h
 * -- the widely used open-source ST7796S sequence derived from the Sitronix
 * vendor demo. The same power/gamma values appear in Adafruit_ST7796S and in
 * the vendor "ST7796S Demo Code". The vendor command set is unlocked with
 * 0xF0/0xC3 + 0xF0/0x96 and locked again with 0xF0/0x3C + 0xF0/0x69 at the
 * end, exactly as in the source sequence.
 *
 * Differences from the cited sequence: the software reset at its head is
 * replaced by the hardware reset in st7796_init(), sleep-out is sent before
 * this table (also in st7796_init()), DISPON follows the table, and MADCTL is
 * landscape (0x28) instead of the source's portrait value 0x48.
 */
static const st7796_init_cmd_t k_init_seq[] = {
    {0xF0u, 1, {0xC3u}, 0},                     /* vendor command unlock, part I */
    {0xF0u, 1, {0x96u}, 0},                     /* vendor command unlock, part II */
    {ST7796_CMD_MADCTL, 1, {ST7796_MADCTL_LANDSCAPE}, 0}, /* landscape, BGR */
    {ST7796_CMD_COLMOD, 1, {0x55u}, 0},         /* 16 bit RGB565 */
    {0xB4u, 1, {0x01u}, 0},                     /* column inversion, 1-dot */
    {0xB6u, 3, {0x80u, 0x02u, 0x3Bu}, 0},       /* display function control */
    {0xE8u, 8, {0x40u, 0x8Au, 0x00u, 0x00u, 0x29u, 0x19u, 0xA5u, 0x33u}, 0}, /* output timing */
    {0xC1u, 1, {0x06u}, 0},                     /* power control 2 */
    {0xC2u, 1, {0xA7u}, 0},                     /* power control 3 */
    {0xC5u, 1, {0x18u}, 120},                   /* VCOM control */
    {0xE0u, 14, {0xF0u, 0x09u, 0x0Bu, 0x06u, 0x04u, 0x15u, 0x2Fu,
                 0x54u, 0x42u, 0x3Cu, 0x17u, 0x14u, 0x18u, 0x1Bu}, 0}, /* positive gamma */
    {0xE1u, 14, {0xE0u, 0x09u, 0x0Bu, 0x06u, 0x04u, 0x03u, 0x2Bu,
                 0x43u, 0x42u, 0x3Bu, 0x16u, 0x14u, 0x17u, 0x1Bu}, 120}, /* negative gamma */
    {0xF0u, 1, {0x3Cu}, 0},                     /* vendor command lock, part I */
    {0xF0u, 1, {0x69u}, 120},                   /* vendor command lock, part II */
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
 * frames); drain it and clear the overrun/timeout flags. The ST7796 module has
 * no MISO, but SPI0 still receives on GP32 if something drives it. */
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
    st7796_wait_idle();
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
    if (words > ST7796_FLASH_HALF_WORDS) {
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
    uint32_t len = (pixels > ST7796_FLASH_HALF_PIXELS) ? ST7796_FLASH_HALF_PIXELS : pixels;
    uint32_t half = 0;

    flash_ring_fill(half, src, len);

    for (;;) {
        flash_ring_send(half, len);

        const uint32_t next_done = done + len;
        uint32_t next_len = 0;
        if (next_done < pixels) {
            const uint32_t left = pixels - next_done;
            next_len = (left > ST7796_FLASH_HALF_PIXELS) ? ST7796_FLASH_HALF_PIXELS : left;
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

/* Pad drive for the display control lines. There is no MISO on this module. */
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

static uint8_t high_byte(uint16_t v) {
    return (uint8_t)(v >> 8);
}

static uint8_t low_byte(uint16_t v) {
    return (uint8_t)(v & 0xFFu);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void st7796_default_config(st7796_config_t *cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->spi = ST7796_SPI;
    cfg->pin_cs = ST7796_PIN_CS;
    cfg->pin_sck = ST7796_PIN_SCK;
    cfg->pin_mosi = ST7796_PIN_MOSI;
    cfg->pin_dc = ST7796_PIN_DC;
    cfg->pin_rst = ST7796_PIN_RST;
    cfg->spi_hz = ST7796_SPI_HZ;
    cfg->fast_pins = (ST7796_FAST_PINS != 0);
}

bool st7796_init(const st7796_config_t *cfg) {
    if (cfg == NULL || cfg->spi == NULL) {
        return false;
    }

    if (s.initialised) {
        st7796_wait_idle();
    }

    s.cfg = *cfg;
    if (s.cfg.spi_hz == 0u) {
        s.cfg.spi_hz = ST7796_SPI_HZ;
    }
    s.rotation = ST7796_LANDSCAPE;
    s.width = (uint16_t)ST7796_LANDSCAPE_WIDTH;
    s.height = (uint16_t)ST7796_LANDSCAPE_HEIGHT;
    s.data_bits = 0;
    s.dma_pending = false;
    s.asleep = false;
    s.madctl = 0;
    st7796_reset_stats();

    s.initialised = false;

    if (!s.dma_claimed) {
        s.dma_chan = (uint)dma_claim_unused_channel(true);
        s.dma_claimed = true;
    }

    /* Two more channels for the flash path. If the pool is exhausted the
     * driver still works: st7796_blit_flash_async() falls back to the plain
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

    /* Configure the pads before the first high-speed clock edge. */
    apply_pin_drive(s.cfg.fast_pins);

    /* Hardware reset (replaces the software reset at the head of the cited
     * init sequence). */
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

    panel_cmd(ST7796_CMD_SLPOUT, NULL, 0);
    sleep_ms(120);

    for (size_t i = 0; i < count_of(k_init_seq); i++) {
        panel_cmd(k_init_seq[i].cmd, k_init_seq[i].data, k_init_seq[i].len);
        if (k_init_seq[i].delay_ms != 0u) {
            sleep_ms(k_init_seq[i].delay_ms);
        }
    }

    panel_cmd(ST7796_CMD_DISPON, NULL, 0);
    sleep_ms(20);

    st7796_set_rotation(ST7796_LANDSCAPE);

    /* No readback path exists on this module: init reports success once the
     * command sequence has been sent. */
    return true;
}

uint32_t st7796_set_spi_clock(uint32_t hz) {
    if (!s.initialised || hz == 0u) {
        return s.spi_hz;
    }
    st7796_wait_idle();
    s.spi_hz = (uint32_t)spi_set_baudrate(s.cfg.spi, hz);
    return s.spi_hz;
}

void st7796_set_pin_drive(bool fast) {
    if (!s.initialised) {
        return;
    }
    apply_pin_drive(fast);
}

uint32_t st7796_get_spi_clock(void) {
    return s.spi_hz;
}

void st7796_set_rotation(st7796_rotation_t r) {
    if (!s.initialised) {
        return;
    }

    uint8_t madctl;
    uint16_t width;
    uint16_t height;

    switch (r) {
        case ST7796_LANDSCAPE:
            madctl = ST7796_MADCTL_LANDSCAPE;
            width = (uint16_t)ST7796_LANDSCAPE_WIDTH;
            height = (uint16_t)ST7796_LANDSCAPE_HEIGHT;
            break;
        case ST7796_LANDSCAPE_FLIPPED:
            madctl = ST7796_MADCTL_LANDSCAPE_FLIPPED;
            width = (uint16_t)ST7796_LANDSCAPE_WIDTH;
            height = (uint16_t)ST7796_LANDSCAPE_HEIGHT;
            break;
        case ST7796_PORTRAIT:
            madctl = ST7796_MADCTL_PORTRAIT;
            width = (uint16_t)ST7796_PORTRAIT_WIDTH;
            height = (uint16_t)ST7796_PORTRAIT_HEIGHT;
            break;
        case ST7796_PORTRAIT_FLIPPED:
            madctl = ST7796_MADCTL_PORTRAIT_FLIPPED;
            width = (uint16_t)ST7796_PORTRAIT_WIDTH;
            height = (uint16_t)ST7796_PORTRAIT_HEIGHT;
            break;
        default:
            return;
    }

    panel_cmd(ST7796_CMD_MADCTL, &madctl, 1);

    s.madctl = madctl;
    s.rotation = r;
    s.width = width;
    s.height = height;
}

uint16_t st7796_width(void) {
    return s.initialised ? s.width : (uint16_t)ST7796_LANDSCAPE_WIDTH;
}

uint16_t st7796_height(void) {
    return s.initialised ? s.height : (uint16_t)ST7796_LANDSCAPE_HEIGHT;
}

void st7796_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!s.initialised || w == 0u || h == 0u) {
        return;
    }

    /* Close any pending transfer and release CS before opening a new window. */
    st7796_wait_idle();

    uint16_t x1 = (uint16_t)(x + w - 1u);
    uint16_t y1 = (uint16_t)(y + h - 1u);
    uint8_t col[4] = {high_byte(x), low_byte(x), high_byte(x1), low_byte(x1)};
    uint8_t row[4] = {high_byte(y), low_byte(y), high_byte(y1), low_byte(y1)};

    write_cmd_byte(ST7796_CMD_CASET);
    spi_write_blocking(s.cfg.spi, col, sizeof col);

    write_cmd_byte(ST7796_CMD_PASET);
    spi_write_blocking(s.cfg.spi, row, sizeof row);

    write_cmd_byte(ST7796_CMD_RAMWR);
}

void st7796_blit_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px) {
    if (!s.initialised || px == NULL || w == 0u || h == 0u) {
        return;
    }

    uint32_t count = (uint32_t)w * (uint32_t)h;
    if (count > ST7796_DMA_MAX_TRANSFER) {
        return;
    }

    st7796_set_window(x, y, w, h);
    begin_pixel_write();
    dma_start(px, count, true);
}

bool st7796_blit_flash_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const uint16_t *px) {
    if (!s.initialised || px == NULL || w == 0u || h == 0u) {
        return false;
    }

    const uint32_t count = (uint32_t)w * (uint32_t)h;
    if (count > ST7796_DMA_MAX_TRANSFER) {
        return false;
    }

    const uintptr_t addr = (uintptr_t)px;
    const bool in_flash = addr >= ST7796_XIP_BASE && addr < ST7796_XIP_END;

    /* RAM, or an unaligned flash pointer the stream unit cannot address:
     * plain 16-bit DMA either way (correct, just not cache-friendly). */
    if (!in_flash || (addr & 3u) != 0u || !s.flash_claimed) {
        st7796_blit_async(x, y, w, h, px);
        return true;
    }

    if (!flashstream_try_claim(FLASHSTREAM_OWNER_DISPLAY)) {
        return false;
    }

    st7796_set_window(x, y, w, h); /* waits for the previous transfer */
    begin_pixel_write();

    const uint16_t *src = px;
    uint32_t remaining = count;
    while (remaining > 0u) {
        const uint32_t chunk =
            (remaining > ST7796_FLASH_CHUNK_PIXELS) ? ST7796_FLASH_CHUNK_PIXELS : remaining;

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
                if ((uint32_t)(time_us_32() - t0) > ST7796_FLASH_YIELD_US) {
                    st7796_wait_idle();
                    return false;
                }
                tight_loop_contents();
            }
        }
    }

    st7796_wait_idle();
    flashstream_release(FLASHSTREAM_OWNER_DISPLAY);
    return true;
}

void st7796_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (!s.initialised || w == 0u || h == 0u) {
        return;
    }

    uint32_t count = (uint32_t)w * (uint32_t)h;
    if (count > ST7796_DMA_MAX_TRANSFER) {
        return;
    }

    /* Start the window first: it waits for any previous DMA that may still be
     * reading s.fill_color, so the new colour can be latched safely. */
    st7796_set_window(x, y, w, h);

    /* The DMA engine keeps reading this value until the transfer completes. */
    s.fill_color = color;

    begin_pixel_write();
    dma_start(&s.fill_color, count, false);
}

void st7796_wait_idle(void) {
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

bool st7796_is_busy(void) {
    if (!s.initialised) {
        return false;
    }
    return s.dma_pending || spi_is_busy(s.cfg.spi);
}

st7796_selftest_result_t st7796_self_test(void) {
    /* The module exposes no MISO/SDO, so there is nothing to read back. */
    return ST7796_SELFTEST_UNSUPPORTED;
}

void st7796_set_backlight(uint8_t percent) {
    /* BL is tied to 3.3 V on the board; nothing to control. */
    (void)percent;
}

void st7796_sleep(bool sleep) {
    if (!s.initialised) {
        return;
    }

    if (sleep) {
        if (s.asleep) {
            return;
        }
        panel_cmd(ST7796_CMD_DISPOFF, NULL, 0);
        panel_cmd(ST7796_CMD_SLPIN, NULL, 0);
        sleep_ms(120);
        s.asleep = true;
    } else {
        if (!s.asleep) {
            return;
        }
        panel_cmd(ST7796_CMD_SLPOUT, NULL, 0);
        sleep_ms(120);
        panel_cmd(ST7796_CMD_DISPON, NULL, 0);
        sleep_ms(20);
        s.asleep = false;
    }
}

void st7796_invert(bool on) {
    if (!s.initialised) {
        return;
    }
    panel_cmd(on ? ST7796_CMD_INVON : ST7796_CMD_INVOFF, NULL, 0);
}

void st7796_get_stats(st7796_stats_t *out) {
    if (out == NULL) {
        return;
    }
    *out = s.stats;
}

void st7796_reset_stats(void) {
    memset(&s.stats, 0, sizeof s.stats);
}
