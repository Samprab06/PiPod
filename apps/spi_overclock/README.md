# spi_overclock: ILI9341 SPI margin sweep

Firmware for the Purdue Proton (RP2350B) that pushes the ILI9341 4-wire SPI bus
past the rated 75 MHz and reports what the panel and wiring actually tolerate.

`clk_sys` stays at **150 MHz**. The PL022 SPI master can only divide
`clk_peri` down (SPI = clk_peri / (CPSDVSR * (1 + SCR)), CPSDVSR >= 2), so the
maximum standard SPI clock is `clk_peri / 2` = 75 MHz. To go higher this app
overclocks `clk_peri` only:

1. `PLL_USB` is reprogrammed to (about) `2 * target SPI` with `pll_init()`.
2. `clk_peri` is switched from `clk_sys` to `clk_usb` (PLL_USB) with divider 1:
   `clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, pll_hz, pll_hz)`.
3. UART0 runs from `clk_peri`, so its baud rate is reprogrammed after every
   clock change (output is flushed first).
4. The SPI baud is re-applied as `clk_peri / 2` and checked against the
   `CPSDVSR`/`SCR` registers.

The 37.5 MHz "safe" step restores `clk_peri` to `clk_sys` (150 MHz) and
re-runs `ili9341_init()` when entered with `s`, which fully re-syncs a panel
that went deaf at a higher clock.

## Caveat: this is out of spec

- Above **150 MHz clk_peri / 75 MHz SPI** the design is outside the RP2350
  datasheet and the ILI9341 datasheet. Results are module- and
  wiring-dependent; a pass here does not make it a production setting.
- **USB is unavailable** in this method: PLL_USB no longer produces 48 MHz.
  The app uses UART0 (GP0/GP1, 115200 8N1) only and builds with USB CDC off.
- **The ADC is unavailable** too: its default clock is derived from PLL_USB.
- The UART is on `clk_peri`. If a clock change corrupts serial output the app
  is not recoverable over UART; use the reset button.
- For the product the cleaner alternative is raising `clk_sys` (which also
  raises `clk_peri` and every other synchronous clock) instead of isolating
  `clk_peri`; it costs more power and affects timers/flash timing.

## What it displays

Every step draws the same deterministic 320x240 pattern, rendered through two
320x16 strip buffers with `ili9341_blit_async()` (no framebuffer):

- banner: step label in Inter 28 bold, `step N/9`, live fps in the animation
- 1 px black/white checkerboard (192x32)
- 1 px vertical stripes and 1 px horizontal stripes
- 8 colour bars (white, yellow, cyan, green, magenta, red, blue, black)
- smooth R/G/B/grey horizontal gradients
- a small Inter 12 text sample line
- a 1 px white border around the screen

The pattern is a pure function of `(x, y, phase)`, so the same scene code
regenerates the expected pixels for the readback comparison. The animation
alternates the normal pattern with its inverse so every pattern pixel changes.

## Readback check (MISO required)

When the self-test finds a driven MISO/SDO pin, each step reads back:

- the 1 px checkerboard, 64x16 at (4, 48)
- a gradient row block, 320x8 at (0, 168)
- the colour bars, 320x8 at (0, 140)
- four fixed pseudo-random 16x16 blocks inside the pattern area

The reads use RAMRD (0x2E) at 6 MHz or less and are compared with the
regenerated scene. A written RGB565 pixel is expanded to 18-bit in panel RAM,
so a correct write must read back as the same RGB565 value exactly.

If the baseline 37.5 MHz step reports mismatches, the readback path itself is
unreliable on that module (dummy byte/dummy bit alignment or a floating SDO);
the app says so and ignores readback results for the rest of the run. The
visual test is still valid.

## Measurements per step

- 5 s full-frame animation, redrawn as fast as the link allows; rolling and
  final full-frame fps are printed and shown in the banner.
- 1 s partial-update measurement of a 320x120 region (8 bands of 15 rows).
- Static pattern held for visual inspection.

## Keys and buttons

| Input | Action |
|---|---|
| `n` / GP21 (Select) | next step (at the last step: print the summary) |
| `p` / GP26 (Menu) | previous step |
| `r` | redo the current step |
| `s` | back to the safe 37.5 MHz step and re-run `ili9341_init()` |
| `a` | toggle auto mode (advance every 8 s) |
| `t` | print the summary table |

GP21/GP26 use the on-board 10k pull-downs; internal pulls are disabled and
presses are debounced for 20 ms.

## What to look for / report back

At each step inspect the panel for:

- sparkles or noise in the 1 px checkerboard (first sign of a marginal link)
- shifted rows, a torn frame, or rows that do not line up at strip boundaries
- wrong colours / gradients stepping the wrong way
- garbled or clipped text
- a white, black or blank screen (link dead at this clock)

Report back:

- the highest step whose checkerboard, stripes, bars and text stay clean
- the per-step `[anim]` full-frame and partial fps lines
- the `[check]` mismatch counts (or "no MISO"/"ignored")
- `arm-none-eabi-size` for the ELF and the board/module/wiring used
  (breadboard vs short wires, SDO connected or not, pad drive setting)

Press `t` at any step for the summary table: step, target and actual SPI MHz,
full-frame fps, partial fps and readback mismatches.

## PLL settings used

`clk_sys` = 150 MHz, XOSC = 12 MHz. The 37.5 MHz safe step uses `clk_sys`
directly. For the rest, PLL_USB is programmed with VCO = `fbdiv * 12 MHz` and
integer post-dividers:

| target SPI | clk_peri wanted | VCO | pd1 | pd2 | actual clk_peri | actual SPI |
|---|---|---|---|---|---|---|
| 37.5 | 150.000 (clk_sys) | - | - | - | 150.000 | 37.500 |
| 62.5 | 125.000 | 1500 | 6 | 2 | 125.000 | 62.500 |
| 66.67 | 133.340 | 1200 | 3 | 3 | 133.333 (nearest) | 66.666 |
| 70.0 | 140.000 | 1260 | 3 | 3 | 140.000 | 70.000 |
| 75.0 | 150.000 | 1500 | 5 | 2 | 150.000 | 75.000 |
| 80.0 | 160.000 | 1440 | 3 | 3 | 160.000 | 80.000 |
| 85.0 | 170.000 | 1020 | 6 | 1 | 170.000 | 85.000 |
| 90.0 | 180.000 | 1440 | 4 | 2 | 180.000 | 90.000 |
| 100.0 | 200.000 | 1200 | 6 | 1 | 200.000 | 100.000 |

All overclock steps use `CPSDVSR = 2, SCR = 0`; the safe 37.5 MHz step uses
`CPSDVSR = 2, SCR = 1` because 150 MHz / 37.5 MHz needs a divide-by-4.

## Build and flash

```sh
cmake -S . -B build
cmake --build build --target spi_overclock
cp build/apps/spi_overclock/spi_overclock.uf2 dist/spi_overclock.uf2
```

Hold BOOTSEL, plug in USB, drag `dist/spi_overclock.uf2` onto the RP2350
drive. Serial: UART0 on GP0/GP1 at 115200 8N1 (on-board debug probe); USB CDC
is disabled.

The app always boots at normal clocks, 37.5 MHz SPI, before touching PLL_USB,
so a power cycle recovers from any bad step.
