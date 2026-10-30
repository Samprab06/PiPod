# Integration guide: display + UI subsystem

This document is the contract between `lib/ui` / `lib/display` / `lib/gfx`
(this workstream) and the three other workstreams:

| Workstream | Talks to the UI through |
|---|---|
| Audio: decoder + I2S | `ui_poll_command()`, `ui_set_*()` |
| SD / library index | `ui_library_t` callbacks |
| Capacitive touch (MPR121) | `ui_post_*()` via `lib/input/input_wheel.c` |

The authoritative header is **`lib/ui/ui.h`**; everything below is derived from
the code as it exists today. Reference implementation of every integration point:
`apps/pipod/` (fake library `sim_library.c`, fake engine `sim_player.c`,
adapters `lib/input/*`).

---

## 1. Architecture

```
                    core 0 (audio / app)                     core 1 (UI owner)
    +------------------------------------------+   +-----------------------------------------+
    | decoder + I2S (your code)                |   | ui_init() once, then ui_run()/ui_task() |
    |                                          |   |                                         |
    |   ui_poll_command() <----[cmd queue 16]--+---+-- ui_shared_post_cmd()  (UI actions)     |
    |   ui_set_now_playing() --+               |   |                                         |
    |   ui_set_play_state()    |               |   |   ui_library_t::count/get_item/art      |
    |   ui_set_position()      +-->[np state]--+-->|   (called ON the UI core, ~10 rows)     |
    |   ui_set_volume() etc.   |  (critical    |   |                                         |
    |                          |   section,    |   |   gfx_render() -> ili9341_blit_async()  |
    | input adapters           |   seq counter)|   |        |                                |
    |  buttons / UART / MPR121 |               |   |        v                                |
    |   ui_post_input() ------>+--[input queue 32]--+--> drain, derive click/long-press       |
    +------------------------------------------+   +-----------------------------------------+
                                                             |  SPI0 + 1 DMA channel
                                                             v
                                                        ILI9341 320x240
```

Two queues carry everything across the cores:

- **Input queue**, 32 `ui_input_event_t` entries (`lib/ui/ui_shared.c:24`),
  spin-locked (`pico/util/queue.h`). Adapters only ever post
  `UI_INPUT_WHEEL`, `UI_INPUT_BUTTON_DOWN`, `UI_INPUT_BUTTON_UP`.
- **Command queue**, 16 `ui_command_t` entries (`lib/ui/ui_shared.c:25`), plus
  an outer critical section so a full queue can coalesce SEEK / SET_VOLUME
  instead of dropping a user action.

Now-playing state is a single struct behind a critical section with a sequence
counter, so the UI copies one consistent snapshot per frame
(`ui_shared_snapshot()`, `lib/ui/ui_shared.c:185`).

### 1.1 Threading rules

| API | Any core | From IRQ | Notes |
|---|---|---|---|
| `ui_set_now_playing`, `ui_clear_now_playing`, `ui_set_play_state`, `ui_set_position`, `ui_set_volume`, `ui_set_battery`, `ui_set_modes` | yes | yes | Copy under `critical_section`; strings are copied and force-terminated, so the caller may reuse its buffers immediately. `ui_set_now_playing` also resets the position to 0. |
| `ui_post_input`, `ui_post_wheel`, `ui_post_button` | yes | yes | Non-blocking; returns `false` when the 32-entry queue is full (event dropped, counted in `ui_perf_t::dropped_inputs`). `ui_post_input` rejects `type > UI_INPUT_BUTTON_UP`, so adapters cannot inject private navigation events. |
| `ui_poll_command` | yes | yes | Non-blocking; returns `false` when empty. |
| `ui_go_home`, `ui_go_back`, `ui_show_now_playing` | yes | yes | They post a private event into the input queue (types `0x10..0x12`); navigation happens on the next `ui_task()`. Silent no-op if the queue is full. |
| `ui_init`, `ui_task`, `ui_run`, `ui_get_perf` | **no — UI core only** | no | `ui_init`/`ui_task`/`ui_run` drive gfx and the panel with no locking; `ui_get_perf` reads unsynchronised UI-core counters. |
| `ui_library_t` callbacks | UI core only | no | Called from inside `ui_task()`. Must not block. |
| `ili9341_*`, `gfx_*` | UI core only | no | Single-owner driver; the UI core owns the panel after `ui_init()`. |
| `input_buttons_*`, `input_uart_*`, `input_wheel_*` | one core (core 0 in the demo) | no | They only call `ui_post_*()`. |
| `ui_shared_*` helpers | `lib/ui` internal | — | Not part of the public contract. |

**Before `ui_init()` everything is ignored.** `lib/ui/ui_shared.c` guards every
public setter, poster and poll with `shared_ready()`
(`s_shared_ready`, set at the end of `ui_shared_init()`), and each one returns
early (or `false`) while it is clear. Practical consequence: initial state you
publish before the UI core has run `ui_init()` is lost silently. Wait for a
ready flag, exactly like `apps/pipod/main.c:192-202` does.

### 1.2 Who owns what

- Core 0 initialises the panel (`ili9341_default_config()` + `ili9341_init()`)
  **before** launching core 1. After `ui_init()` the UI core is the only caller
  of `ili9341_*` and `gfx_*`.
- The UI core runs `ui_init()` once, then either `ui_run()` (paced loop that
  never returns) or its own `ui_task()` loop.
- No other module may call `ui_task()`, `ui_get_perf()`, or draw into the
  display.

---

## 2. Startup sequence

```c
/* core 0 */
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "ili9341.h"
#include "ui.h"
#include "my_library.h"

static volatile bool s_ui_ready;

static void ui_core_entry(void)
{
    const ui_config_t cfg = {
        .library        = my_library(),   /* required */
        .target_fps     = 30,             /* 0 = 30 */
        .initial_volume = 60,             /* 0..100 */
        .long_press_ms  = 600,            /* 0 = 600 */
    };
    ui_init(&cfg);                        /* draws the first frame itself */
    __atomic_store_n(&s_ui_ready, true, __ATOMIC_RELEASE);
    ui_run();                             /* never returns */
}

int main(void)
{
    stdio_init_all();

    ili9341_config_t dc;
    ili9341_default_config(&dc);
    (void)ili9341_init(&dc);              /* panel up before the UI starts */

    multicore_launch_core1(ui_core_entry);

    /* ui_set_* is a no-op until ui_init() has run: wait for it. */
    while (!__atomic_load_n(&s_ui_ready, __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    ui_set_volume(60);
    ui_set_battery(100, false);
    ui_set_modes(false, false);

    for (;;) {
        audio_service();                  /* decoder + I2S, your code */

        ui_command_t cmd;
        while (ui_poll_command(&cmd)) {
            audio_handle_command(&cmd);
        }

        sleep_ms(2);                      /* poll often; the queue is small */
    }
}
```

If you prefer your own frame loop, call `ui_task()` instead of `ui_run()` and
pace it yourself:

```c
for (;;) {
    ui_task();                            /* returns without sleeping */
    sleep_until(delayed_by_us(start, 1000000u / 30u));
}
```

`ui_init()` already renders one full frame (`lib/ui/ui_core.c:650-652`), so the
panel is valid before the loop starts. `target_fps` is clamped to 1..100.

---

## 3. Audio side: receiving commands

### 3.1 Minimal handler

```c
/* helper used below: clamp an int32 into [lo, hi] */
static int32_t cmd_clamp(int32_t v, int32_t lo, int32_t hi);

static void audio_handle_command(const ui_command_t *c)
{
    switch (c->type) {
    case UI_CMD_PLAY_LIST:                 /* user selected a song */
        audio_start_queue((ui_list_kind_t)c->list, c->parent_id,
                          c->index, c->item_id);
        break;

    case UI_CMD_TOGGLE_PAUSE:
        audio_set_paused(!audio_is_playing());
        break;

    case UI_CMD_NEXT:
        audio_next_track();                /* wrap or stop: your choice */
        break;

    case UI_CMD_PREV:
        /* UI does not decide: restart if > 3 s in, else previous track. */
        if (audio_position_ms() > 3000u) {
            audio_restart_track();
        } else {
            audio_prev_track();
        }
        break;

    case UI_CMD_SEEK:                      /* absolute ms */
        audio_seek_ms(cmd_clamp(c->value, 0, (int32_t)audio_duration_ms()));
        break;

    case UI_CMD_SET_VOLUME:                /* 0..100 */
        audio_set_volume((uint8_t)cmd_clamp(c->value, 0, 100));
        ui_set_volume((uint8_t)cmd_clamp(c->value, 0, 100));  /* echo back */
        break;

    case UI_CMD_SET_SHUFFLE:               /* 0/1 */
        audio_set_shuffle(c->value != 0);
        ui_set_modes(c->value != 0, audio_repeat());
        break;

    case UI_CMD_SET_REPEAT:                /* 0/1 */
        audio_set_repeat(c->value != 0);
        ui_set_modes(audio_shuffle(), c->value != 0);
        break;

    case UI_CMD_SHUFFLE_ALL:               /* "Shuffle Songs" menu item */
        audio_build_shuffled_queue_all_songs();
        break;

    default:
        break;
    }
}
```

Reference: `apps/pipod/sim_player.c:182-258` implements all nine cases.

### 3.2 Every command

| `type` | Fields the UI fills | Expected audio-side behaviour |
|---|---|---|
| `UI_CMD_PLAY_LIST` | `list`, `parent_id`, `index`, `item_id` | `list` is `UI_LIST_SONGS` or `UI_LIST_PLAYLIST_SONGS` only (the UI never sends PLAY_LIST for artists/albums/playlists). Build the play queue from that list scope, start at `item_id` (fall back to `index`), report the track with `ui_set_now_playing()`, then `ui_set_play_state(UI_PLAY_PLAYING)`. `parent_id == UI_ID_ALL` (0xFFFFFFFF) means "all songs". |
| `UI_CMD_TOGGLE_PAUSE` | none (`value` 0) | Toggle play/pause. Report the new state via `ui_set_play_state()`. No-op when nothing is loaded. |
| `UI_CMD_NEXT` | none | Advance one queue position. At the end, wrap, stop, or honour repeat — the UI does not care; it only renders what you report. |
| `UI_CMD_PREV` | none | **UI does not distinguish "restart" from "previous".** Restart the current track when the position is beyond a threshold (the demo uses 3000 ms), otherwise go to the previous queue entry (wrapping). |
| `UI_CMD_SEEK` | `value` = absolute position in ms, ≥ 0 | Clamp to `[0, duration_ms]`, seek, then report with `ui_set_position()`. The UI generates these from scrub mode and from NEXT/PREV long-press. |
| `UI_CMD_SET_VOLUME` | `value` = 0..100 | Clamp, apply, and echo the clamped value through `ui_set_volume()` so the UI and the audio side agree. |
| `UI_CMD_SET_SHUFFLE` | `value` = 0/1 | Apply and echo both mode flags via `ui_set_modes(shuffle, repeat)`. |
| `UI_CMD_SET_REPEAT` | `value` = 0/1 | Same. |
| `UI_CMD_SHUFFLE_ALL` | none | Build a queue of every song in the library in shuffled order and start playing. |

`ui_command_t.index` is a `uint16_t` index in the source list; `item_id` is the
item's library id. `list` is a `ui_list_kind_t`.

### 3.3 Queue and coalescing

- Capacity 16 commands. The UI core drains input, then posts commands; the audio
  core drains with `ui_poll_command()`.
- When the queue is full, the UI drains it, removes the **oldest**
  `UI_CMD_SEEK` or `UI_CMD_SET_VOLUME`, and re-appends everything plus the new
  command. Both are idempotent "absolute value" commands, so dropping the older
  one loses no information. If no coalescable command is present, the **oldest
  command overall** is dropped and the new one is queued
  (`lib/ui/ui_shared.c:121-161`).
- User actions (play/pause, next, play-list) are never coalesced away unless the
  queue is full of them.
- Poll often. A blocking I2S write of several milliseconds is fine; a stall of a
  whole track is not (the queue is small and the UI never retries).

---

## 4. Audio side: updating track information

`ui_track_info_t` (`lib/ui/ui.h:82-91`):

| Field | Type | Meaning |
|---|---|---|
| `track_id` | `uint32_t` | Library id of the track; passed back in `PLAY_LIST.item_id` contexts. |
| `album_id` | `uint32_t` | Album id; the UI calls `ui_library_t::get_album_art(album_id)` for it. |
| `title` | `char[64]` | NUL-terminated. Copied; truncated to 63 bytes. |
| `artist` | `char[48]` | Copied; truncated to 47 bytes. |
| `album` | `char[48]` | Copied; truncated to 47 bytes. |
| `duration_ms` | `uint32_t` | Track length, used for the progress bar and label. |
| `queue_index` | `uint16_t` | **1-based** position in the play queue ("3 of 12"). |
| `queue_length` | `uint16_t` | Queue size. |

Only printable ASCII (0x20..0x7E) is drawn; every other byte becomes `?` at
draw time (`ui_widgets.c:ui_ascii`). Strings are force-terminated by
`ui_shared.c` regardless of what the caller passes.

When to call what:

| Call | When | Rate |
|---|---|---|
| `ui_set_now_playing(&track)` | A new track starts (or the same track restarts with new metadata). Fills the whole struct, resets position to 0 and bumps `track_seq`. | once per track |
| `ui_clear_now_playing()` | Playback stopped and the queue is empty. The screen then shows "Nothing playing". | on stop |
| `ui_set_play_state(state)` | After every successful pause/resume/stop/start. | on change |
| `ui_set_position(ms)` | While playing, and immediately after a seek/pause. | **5–10 Hz is enough** (the demo uses 10 Hz, `sim_player.c:18`); calling faster is harmless but pointless. |
| `ui_set_volume(v)` | After the audio side clamps/applies a volume change (including its own, e.g. an amp-init default). | on change |
| `ui_set_battery(percent, charging)` | Periodically. | 0.2–1 Hz |
| `ui_set_modes(shuffle, repeat)` | After shuffle/repeat actually change, including changes requested through `UI_CMD_SET_*`. | on change |

Typical new-track sequence:

```c
ui_track_info_t t = {0};
t.track_id     = id;
t.album_id     = album;
snprintf(t.title,  sizeof t.title,  "%s", title);
snprintf(t.artist, sizeof t.artist, "%s", artist);
snprintf(t.album,  sizeof t.album,  "%s", album_name);
t.duration_ms  = duration_ms;
t.queue_index  = pos + 1;      /* 1-based */
t.queue_length = queue_len;

ui_set_now_playing(&t);        /* also resets the shown position to 0 */
ui_set_play_state(UI_PLAY_PLAYING);
ui_set_position(0);
```

Redundant scalar setters are cheap: each one compares the value under the
critical section and only bumps the sequence counter when something changed.
(`ui_set_now_playing` and `ui_clear_now_playing` always bump, because they
replace whole-track state.)

---

## 5. Navigating menus

| Function | Effect | Thread |
|---|---|---|
| `ui_go_home()` | Pop the stack to the root "PiPod" menu. | any core / IRQ |
| `ui_go_back()` | Same as a MENU click (pop one level; no-op at the root). | any core / IRQ |
| `ui_show_now_playing()` | Push the Now Playing screen (no-op if it is already on top). | any core / IRQ |

All three are **asynchronous**: they post an event that the next `ui_task()`
consumes. A wheel tick that arrives in the same frame may still apply to the
old screen.

Menu tree (`lib/ui/ui_menu.c:94-140`):

```
PiPod (root)
├── Music
│   ├── Playlists  -> Playlist songs
│   ├── Artists    -> Albums (parent = artist) -> Songs (parent = album)
│   ├── Albums     -> Songs (parent = album)
│   └── Songs      (all songs, parent = UI_ID_ALL)
├── Now Playing            (only while a track is loaded)
├── Shuffle Songs          (posts UI_CMD_SHUFFLE_ALL)
└── Settings
    ├── Backlight          (wheel adjusts 10..100 % in place)
    ├── Shuffle            (posts UI_CMD_SET_SHUFFLE)
    ├── Repeat             (posts UI_CMD_SET_REPEAT)
    ├── Display Clock      (cycles 18.75 / 25 / 37.5 / 75 MHz; 75 MHz experimental)
    ├── Performance overlay(On/Off FPS/CPU readout in the status bar)
    └── About              (static screen)
```

The stack is at most 8 levels (`UI_MAX_DEPTH`); each level keeps its own
selection and scroll offset, so going back restores the previous view.

---

## 6. Receiving user input

### 6.1 Event model

```c
typedef struct {
    uint8_t  type;    /* UI_INPUT_WHEEL, UI_INPUT_BUTTON_DOWN, UI_INPUT_BUTTON_UP */
    uint8_t  button;  /* ui_button_t for button events */
    int16_t  delta;   /* wheel ticks, clockwise positive */
} ui_input_event_t;
```

- **One wheel tick moves the selection by one row.** Positive is clockwise.
  The UI applies acceleration on long lists (≥ 50 rows): ticks less than 20 ms
  apart move 4 rows, less than 40 ms apart move 2 rows
  (`lib/ui/ui_core.c:270-303`).
- **Adapters post down/up edges only.** The UI derives a click (released before
  `long_press_ms`, default 600 ms) or a long press (fires once at
  `long_press_ms` while still held; the later release produces no click).
- Long-press actions: MENU → home; NEXT/PREV → seek +3 s / -3 s immediately,
  then repeat every 200 ms while held. SELECT/PLAY_PAUSE have no long action.
- Wheel and SELECT/MENU clicks are ignored while a screen transition is
  running (220 ms).
- On Now Playing the wheel means volume (±2 per tick, posted as
  `UI_CMD_SET_VOLUME`) or, in scrub mode, ±2 s per tick (±10 s when ticks are
  less than 60 ms apart; `UI_CMD_SEEK` debounced to 10/s). SELECT toggles scrub
  mode; it exits after 2 s idle or on SELECT.
- Queue: 32 events. `ui_post_input()` returns `false` when full; the demo
  surfaces the count in `[perf] ... dropped N`.

### 6.2 Writing a new adapter

1. Own one core (or use an existing poll loop). Debounce and decode your
   hardware there.
2. Translate to abstract events only:
   ```c
   ui_post_wheel(+1);                       /* one detent clockwise */
   ui_post_button(UI_BTN_SELECT, true);     /* press   */
   ui_post_button(UI_BTN_SELECT, false);    /* release */
   ```
3. Never call `ui_shared_*`, never touch gfx, never call `ui_task()`.
4. Poll at 1–5 ms for buttons (the button adapter uses an integrating
   debounce, so jitter in the poll interval is tolerated), or at whatever rate
   your touch controller produces samples.
5. If you post from an IRQ, keep it to a single `ui_post_*()` call.

### 6.3 Existing adapters

**Breadboard buttons** — `lib/input/input_buttons.h`:

```c
input_buttons_init(input_buttons_default_map(),
                   input_buttons_default_map_count(), 5 /* ms */);

for (;;) {
    input_buttons_poll();      /* every 1-5 ms */
    /* ... */
}
```

Default map: GP2 wheel CW, GP3 wheel CCW, GP4 play/pause, GP8 prev, GP9 next,
GP21 select, GP26 menu. Active high, **external pull-downs required**
(RP2350-E9). The two wheel entries are pseudo-buttons: each accepted press
posts one tick, then auto-repeats at 250 ms then every 80 ms.

**UART keys** — `lib/input/input_uart.h`:

```c
stdio_init_all();
input_uart_init();
input_uart_set_perf_hook(my_report);   /* 'P' */
for (;;) { input_uart_poll(); /* ... */ }
```

Keys: `w`/`k`/Up = -1 tick, `s`/`j`/Down = +1, Enter/`e` = SELECT,
`m`/Backspace/lone ESC = MENU, space = PLAY_PAUSE, `n`/Right = NEXT,
`p`/Left = PREV, `h` = home, `P` = perf hook. A key posts down and up
back-to-back, so the UI derives a click from the edges. `input_uart_poll()`
needs stdio initialised.

**Capacitive wheel (MPR121)** — `lib/input/input_wheel.h`. This module is the
glue only; the I2C/MPR121 driver belongs to the touch workstream. Two entry
points:

```c
/* Option A: you already have an angle */
input_wheel_feed_angle(angle_deg10, touching);

/* Option B: hand over raw electrode strengths (2..32 pads) */
input_wheel_feed_pads(pad_strength, n_pads, threshold);
/* or, if you need the angle yourself first: */
int32_t a = input_wheel_angle_from_pads(pad_strength, n_pads);
```

What the touch teammate's poll should look like:

```c
input_wheel_init(INPUT_WHEEL_DEFAULT_TICKS_PER_REV,   /* 24 detents/rev */
                 INPUT_WHEEL_DEFAULT_HYSTERESIS_DEG10);/* 4.0 degrees  */

for (;;) {
    uint16_t pads[8];
    bool touched;

    if (mpr121_read_filtered(pads, &touched)) {   /* your driver */
        if (touched) {
            input_wheel_feed_pads(pads, 8, INPUT_WHEEL_PAD_TOUCH_THRESHOLD);
        } else {
            /* finger off: re-arm the reference without producing ticks */
            input_wheel_feed_angle(0, false);
        }
    }
    sleep_ms(5);                                   /* 200 Hz is plenty */
}
```

Semantics guaranteed by `input_wheel.c`:

- Pad 0 is at 12 o'clock, increasing clockwise; angles are 0..3599 in 0.1°.
- A completed detent emits a tick; sub-detent movement is kept as residue.
- Reversals smaller than `hysteresis_deg10` (default 4.0°) are absorbed.
- A jump larger than 90° between samples is treated as a slip/re-touch: no
  ticks, the reference re-arms at the new angle.
- `touching == false` drops the reference and the residue, so the next touch
  cannot generate a tick from a stale angle.
- `input_wheel_feed_pads()` with no pad above the threshold calls
  `feed_angle(-1, false)` internally.

Tuning: fewer ticks per revolution makes the wheel less sensitive; larger
hysteresis suppresses oscillation under a resting finger.

---

## 7. Library provider (SD / index workstream)

```c
typedef struct {
    uint32_t (*count)(ui_list_kind_t list, uint32_t parent_id, void *ctx);
    bool (*get_item)(ui_list_kind_t list, uint32_t parent_id, uint32_t index,
                     ui_list_item_t *out, void *ctx);
    const gfx_image_t *(*get_album_art)(uint32_t album_id, void *ctx);
    void *ctx;
} ui_library_t;
```

Rules:

1. **Called on the UI core.** Never block on SD I/O there. Build the index on
   your core, publish it, then let the UI read it. There is no locking inside
   `ui_library_t`: publish an immutable snapshot (or swap a pointer atomically)
   and never mutate memory the UI core may be reading.
2. `count()` is called exactly once when a list screen is pushed
   (`ui_menu_screen_init()`); the result is cached in that screen level for its
   lifetime. Return 0 for an invalid scope.
3. `get_item()` is called for **visible rows only**: a window of
   `top-1 .. top+UI_ROWS_VISIBLE` (10 rows) each time the list scrolls to a new
   top row, plus the selected row on activation. A 10-entry cache inside
   `lib/ui` means a row is normally fetched once per view. Return `false` for an
   invalid index; the UI then shows "..." for that row.
4. Fill `out->id`, `out->title` (64 bytes) and optionally `out->subtitle`
   (48 bytes). Strings are copied; the UI force-terminates them.
5. `get_album_art(album_id)` may return `NULL` (placeholder is drawn). The
   returned pointer is cached by the Now Playing screen **until the album id
   changes**, so it must stay valid while that album is displayed. Do not
   return a pointer into a rotating scratch buffer.
6. Image format: RGB565, row-major, native `uint16_t`, ideally **120x120**.
   Other sizes are drawn scaled with nearest-neighbour sampling into the
   120x120 frame (`ui_nowplaying.c`), so 120x120 is both fastest and sharpest.
   Generate covers with `scripts/gen_image.py` (see `scripts/README.md`) or ship
   them `const` in flash.

A complete reference implementation is
[`apps/pipod/sim_library.c`](../apps/pipod/sim_library.c) — 8 artists,
14 albums, 166 songs, 3 playlists, all computed from const tables, no
allocation.

---

## 8. Display driver (`lib/display/ili9341.h`)

The driver is single-owner and unsynchronised. It uses SPI0 with 8-bit frames
for commands/parameters and 16-bit frames for pixels (native RGB565, MSB
first), one dynamically claimed DMA channel, and polls DMA completion (no DMA
IRQ).

| Function | Purpose |
|---|---|
| `ili9341_default_config(&cfg)` | Fill a config with `ili9341_config.h` defaults (SPI0, GP32..38, 37.5 MHz). |
| `ili9341_init(&cfg)` | Reset, full panel init, COLMOD 16-bit, landscape MADCTL, sleep-out, DISPON, backlight 100 %, claim one DMA channel. Returns the self-test verdict; `false` still leaves the panel initialised and usable write-only. |
| `ili9341_set_spi_clock(hz)` / `ili9341_get_spi_clock()` | Runtime clock change (waits for idle first). Returns the **actual** clock the SPI hardware achieved, which may be lower than requested. At clk_peri = 150 MHz the achievable rates are `150 / (even prescale * (1 + SCR))`, i.e. 15 / 18.75 / 25 / 37.5 / 75 MHz near the useful range (75 is experimental). |
| `ili9341_set_rotation(r)` | Landscape / landscape-flipped / portrait / portrait-flipped; updates `width()`/`height()`. |
| `ili9341_width()` / `ili9341_height()` | 320x240 in landscape, 240x320 in portrait. |
| `ili9341_set_window(x,y,w,h)` | CASET/PASET/RAMWR. Leaves CS low and the panel in pixel-write mode; the next blit/fill continues the transaction. |
| `ili9341_blit_async(x,y,w,h,px)` | Waits for the previous transfer, sets the window, starts a 16-bit DMA transfer and returns while it runs. `px` must stay untouched until `ili9341_wait_idle()` or the next blit. |
| `ili9341_fill_rect(x,y,w,h,color)` | Same, with a fixed source (fast solid fill). |
| `ili9341_wait_idle()` | Block until DMA, SPI shifter and CS are done. Call before reusing a blit buffer or changing SPI config. |
| `ili9341_is_busy()` | True while DMA or the SPI shifter is busy. |
| `ili9341_read_id()` | 24-bit RDDID at ≤ 6 MHz, restores the previous clock. `0x009341` = genuine ILI9341; 0/0xFFFFFF = unprogrammed ID or no MISO. |
| `ili9341_read_reg8(cmd)` | 8-bit status register read at ≤ 6 MHz, using the dummy-bit alignment detected by the self-test. |
| `ili9341_self_test(&st)` | Probes RDDID, RDMADCTL, RDDCOLMOD, RDDPM; reports `miso_alive`, `madctl_ok`, `pixfmt_ok`, `display_on`, `dummy_bit`. A `false` result is expected when SDO/MISO is not wired. |
| `ili9341_set_backlight(percent)` | 0..100 % on the PWM backlight (GP38, ~20.2 kHz). |
| `ili9341_sleep(bool)`, `ili9341_invert(bool)` | Panel sleep and display inversion. |
| `ili9341_set_vertical_scroll(top,area,bottom)` / `ili9341_scroll_to(line)` | Hardware vertical scroll (native 320-pixel axis only). |
| `ili9341_get_stats(&st)` / `ili9341_reset_stats()` | `bytes_sent`, `transfers`, `dma_busy_us` since the last reset. Used by the benchmarks. |

---

## 9. Drawing custom screens (`lib/gfx`)

If you only use `lib/ui`, you do not need this section. If you draw your own
screen (an About/debug page, a custom list), use the same model:

- **There is no framebuffer.** A `gfx_canvas_t` is a window onto a small RGB565
  strip; the renderer calls your draw callback once per band and clips to it.
  Your callback must paint the **whole scene** every call and rely on the clip.
- Coordinates are screen coordinates (x right, y down, origin top-left).
- Useful primitives (`gfx.h`): `gfx_fill`, `gfx_fill_rect`,
  `gfx_fill_rect_alpha`, `gfx_fill_round_rect`, `gfx_draw_round_rect`,
  `gfx_fill_gradient_v`, `gfx_hline`, `gfx_vline`, `gfx_fill_circle`,
  `gfx_fill_triangle`, `gfx_draw_image`, `gfx_draw_image_scaled`,
  `gfx_draw_text`, `gfx_draw_text_clipped`, `gfx_text_width`, plus
  `gfx_rect_intersect/union/empty` and `GFX_RGB(r,g,b)`.
- Dirty-rect API (`gfx_render.h`): `gfx_invalidate(rect)`, `gfx_invalidate_all()`,
  `gfx_has_dirty()`, `gfx_render(draw_fn, user)`, and the
  `gfx_render_stats_t` counters.
- Fonts live in `lib/gfx/fonts/fonts.h` (`font_inter_12/14/16sb/20b/28b`).
  Glyphs are 4bpp anti-aliased alpha, ASCII 0x20..0x7E only; other bytes are
  skipped, so sanitise UTF-8 first (`ui_ascii()` does).
- `gfx_render()` leaves the last DMA transfer in flight by design. Call
  `ili9341_wait_idle()` before touching the source buffers or reading stats for
  a frame.

---

## 10. Resource contract

| Resource | What this subsystem takes | Rule for the other workstreams |
|---|---|---|
| GPIO | GP32 (MISO), GP33 (CS), GP34 (SCK), GP35 (MOSI), GP36 (DC), GP37 (RST), GP38 (backlight PWM11 A) — see [wiring.md](wiring.md) | Do not reconfigure GP32..GP38. |
| GPIO | GP2/3/4/8/9/21/26 breadboard buttons (active high, external pull-downs) | GP21/GP26 are board buttons; any new input pin needs an external pull-down (RP2350-E9). |
| UART | GP0/GP1 UART0 to the debug probe (115200 8N1) | Do not take UART0. USB CDC is disabled in both apps. |
| SPI | **SPI0 is exclusive to the display**. ILI9341_SPI = `spi0`, mode 0, MSB first, 8/16-bit frames. | Use SPI1 for the SD card (proposed GP28..GP31). |
| DMA | **Exactly one channel**, claimed with `dma_claim_unused_channel(true)` in `ili9341_init()` (`lib/display/ili9341.c:262`). No DMA IRQ handler; completion is polled. | Claim dynamically as well; never hard-code a channel number. |
| IRQ / alarms | **None.** `lib/ui`, `lib/gfx`, `lib/display` contain no `irq_set_*`, `add_alarm*`, `repeating_timer` or timer IRQ. The only repeating timer in the tree is the demo's 1 ms audio stand-in (`apps/pipod/audio_load_sim.c`), on core 0. | The I2S DMA IRQ stays on core 0; the UI never raises an IRQ on your core. |
| PWM | PWM slice **11 channel A** on GP38, 150 MHz / (29 x 256) ≈ 20.2 kHz, 0..100 %. | Do not use slice 11A. |
| RAM | `s_buf[2][320*16]` = **20,480 B** (gfx strips) + 90 B renderer state = 20,570 B; `lib/ui` static = **2,706 B**; `ili9341` = 96 B; `lib/input` = 430 B. **Total library statics: 23,802 B (~23.2 KB).** The core-1 stack is app-owned (pipod: 2,048 B). | The UI budget is 64 KB; no malloc after init. Keep other core-1 statics small. |
| Flash | UI text (lib/ui + gfx + display + input) ≈ 28.5 KB; fonts: generated set of five **38,343 B (~38 KB)**, linked into pipod **23,420 B (~23 KB)** (four faces; the unused 28 px `font_inter_28b` is dropped by `--gc-sections`); demo art **172,800 B** + 72 B tables. `pipod` image: `text 258,712 B`. | Fonts/art are `const`, so they cost flash, not RAM. |
| Cores | Core 0: audio/decoder/I2S + input adapters (convention). Core 1: display + UI. | Never call UI-core-only APIs from core 0. |

Measured from the current build with:

```sh
tools/arm-gnu/bin/arm-none-eabi-size build/apps/pipod/pipod.elf
tools/arm-gnu/bin/arm-none-eabi-nm -S --size-sort -t d build/apps/pipod/pipod.elf
```

(These are linked-ELF numbers; per-object statics live in `.bss.<symbol>`
sections, so sum every `.bss.*` section if you measure an object file.)

---

## 11. Gotchas

- `ui_set_*` before `ui_init()` is silently ignored. Wait for the UI core.
- `ui_post_*` returns `false` on a full queue; do not spin, but do not assume
  the event arrived. Keep polling at 1–5 ms.
- `ui_go_home/back/show_now_playing` are asynchronous; do not read UI state
  back immediately.
- Do not call `ui_task()` or `ui_get_perf()` from core 0. `ui_get_perf()`
  mutates the UI-core counters; if you need the numbers elsewhere, copy them
  under your own critical section like `apps/pipod/main.c:54-72`.
- Do not draw from an IRQ. The display driver is not re-entrant.
- `ili9341_blit_async()` keeps a pointer to your pixels until the transfer
  completes. Never reuse a blit buffer without `ili9341_wait_idle()`.
- The SPI clock you request is not always the clock you get: see the clock
  table in [performance.md](performance.md). Use `ili9341_get_spi_clock()` for
  anything user-visible.
