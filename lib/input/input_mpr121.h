#ifndef INPUT_MPR121_H
#define INPUT_MPR121_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef INPUT_MPR121_HOST
typedef struct i2c_inst i2c_inst_t;
#else
#include "hardware/i2c.h"
#endif

/* Single instance, call from one core/main loop. Breadboard: I2C1 SDA GP6,
 * SCL GP7, IRQ GP12, ADD=GND (0x5A), 3.3 V. External 10k pull-ups.
 * default_config leaves pins at GPIO_UNUSED: the app MUST supply them. */
#define INPUT_MPR121_GPIO_UNUSED 0xFFu
#define INPUT_MPR121_ELECTRODES 12u
typedef void (*input_mpr121_event_cb_t)(uint8_t electrode, bool pressed,
                                      void *user);
typedef struct {
    i2c_inst_t *i2c;
    uint8_t sda_gpio, scl_gpio, irq_gpio;
    uint32_t i2c_baud;             /* 0 = 400000; also accepts 100000 */
    uint32_t poll_interval_us;     /* 0 = 20000; clickwheel apps use 5000 */
    uint8_t address;               /* 0 = first ACK in 0x5A..0x5D */
    uint8_t touch_threshold;       /* 0 = 12 */
    uint8_t release_threshold;     /* 0 = 6 */
    uint8_t electrode_count;       /* 0 = 12; otherwise 1..12 */
    input_mpr121_event_cb_t event_cb;
    void *event_user;
} input_mpr121_config_t;

typedef struct {
    uint16_t touched, pressed, released;
    bool over_current;
} input_mpr121_edges_t;
/* Pure status decoder: proximity/reserved bits never become touch events.
 * Over-current keeps the old mask; the caller must recover before trusting it. */
input_mpr121_edges_t input_mpr121_decode_status(uint16_t previous,
                                               uint16_t status, uint8_t count);
void input_mpr121_default_config(input_mpr121_config_t *cfg);
bool input_mpr121_init(const input_mpr121_config_t *cfg);
void input_mpr121_poll(void);
uint16_t input_mpr121_touched_mask(void);
/* Return false on invalid electrode/output pointer, absent device or I2C error;
 * output is unchanged on failure. Baseline and filtered use the same units. */
bool input_mpr121_filtered_data(uint8_t electrode, uint16_t *value);
bool input_mpr121_baseline(uint8_t electrode, uint16_t *value);
/* 0 initially/after successful init, otherwise latest negative I2C error.
 * -100 = bad config, -101 = unexpected reset defaults, -102 = short transfer. */
int input_mpr121_last_i2c_error(void);
uint8_t input_mpr121_found_address(void); /* 0 if no address ACKed */
bool input_mpr121_present(void);         /* successful configuration */
void input_mpr121_debug_print(void);

#ifdef INPUT_MPR121_HOST
/* Test transport replaces only the SDK boundary; driver/init/poll are real. */
int input_mpr121_host_read(uint8_t address, uint8_t reg, uint8_t *data, size_t n);
int input_mpr121_host_write(uint8_t address, uint8_t reg, uint8_t value);
uint64_t input_mpr121_host_now_us(void);
bool input_mpr121_host_irq_low(void);
#endif
#endif
