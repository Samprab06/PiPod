#include "input_mpr121.h"
#include <stdio.h>
#include <string.h>
#ifndef INPUT_MPR121_HOST
#include "pico/stdlib.h"
#endif

#define I2C_TIMEOUT_US 5000u
#define POLL_INTERVAL_US 20000u
#define REG_ECR 0x5Eu
static input_mpr121_config_t s_cfg;
static uint8_t s_address;
static uint16_t s_touched;
static int s_error;
static bool s_present;
static uint64_t s_last_poll;

input_mpr121_edges_t input_mpr121_decode_status(uint16_t previous,
                                               uint16_t status, uint8_t count) {
    uint16_t mask = count >= 12u ? 0x0FFFu : (uint16_t)((1u << count) - 1u);
    input_mpr121_edges_t e = {0};
    previous &= mask;
    e.over_current = (status & 0x8000u) != 0u;
    e.touched = e.over_current ? previous : status & mask;
    e.pressed = e.touched & (uint16_t)~previous;
    e.released = previous & (uint16_t)~e.touched;
    return e;
}

static uint64_t now_us(void) {
#ifdef INPUT_MPR121_HOST
    return input_mpr121_host_now_us();
#else
    return time_us_64();
#endif
}

static bool transfer_ok(int result, int expected) {
    if (result == expected) return true;
    s_error = result < 0 ? result : -102;
    return false;
}

static bool read_regs(uint8_t reg, uint8_t *data, size_t n) {
#ifdef INPUT_MPR121_HOST
    return transfer_ok(input_mpr121_host_read(s_address, reg, data, n), (int)n);
#else
    if (!transfer_ok(i2c_write_timeout_us(s_cfg.i2c, s_address, &reg, 1,
                                         true, I2C_TIMEOUT_US), 1)) return false;
    return transfer_ok(i2c_read_timeout_us(s_cfg.i2c, s_address, data, n,
                                          false, I2C_TIMEOUT_US), (int)n);
#endif
}

static bool write_reg(uint8_t reg, uint8_t value) {
#ifdef INPUT_MPR121_HOST
    return transfer_ok(input_mpr121_host_write(s_address, reg, value), 2);
#else
    const uint8_t data[] = {reg, value};
    return transfer_ok(i2c_write_timeout_us(s_cfg.i2c, s_address, data, 2,
                                           false, I2C_TIMEOUT_US), 2);
#endif
}

void input_mpr121_default_config(input_mpr121_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
#ifndef INPUT_MPR121_HOST
    cfg->i2c = i2c1;
#endif
    cfg->sda_gpio = cfg->scl_gpio = cfg->irq_gpio = INPUT_MPR121_GPIO_UNUSED;
    cfg->i2c_baud = 400000u;
    cfg->touch_threshold = 12;
    cfg->release_threshold = 6;
    cfg->electrode_count = 12;
}

static bool configure(void) {
    uint8_t reset_cdt;
    s_present = false;
    if (!write_reg(0x80, 0x63)) return false;
#ifndef INPUT_MPR121_HOST
    sleep_ms(1);
#endif
    if (!read_regs(0x5D, &reset_cdt, 1)) return false;
    printf("[mpr121] address=0x%02X reset CDT=0x%02X (expected 0x24): %s\n",
           (unsigned)s_address, (unsigned)reset_cdt,
           reset_cdt == 0x24 ? "OK" : "BAD DEFAULTS");
    if (reset_cdt != 0x24) { s_error = -101; return false; }
    if (!write_reg(REG_ECR, 0)) return false;
    /* AN3944 baseline filters: rising MHD/NHD/NCL/FDL, falling same,
     * touched NHD/NCL/FDL. No MHD register exists for touched state. */
    static const uint8_t filters[] = {
        0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0xFF, 0x02,
        0x00, 0x00, 0x00
    };
    for (uint8_t i = 0; i < sizeof(filters); ++i)
        if (!write_reg((uint8_t)(0x2B + i), filters[i])) return false;
    for (uint8_t i = 0; i < 12u; ++i) {
        if (!write_reg((uint8_t)(0x41 + 2*i), s_cfg.touch_threshold) ||
            !write_reg((uint8_t)(0x42 + 2*i), s_cfg.release_threshold)) return false;
    }
    /* Debounce=0, FFI=6 samples/CDC=16uA; CDT=0.5us, SFI=4/ESI=1ms.
     * Autoconfig: retry=0, BVA=2 (baseline initialise), ARE/ACE=1,
     * FFI=6 (ACCR0=0x0B).
     * 3.3V USL=201, TL=181, LSL=131; ACCR1=0 (no IRQs). */
    static const uint8_t settings[][2] = {
        {0x5B, 0x00}, {0x5C, 0x10}, {0x5D, 0x20},
        {0x7C, 0x00}, {0x7D, 201}, {0x7E, 131}, {0x7F, 181}, {0x7B, 0x0B}
    };
    for (size_t i = 0; i < sizeof(settings)/sizeof(settings[0]); ++i)
        if (!write_reg(settings[i][0], settings[i][1])) return false;
    if (!write_reg(REG_ECR, (uint8_t)(0x80u | s_cfg.electrode_count))) return false;
    s_error = 0;
    s_present = true;
    s_last_poll = now_us();
    return true;
}

bool input_mpr121_init(const input_mpr121_config_t *cfg) {
    /* Copy first so recovery/re-init may safely pass &s_cfg. */
    input_mpr121_config_t next;
    if (cfg) next = *cfg;
    else input_mpr121_default_config(&next);
    s_cfg = next;
    s_present = false;
    s_touched = 0;
    s_address = 0;
    s_error = 0;
    if (!s_cfg.i2c_baud) s_cfg.i2c_baud = 400000;
    if (!s_cfg.touch_threshold) s_cfg.touch_threshold = 12;
    if (!s_cfg.release_threshold) s_cfg.release_threshold = 6;
    if (!s_cfg.electrode_count) s_cfg.electrode_count = 12;
    if (s_cfg.electrode_count > 12 ||
        (s_cfg.i2c_baud != 100000 && s_cfg.i2c_baud != 400000) ||
        (s_cfg.address && (s_cfg.address < 0x5A || s_cfg.address > 0x5D)) ||
        s_cfg.sda_gpio >= 48 || s_cfg.scl_gpio >= 48 || s_cfg.irq_gpio >= 48 ||
        s_cfg.sda_gpio == s_cfg.scl_gpio || s_cfg.sda_gpio == s_cfg.irq_gpio ||
        s_cfg.scl_gpio == s_cfg.irq_gpio) { s_error = -100; return false; }
#ifndef INPUT_MPR121_HOST
    if ((s_cfg.i2c != i2c0 && s_cfg.i2c != i2c1) ||
        (s_cfg.sda_gpio & 3u) != 2u*(s_cfg.i2c == i2c1) ||
        (s_cfg.scl_gpio & 3u) != 2u*(s_cfg.i2c == i2c1) + 1u) {
        s_error = -100; return false;
    }
    i2c_init(s_cfg.i2c, s_cfg.i2c_baud);
    gpio_set_function(s_cfg.sda_gpio, GPIO_FUNC_I2C);
    gpio_set_function(s_cfg.scl_gpio, GPIO_FUNC_I2C);
    gpio_pull_up(s_cfg.sda_gpio);
    gpio_pull_up(s_cfg.scl_gpio);
    gpio_init(s_cfg.irq_gpio);
    gpio_set_dir(s_cfg.irq_gpio, GPIO_IN);
    gpio_pull_up(s_cfg.irq_gpio); /* RP2350-E9: never use pull-down here. */
#endif
    uint8_t status;
    uint8_t first = s_cfg.address ? s_cfg.address : 0x5A;
    uint8_t last = s_cfg.address ? s_cfg.address : 0x5D;
    for (s_address = first; s_address <= last; ++s_address) {
        if (read_regs(0, &status, 1)) {
            printf("[mpr121] ACK at 0x%02X\n", (unsigned)s_address);
            return configure();
        }
    }
    s_address = 0;
    printf("[mpr121] no device ACKed\n");
    return false;
}

static void publish(input_mpr121_edges_t e) {
    s_touched = e.touched;
    if (!s_cfg.event_cb) return;
    for (uint8_t i = 0; i < s_cfg.electrode_count; ++i) {
        uint16_t bit = (uint16_t)(1u << i);
        if ((e.pressed | e.released) & bit)
            s_cfg.event_cb(i, (e.pressed & bit) != 0, s_cfg.event_user);
    }
}

void input_mpr121_poll(void) {
    if (!s_present) return;
    uint64_t now = now_us();
#ifdef INPUT_MPR121_HOST
    bool irq_low = input_mpr121_host_irq_low();
#else
    bool irq_low = !gpio_get(s_cfg.irq_gpio);
#endif
    uint32_t interval = s_cfg.poll_interval_us ? s_cfg.poll_interval_us : POLL_INTERVAL_US;
    if (!irq_low && now - s_last_poll < interval) return;
    s_last_poll = now;
    uint8_t status[2];
    if (!read_regs(0, status, 2)) return;
    input_mpr121_edges_t e = input_mpr121_decode_status(s_touched,
        (uint16_t)(status[0] | ((uint16_t)status[1] << 8)), s_cfg.electrode_count);
    if (e.over_current) {
        printf("[mpr121] over-current: resetting/reconfiguring\n");
        publish(input_mpr121_decode_status(s_touched, 0, s_cfg.electrode_count));
        (void)configure();
        return;
    }
    publish(e);
}

uint16_t input_mpr121_touched_mask(void) { return s_touched; }
int input_mpr121_last_i2c_error(void) { return s_error; }
uint8_t input_mpr121_found_address(void) { return s_address; }
bool input_mpr121_present(void) { return s_present; }

bool input_mpr121_filtered_data(uint8_t electrode, uint16_t *value) {
    uint8_t data[2];
    if (!s_present || !value || electrode >= s_cfg.electrode_count) return false;
    if (!read_regs((uint8_t)(0x04 + 2*electrode), data, 2)) return false;
    *value = (uint16_t)((data[0] | ((uint16_t)data[1] << 8)) & 0x3FFu);
    return true;
}

bool input_mpr121_baseline(uint8_t electrode, uint16_t *value) {
    uint8_t data;
    if (!s_present || !value || electrode >= s_cfg.electrode_count) return false;
    if (!read_regs((uint8_t)(0x1E + electrode), &data, 1)) return false;
    *value = (uint16_t)((uint16_t)data << 2);
    return true;
}

void input_mpr121_debug_print(void) {
    if (!s_present) { printf("[mpr121] not present (error=%d)\n", s_error); return; }
    for (uint8_t i = 0; i < s_cfg.electrode_count; ++i) {
        uint16_t filtered, baseline;
        if (!input_mpr121_filtered_data(i, &filtered) ||
            !input_mpr121_baseline(i, &baseline)) {
            printf("[mpr121] E%u I2C error=%d\n", (unsigned)i, s_error);
            continue;
        }
        printf("[mpr121] E%u filtered=%u baseline=%u delta=%d touched=%u\n",
               (unsigned)i, (unsigned)filtered, (unsigned)baseline,
               (int)baseline - (int)filtered, (unsigned)((s_touched >> i) & 1u));
    }
}
