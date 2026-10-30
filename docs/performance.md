# Display and UI performance

Status: **no measurements have been taken on hardware yet.** The hardware
result tables (sections 3 and 5.3) are empty placeholders, to be filled on the
first board run. Section 6 is estimated from the renderer's own cost model, not
from the board. Everything outside the results tables is derived from the code
and the SPI arithmetic, not from measurement.

- Benchmarks: `apps/display_test/main.c`
- Live UI counters: `apps/pipod/main.c` (`[perf]` lines every 5 s, or
  immediately on the UART key `P`)
- Clock and DMA behaviour: `lib/display/ili9341.c`
- Renderer: `lib/gfx/gfx_render.c`

---

## 1. Theory

### 1.1 What goes over the wire

- Panel used in landscape, 320x240, RGB565.
- Commands and parameters use 8-bit SPI frames; **pixels use 16-bit frames**
  (native `uint16_t`, MSB first), so one pixel costs exactly 16 SPI clock
  cycles plus no per-byte framing overhead.
- CS is driven by SIO and held low for a whole command+data transaction, so
  there is no CS toggle per byte.
- Full frame: `320 * 240 * 16 = 1,228,800` bits.
- One 320x16 strip (the renderer's band): `320 * 16 * 16 = 81,920` bits.
- One 320x27 menu row: `320 * 27 * 16 = 138,240` bits.

`t = bits / f`, `fps = 1 / t`:

| SPI clock | Full frame | Max full-frame fps | 320x16 strip | 320x27 row | 320x218 content |
|---:|---:|---:|---:|---:|---:|
| 15 MHz | 81.920 ms | 12.21 | 5.461 ms | 9.216 ms | 74.41 ms |
| 18.75 MHz | 65.536 ms | 15.26 | 4.369 ms | 7.373 ms | 59.53 ms |
| 25 MHz | 49.152 ms | 20.35 | 3.277 ms | 5.530 ms | 44.65 ms |
| 37.5 MHz | 32.768 ms | 30.52 | 2.185 ms | 3.686 ms | 29.76 ms |
| 75 MHz (experimental) | 16.384 ms | 61.04 | 1.092 ms | 1.843 ms | 14.88 ms |

Consequence: at the default 37.5 MHz a **full repaint costs 32.77 ms of SPI
time**, i.e. 98 % of the 33.33 ms budget of a 30 FPS frame, before any CPU work.
30 FPS is only reachable because the UI never repaints the whole panel: it
invalidates dirty rectangles and usually touches one or two rows.

### 1.2 Command overhead per band

Every band pays `CASET` (1 command byte + 4 parameter bytes) + `PASET`
(1 + 4) + `RAMWR` (1) = **13 bytes = 104 bits**, independent of the band size:

| Clock | 13-byte band header | 15 bands (full frame) | Overhead share of a full frame |
|---:|---:|---:|---:|
| 15 MHz | 6.93 us | 104.0 us | 0.13 % |
| 18.75 MHz | 5.55 us | 83.2 us | 0.13 % |
| 25 MHz | 4.16 us | 62.4 us | 0.13 % |
| 37.5 MHz | 2.77 us | 41.6 us | 0.13 % |
| 75 MHz | 1.39 us | 20.8 us | 0.13 % |

There is also one DMA start and one `ili9341_wait_idle()` poll per band; those
are CPU costs, not wire costs, and are visible in `[perf] ... dma wait` and
`gfx ... bands`.

### 1.3 What clock you actually get

The PL022 SPI block divides `clk_peri` by an **even prescaler (CPSDVSR, 2..254)
and an integer post-divider (1 + SCR, SCR 0..255)**:

```
f_spi = clk_peri / (CPSDVSR * (1 + SCR))
```

`ili9341_set_spi_clock()` calls `spi_set_baudrate()` and stores the value it
returns, i.e. the **achieved** rate, not the requested one. At
`clk_peri = clk_sys = 150 MHz` (the Proton default; no overclocking in this
project) the achievable set is the `75 / n MHz` harmonic series: 75, 37.5, 25,
18.75, 15, 12.5, 10.71, ... The five steps used by this project are:

| Requested | Actually achieved at 150 MHz | Why |
|---:|---:|---|
| 15 MHz | 15.000 MHz | 150 / (10 * 1) |
| 18.75 MHz | 18.750 MHz | 150 / (8 * 1) |
| 25 MHz | 25.000 MHz | 150 / (2 * 3) |
| 37.5 MHz | 37.500 MHz | 150 / (2 * 2) |
| 75 MHz (experimental) | 75.000 MHz | 150 / (2 * 1) |

These five are the `k_bench_clocks[]` sweep in `apps/display_test/main.c`;
Settings > Display Clock offers the achievable subset **18.75 / 25 / 37.5 /
75 MHz** (`lib/ui/ui_settings.c`). A request outside the
set rounds **down** to the next achievable step: 31.25 MHz becomes 25 MHz,
50 MHz becomes 37.5 MHz, 62.5 MHz becomes 37.5 MHz. Always read the actual
value from `display_test`'s `INFO spi request=... actual=...` line (or
`ili9341_get_spi_clock()`) rather than assuming the request took effect.

Panel rating: the ILI9341 datasheet specifies a **10 MHz write clock**. 37.5 MHz
is a widely used overclock that is commonly reliable; **75 MHz is experimental**
and may show speckles, shifted windows or wrong colours depending on the panel,
wiring and temperature. `display_test` re-draws the colour bars after the 75 MHz
run and asks you to check them for corruption. If 75 MHz corrupts, 37.5 MHz is
the safe rate.

If a clock above 75 MHz is ever needed, it has to come from raising `clk_sys` /
`clk_peri` (unchanged here), not from a larger SPI request.

Warning: the boot path sets `clk_peri = clk_sys` (150 MHz), but calling
`set_sys_clock_khz()`/`set_sys_clock_pll()` moves `clk_peri` to the 48 MHz USB
clock unless `PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1`. At 48 MHz the
highest achievable SPI clock is 24 MHz, so do not change the system clock
without re-measuring the panel.

---

## 2. Why the design reaches 30 FPS

The renderer (`lib/gfx/gfx_render.c`) is framebuffer-less:

1. **Partial updates.** The UI marks only what changed: the status bar title,
   the play glyph, battery, mode icons, the union of the old and new selection
   highlight, the scrolling content area, the progress bar/second label, the
   marquee clip. With nothing dirty, `ui_task()` renders nothing and counts an
   **idle frame**.
2. **Dirty-rect merging.** Up to 8 rectangles are tracked; a new rect is merged
   into an existing one when the union costs at most 1.3x the two separate
   areas. When the list is full, the pair whose union adds the least area is
   merged first.
3. **Strip rendering.** Each dirty rect is split into bands of at most
   `GFX_STRIP_H = 16` rows. A band is rendered into one of two 320x16 RGB565
   buffers (2 x 10 KB) with canvas area and clip set to the band, then handed to
   `ili9341_blit_async()`.
4. **DMA overlap, double buffering.** `ili9341_blit_async()` waits for the
   *previous* transfer, then starts DMA and returns. While band N is on the
   wire, the CPU renders band N+1 into the other buffer. The buffer parity is
   kept across frames, so a buffer is only reused after the transfer that used
   it has completed. Wire time is additive (DMA serialises); CPU render time is
   hidden as long as `render(n) <= dma(n)`.
5. **The last transfer is not awaited.** `gfx_render()` returns with the final
   band still in flight; `ui_task()` calls `ili9341_wait_idle()` once per
   rendered frame (`lib/ui/ui_core.c:692`) before sampling stats, and the next
   blit (or the next frame's first band) completes anything left over.
6. **Frame pacing.** `ui_run()` sleeps to the `target_fps` period. `pipod`
   uses its own loop and re-phases instead of bursting when it falls behind
   (`apps/pipod/main.c:84-102`).

Typical dirty-rect costs at 37.5 MHz (pure wire time):

| What changed | Rows | Pixels | Wire time |
|---|---:|---:|---:|
| Status-bar glyph / battery | 22 | 7,040 | 3.00 ms |
| One menu row | 27 | 8,640 | 3.69 ms |
| Highlight moves one row (union of 2 rows) | 54 | 17,280 | 7.37 ms |
| Album art (120x120) | 120 | 14,400 | 6.14 ms |
| Whole content area (scroll) | 218 | 69,760 | 29.76 ms |
| Whole panel (`gfx_invalidate_all`) | 240 | 76,800 | 32.77 ms |

The scroll case is the only one that approaches the 33.33 ms frame budget, and
at 37.5 MHz it fits (30.5 FPS wire limit) only if the CPU keeps up band by band.

---

## 3. Hardware results — TO BE MEASURED

**Nothing below has been run on a board.** All cells are placeholders (`__`).
Fill them from the serial output described in section 4.

### 3.1 `display_test` per SPI clock

Columns are the five values in `k_bench_clocks[]`
(`apps/display_test/main.c:29-40`): 15, 18.75, 25 and 37.5 MHz plus the
experimental 75 MHz. Record the *actual* value from the
`INFO spi request=... actual=...` line — section 1.3 explains why the achieved
rate is authoritative. After the 75 MHz run the app re-draws the colour bars and
prints a note asking you to check them for corruption.

| Measurement (unit) | Source line | 15 MHz | 18.75 MHz | 25 MHz | 37.5 MHz | 75 MHz (exp.) |
|---|---|---|---:|---:|---:|---:|---:|
| Actual SPI clock (MHz) | `INFO  spi request=... actual=...` | __ | __ | __ | __ | __ |
| Full fill 320x240 (us) | `PASS  full fill 320x240 : ... us` | __ | __ | __ | __ | __ |
| Full fill (fps) | same line, `% fps` | __ | __ | __ | __ | __ |
| Full fill DMA busy (us) | same line, `dma=...` | __ | __ | __ | __ | __ |
| Full fill CPU (us) | same line, `cpu=...` | __ | __ | __ | __ | __ |
| Full blit, 15 strips (us) | `PASS  full blit 15 strips: ... us` | __ | __ | __ | __ | __ |
| Full blit (fps) | same line | __ | __ | __ | __ | __ |
| Full blit DMA busy (us) | same line, `dma=...` | __ | __ | __ | __ | __ |
| Partial blit 100x100 (us) | `PASS  partial blit 100x100: ... us` | __ | __ | __ | __ | __ |
| Checkerboard 80 x 32x32 (us) | `PASS  checkerboard: ... in ... us` | — | — | — | __ | — |
| Bouncing square (fps) | `INFO  anim fps=...` | — | — | — | __ | — |

The checkerboard runs once before the sweep at the default clock; the bouncing
square runs after the sweep, with 37.5 MHz restored.

Fixed-clock checks (run once, before the sweep):

| Check | Source line | Result |
|---|---|---|
| Self-test: MADCTL/COLMOD/display-on | `PASS  self-test: ...` | __ |
| MISO/SDO readback | `PASS/FAIL  MISO/SDO readback: ...` | __ |
| RDDID | `INFO  rddid=0x......` | __ |
| RDDCOLMOD | `PASS  RDDCOLMOD (0x0C): 0x..` | __ |
| Orientation frame | `PASS  frame + corner markers ...` | __ |
| Solid fills / colour bars | `PASS  fill ...` / `PASS  colour bars` | __ |

### 3.2 `pipod` at 30 FPS target

One block of `[perf]` lines (every 5 s, or press `P`) while the UI is animating
(scroll a long list with the wheel, or hold NEXT/PREV to force scrub seeks).
Take the worst of at least 10 consecutive blocks (50 s).

| Metric | Source token | Target | Worst observed |
|---|---|---|---:|
| Frame slots per second | `[perf] ui: X.X fps` | 30.0 ± 0.5 | __ |
| UI core load | `cpu N%` | < 50 % | __ |
| Rendered frames | `frames N` | — | __ |
| Idle frames | `idle N` | > 0 when still | __ |
| CPU time per rendered frame | `frame avg N us` | < 10 ms | __ |
| Worst CPU time per frame | `max N us` | < 16 ms | __ |
| Pixels per rendered frame | `px/frame avg N` | << 76,800 | __ |
| CPU waiting on DMA | `dma wait N us` | small vs the window | __ |
| Dropped input events | `dropped N` | 0 | __ |
| Bands queued | `[perf] gfx: ... bands N` | — | __ |
| Rendered pixels | `gfx ... pixels N` | — | __ |
| Longest `gfx_render()` | `gfx ... max N us` | < 12 ms | __ |
| Bytes over SPI | `[perf] ili9341: N bytes` | — | __ |
| DMA transfers | `ili9341: N xfers` | — | __ |
| DMA busy time | `ili9341: ... dma busy N us` | ≈ wire time | __ |
| Audio callbacks | `[audio] 1ms irq: cb N` | ≈ window_ms (≈ 5000 per 5 s) | __ |
| Audio jitter average | `jitter avg N us` | < 10 us | __ |
| Audio jitter worst | `jitter max N us` | < 100 us | __ |
| Audio callbacks over 100 us | `over100us N` | 0 | __ |
| Missed audio deadlines | `missed N` | 0 | __ |

`ui fps` is `(frames + idle_frames) / elapsed`, so it measures frame *slots*,
not rendered frames; a still screen shows 30 fps with almost all slots idle.

`dma wait` is the sum, over bands, of the wall time from DMA start to
completion. Because the next band is rendered during that window, it includes
the overlap and is therefore an **upper bound** on the time the CPU actually
stalled; a small value relative to the window is the healthy case.

---

## 4. How to run the measurements

### 4.1 Build and flash

```sh
cmake -S . -B build -G Ninja \
  -DPICO_SDK_PATH=D:/362Project/tools/pico-sdk \
  -DPICO_TOOLCHAIN_PATH=D:/362Project/tools/arm-gnu \
  -Dpicotool_DIR=D:/362Project/tools/picotool/picotool \
  -DPICO_PLATFORM=rp2350-arm-s -DPICO_BOARD=proton
cmake --build build --target display_test pipod
```

Flash `build/apps/display_test/display_test.uf2` (BOOTSEL drag-and-drop) or
`tools/picotool/picotool/picotool.exe load -x build/apps/display_test/display_test.elf`.
Both apps route stdio to **UART0 only** (GP0 TX / GP1 RX, 115200 8N1, on-board
debug probe). USB CDC is disabled, so opening the USB serial port shows nothing.

### 4.2 `display_test`

1. Open the probe's serial port at 115200 8N1 and reset the board.
2. Record the self-test block and the orientation/corner result.
3. The app then runs the five SPI clock benchmarks (15, 18.75, 25, 37.5 and
   experimental 75 MHz) and prints, per clock: `INFO spi request=... actual=...`,
   `PASS full fill ...`, `PASS full blit ...`, `PASS partial blit 100x100 ...`.
   The 75 MHz entry is labelled `experimental`; after it the app re-draws the
   colour bars and prints a note asking you to check them for speckles, shifted
   bars or wrong colours.
4. After the sweep it restores `ILI9341_SPI_HZ` (37.5 MHz) and enters the
   bouncing-square animation, printing `INFO anim fps=...` once per second for
   the rest of the run. Let it run for at least 30 s and record the
   steady-state fps.
5. Map each printed value into the table in section 3.1, one column per clock.

The checkerboard line is one aggregate number (80 tiles of 32x32 written as
separate windows/DMA transfers); it measures window-command overhead, not raw
bandwidth.

### 4.3 `pipod`

1. Flash `pipod.uf2` (or load the ELF), same UART.
2. It prints the display init/ID line, the pin banner and the input key list,
   then a `[perf]`/`[player]`/`[audio]` block every 5 s.
3. Keep the UI busy for at least 60 s: hold a wheel pseudo-button (GP2/GP3) or
   send `s`/`w` over UART, and hold NEXT/PREV to exercise scrub seeks.
4. Record the worst of 10+ blocks into section 3.2. `P` prints a block
   immediately without waiting for the 5 s timer.
5. Report with and without the performance overlay (Settings > Performance
   overlay), and with the backlight at 100 % (the default).

Notes:

- Each `[perf]` block covers the window since the previous one; `ui_get_perf(...,
  true)` and the gfx/panel counters are reset after every report.
- The `[audio]` counters are reset **after** the UART write, so the blocking
  115200-baud output is excluded from the next jitter window.

---

## 5. Audio jitter (UI must not disturb the decoder/I2S)

### 5.1 Method

`apps/pipod/audio_load_sim.c` stands in for the audio workstream's I2S
scheduling:

- A **1 ms repeating timer** (`add_repeating_timer_us(1000, ...)`) models the
  I2S DMA half-complete interrupt. Each callback advances an ideal schedule by
  exactly 1000 us and records `jitter = |now - expected|`.
- `AUDIO_SIM_MISS_US = 500`: a callback later than 500 us counts as a **missed
  deadline**.
- `AUDIO_SIM_JITTER_LIMIT_US = 100`: every callback whose jitter exceeds 100 us
  is counted separately and printed as `over100us N`. This is the direct form of
  the pass criterion; `missed 0` alone would allow up to 500 us of jitter.
- A duty-cycled busy loop burns `AUDIO_SIM_DEFAULT_BUSY_PCT` (50 %) of core 0 to
  model decode work; `audio_sim_set_busy_percent(0..95)` changes it at runtime.
- The ISR itself does no printing and no work beyond the bookkeeping.
- Counters are printed by `audio_sim_print_report()` from the core-0 main loop
  only (`[audio] 1ms irq: cb ... jitter avg ... max ... missed ... over100us ...
  busy ...`).

This is a proxy, not the real I2S driver. The real audio ISR must be equally
short (kick the next DMA buffer and return); the measurement then shows
scheduling interference from the UI core, which is what we care about.

### 5.2 Pass criteria

Run at least 5 minutes with the UI continuously animating (menu scroll +
Now Playing marquee + repeated scrub seeks), busy load at 50 % and again at
80 %. The load is compile-time (`AUDIO_SIM_DEFAULT_BUSY_PCT`) or runtime via
`audio_sim_set_busy_percent(0..95)`; the current `pipod` has no UART key for
it, so change the constant or call it from `main.c` for the stress run.

| Criterion | Threshold | Where to read it |
|---|---|---|
| Missed deadlines | **0** | `[audio] ... missed N` |
| Worst jitter | **< 100 us** | `[audio] ... jitter max N us` |
| Callbacks over 100 us | **0** (`over100us 0`) | `[audio] ... over100us N` |
| Mean jitter | < 10 us | `[audio] ... jitter avg N us` |
| UI frame rate while animating | ≥ 29.5 fps | `[perf] ui: X.X fps` |
| Dropped input events | 0 | `[perf] ui: ... dropped N` |

The pass criterion while the UI animates is **`jitter max` below 100 us and
`over100us 0`**. `over100us` counts every callback whose lateness exceeds
`AUDIO_SIM_JITTER_LIMIT_US` (100 us) in the report window, so it is stricter
than `missed`, which only fires above 500 us.

### 5.3 Results — TO BE MEASURED

| Condition | Jitter avg (us) | Jitter max (us) | Over 100 us | Missed | UI fps | Notes |
|---|---:|---:|---:|---:|---:|---|
| 50 % busy, UI idle | __ | __ | __ | __ | __ | |
| 50 % busy, UI animating | __ | __ | __ | __ | __ | |
| 80 % busy, UI idle | __ | __ | __ | __ | __ | |
| 80 % busy, UI animating | __ | __ | __ | __ | __ | |
| 5 min soak, 50 % busy + animating | __ | __ | __ | __ | __ | |

---

## 6. Estimated per-frame pixel cost per interaction

Ten interaction traces through the real `lib/ui` + `lib/gfx` code at a 33 ms
frame period (30 FPS), recording the pixels, dirty rects and strip bands each
frame would touch.

**These are estimates from the renderer's cost model, not hardware
measurements.** `est SPI ms`
is pure wire time at 37.5 MHz (`pixels * 16 / 37.5e6`); it excludes the
13-byte-per-band window header (~0.13 %), DMA setup/polling, CPU render time
and any clock the panel actually achieves. Compare with sections 3/5.3 once the
board is measured.

Averages are over **drawn** frames only (idle frames render nothing and are
counted in the `frames` column as `drawn/total`); max is the worst single
frame. `Est FPS cap` is `min(30, 1000 / est_ms)` for the worst frame.

| Scenario step | Frames (drawn/total) | Pixels/frame avg | Pixels/frame max | Rects/frame avg | Rects/frame max | Bands/frame avg | Bands/frame max | Est SPI ms (avg) | Est SPI ms (max) | Worst frame budget | Est FPS cap |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `01_main_menu` (initial full repaint) | 1/6 | 75,840 | 75,840 | 1.00 | 1 | 15.00 | 15 | 32.358 | 32.358 | 97.1 % | 30.0 |
| `02_music_menu` (push slide + settle) | 7/13 | 71,589 | 76,160 | 1.00 | 1 | 14.29 | 15 | 30.544 | 32.495 | 97.5 % | 30.0 |
| `03_songs_list` (scroll ~20 rows) | 26/46 | 52,098 | 76,160 | 1.00 | 1 | 10.54 | 15 | 22.229 | 32.495 | 97.5 % | 30.0 |
| `04_transition` (mid push slide) | 10/14 | 72,320 | 76,160 | 1.00 | 1 | 14.40 | 15 | 30.857 | 32.495 | 97.5 % | 30.0 |
| `05_now_playing` (marquee, seek, entry) | 65/114 | 23,675 | 76,160 | 1.00 | 1 | 5.43 | 15 | 10.101 | 32.495 | 97.5 % | 30.0 |
| `06_volume` (overlay + fade) | 61/62 | 6,056 | 17,568 | 1.10 | 2 | 2.30 | 5 | 2.584 | 7.496 | 22.5 % | 30.0 |
| `07_scrub` (scrub mode) | 8/8 | 9,588 | 17,568 | 1.38 | 2 | 3.12 | 5 | 4.091 | 7.496 | 22.5 % | 30.0 |
| `08_settings` (push slide + highlight) | 19/31 | 55,933 | 76,160 | 1.00 | 1 | 11.21 | 15 | 23.865 | 32.495 | 97.5 % | 30.0 |
| `09_about` (push slide) | 7/12 | 71,589 | 76,160 | 1.00 | 1 | 14.29 | 15 | 30.544 | 32.495 | 97.5 % | 30.0 |
| `10_albums` (menu + list scrolling) | 31/52 | 52,676 | 76,160 | 1.00 | 1 | 10.61 | 15 | 22.475 | 32.495 | 97.5 % | 30.0 |
| **All steps** | **235/358** | **33,347** | **76,160** | — | **2** | — | **15** | **14.228** | **32.495** | **97.5 %** | **30.0** |

Interpretation:

- **Screen slides are SPI-bound.** Every push/pop animates both screens for
  220 ms with the whole visible area invalidated, so those frames push up to
  76,160 px in 15 bands = **32.5 ms of SPI**, 97.5 % of the 33.33 ms frame
  budget. That is why eight of the ten steps show a ~32.5 ms worst frame:
  each starts with a transition or full repaint (`04_transition` is the slide
  itself); only `06_volume` and `07_scrub`, which stay on Now Playing, avoid
  it. At 37.5 MHz the 30 FPS target still holds (wire-only limit 30.8 FPS), but
  the margin is ~0.8 ms; at 25 MHz the same frame needs 48.7 ms (20.5 FPS cap)
  and at 18.75 MHz 65.0 ms (15.4 FPS).
- **List scrolling is the second SPI-bound case.** Scrolling invalidates the
  content area; the content alone costs up to 69,760 px / 29.76 ms per moving
  frame, and a scroll frame that also touches the status bar reaches 76,160 px.
  The `03`/`10` averages (52k px) are pulled down by settle frames that touch
  one or two rows (~8.9k px / 3.8 ms).
- **Marquee and progress are cheap.** A Now Playing "steady" frame touches the
  title clip (4,800 px / 2.05 ms) or the progress bar (~17.6k px / 7.5 ms), so
  `05_now_playing`'s typical drawn frame is 23,675 px (10.1 ms) — CPU-bound,
  not SPI-bound; its 32.5 ms max comes from entering the screen.
- **Volume overlay / scrub are the cheapest interactions.** Two rects
  (progress bar + time labels), at most 17,568 px = 7.5 ms (22.5 % of budget);
  1.1–1.4 rects per drawn frame shows dirty-rect merging working as intended.
- **Dirty rects collapse to 1 per frame** almost everywhere. The renderer's
  1.3x merge rule unions neighbouring invalidations, so the band count (15 =
  full screen, 5 = progress area) is the real cost driver, not the rect count.
- Idle frames render nothing (`0 px`), which is what keeps the average below
  the worst case and why `ui fps` (frame slots) stays at 30 while the wire is
  idle.

---

## 7. Memory footprint (measured from the current build)

Command:

```sh
tools/arm-gnu/bin/arm-none-eabi-size build/apps/pipod/pipod.elf \
                                    build/apps/display_test/display_test.elf
tools/arm-gnu/bin/arm-none-eabi-nm -S --size-sort -t d build/apps/pipod/pipod.elf
```

### 7.1 Images

| Image | text (flash) | data | bss (RAM) | Notes |
|---|---:|---:|---:|---|
| `pipod.elf` | 258,712 B | 0 B | 28,020 B | Full UI + virtualised library + fake audio + 6 album covers + audio-load sim |
| `display_test.elf` | 28,092 B | 0 B | 11,132 B | Driver test + benchmarks, one 10 KB strip |

### 7.2 Biggest RAM symbols (`pipod.elf`)

| Symbol | Size | What |
|---|---:|---|
| `s_buf` | 20,480 B | Two 320x16 RGB565 strip buffers (`gfx_render.c`) |
| `core1_stack` | 2,048 B | Core-1 stack from `multicore_launch_core1` |
| `s_cache` | 1,280 B | 10-entry list-row cache (`lib/ui/ui_menu.c`) |
| `s_queue` | 768 B | Demo fake-player queue (192 ids) |
| `s_buttons` | 384 B | Button adapter state (16 slots) |
| `default_alarm_pool_entries` | 384 B | Pico SDK alarm pool (unused by `lib/ui`) |
| `ram_vector_table` | 272 B | SDK vector table |
| `s_np_state`, `s_np`, `s_prev` | 200 B each | Cross-core state + UI snapshots |
| `s_stack` | 256 B | UI navigation stack (8 levels) |

`display_test.elf` is dominated by `g_strip` (10,240 B) plus the SDK runtime
(~892 B).

### 7.3 Library statics (linked into `pipod.elf`)

| Module | .bss | Main consumers |
|---|---:|---|
| `lib/gfx` | 20,570 B | 20,480 B strip buffers + 90 B renderer/dirty state |
| `lib/ui` | 2,706 B | `ui_core` 879, `ui_menu` 1,281 (row cache), `ui_shared` 257, `ui_settings` 197, `ui_nowplaying` 92 |
| `lib/display` | 96 B | driver state + stats |
| `lib/input` | 430 B | buttons 401, UART 13, wheel 16 |
| **Total** | **23,802 B (~23.2 KB)** | well inside the 64 KB UI budget |

### 7.4 Flash consumers

| Item | Size | Notes |
|---|---:|---|
| Inter fonts, generated set (5 faces) | 38,343 B (~38 KB) | `scripts/README.md` (bitmaps 32,563 + glyph tables 5,700 + structs) |
| Inter fonts, linked into `pipod` | 23,420 B (~23 KB) | 12/14/16sb/20b only; the 28 px face (`font_inter_28b`) is unused and dropped by `--gc-sections` |
| Demo album art | 172,800 B | 6 x 120x120 RGB565 |
| Demo art structs + pointer table | 72 B | 6 x 8 B + 24 B |
| `pipod` text total | 258,712 B | includes SDK, newlib, printf |

### 7.5 Note on per-object numbers

The build places statics in `.bss.<symbol>` sections. Per-object figures above
were obtained by summing every `.bss.*` section with `arm-none-eabi-objdump -h`;
`arm-none-eabi-nm -S -t d` prints sizes in decimal, which is easy to misread.
The image totals in 7.1 come from the linked ELF and are the authoritative
numbers.
