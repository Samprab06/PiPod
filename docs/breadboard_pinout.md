# PiPod breadboard: final-PCB pin map

Purdue Proton, RP2350B, Pico SDK 2.3.1. **All logic is 3.3 V; every module is powered from 3V3 except the microSD module (5 V, see below).**
Share a common ground and keep SPI leads short. The authoritative map is
[`boards/pipod_breadboard_pins.h`](../boards/pipod_breadboard_pins.h); its
compile-time range and collision checks include the reserved DAC signals.
GP22–25 on-board LEDs are unused.

## ST7796S 3.5-inch display (primary: `pipod_st7796`)

| Module pin | Proton pin | Notes |
|---|---|---|
| GND | GND | Common ground |
| VCC | 3V3 | 3.3 V only |
| SCL | GP2 | SPI0 SCK, not I2C |
| SDA | GP3 | SPI0 MOSI, not I2C |
| RST | GP5 | Active-low reset |
| DC | GP4 | Command/data |
| CS | GP14 | Active-low GPIO chip select |
| BL | 3V3 | Always on; no PWM GPIO |
| SDA-0 | NC | Leave unconnected; no MISO/readback |

The ST7796 build retains its 480×320 landscape output, **75 MHz SPI0 default**
and no-op backlight control. Readback/self-test is unsupported, not a failure.
The alternative ILI9341 `pipod` build uses the same five SPI0/control pins,
320×240 landscape, with SDO/MISO unconnected and LED/BL tied to 3V3;
readback/self-test and backlight PWM must be disabled on this map. Check the
ILI9341 module's regulator/jumper and backlight current requirements before
powering it; do not connect its old GP32/GP38 leads.

SPI0 (display) and SPI1 (SD) are independent PL022 blocks with independent
dividers: display 75 MHz and SD 25 MHz can run simultaneously at the stock
150 MHz peripheral clock. Both apps print `spi_get_baudrate()` for both buses
after mounting (an absent card can leave SD at the slow initialisation rate).
**75 MHz is an overclock, far above the ST7796 rated serial clock**, not a
guaranteed operating point. Verify long-run stability on the breadboard with
short wires and fast pad drive (12 mA, fast slew, enabled by default). Use
**Settings → Display Clock → 37.5 MHz** if pixels corrupt or the panel stalls.
ST7796 PiPod targets 60 FPS; ILI9341 stays at 30 FPS. `[perf]` reports achieved
UI cadence, rendered/idle frames and SPI/DMA busy time; 60 FPS is a pacing
target, not a promise of full-screen 60 Hz transfers over serial SPI.

The ILI9341 driver accepts `ILI9341_GPIO_UNUSED` (0xFF) for MISO and BL.
This map selects those defaults: no GPIO is configured for either signal,
readback reports "no readback", and brightness control is a no-op. Supplying
real MISO/BL pins retains readback self-test and PWM behaviour.

## MPR121 capacitive touch breakout

| Module pin | Proton pin | Notes |
|---|---|---|
| 3.3V | 3V3 | Supply and logic reference |
| GND | GND | Common ground |
| SDA | GP6 | I2C1 SDA |
| SCL | GP7 | I2C1 SCL |
| IRQ | GP12 | Active low, open drain; pull up to 3V3 |
| ADD | GND | Approved address 0x5A |
| Electrodes 0–10 | Bare wires | Arrange around a circle for now |
| Electrode 11 | Bare wire | Centre electrode |

| ADD connection | 7-bit I2C address |
|---|---|
| GND | 0x5A |
| 3V3 | 0x5B |
| SDA | 0x5C |
| SCL | 0x5D |

**Do not tie ADD to 3V3 if the back solder jumper is bridged:** the jumper
may already connect ADD to GND, causing a short. Inspect the breakout first.
PiPod initialises and polls the MPR121 using this map. If it is missing, a
UART warning is printed; on-board buttons and UART controls remain usable.

## Bring-up order

1. With power off, check wiring, common ground, ADD and the 3V3 supply.
2. Flash `build_c1/apps/hw_test/hw_test.uf2`. **Do not touch the electrode
   wires during power-up: the MPR121 calibrates then.**
3. Check the ST7796 480×320 screen. The left side shows the SD mount result,
   card type/capacity, recursive MP3 count, scan time and first five songs
   (`title - artist`, filename fallback). A missing card or scan error is
   shown without stopping the touch test. The walk is read-only and bounded
   by the storage layer's 12-folder depth limit.
4. Check the MPR121 status and all 12 numbered touch boxes: ELE0 at the top,
   ELE1–ELE10 clockwise in a circle, ELE11 in the middle. Each touched electrode should highlight and appear in
   `Pressed:`. UART0 prints press/release edges; `d` requests filtered/baseline
   debug data, also printed every two seconds while touching.
   The bottom line shows the live angle (tenths of a degree; `off` means no ring
   contact), accumulated ticks and last decoded click/hold/scroll event.
5. Flash `build_c1/apps/pipod/pipod_st7796.uf2` and check navigation using
   the mapping below (also printed at boot).

### Real click-wheel wiring and gestures

PCB footprint: `PiPod_ClickWheel_MPR121_48mm`; MPR121 ELEn connects to wheel
pad n+1. Place bare wires the same way: **11 equal ring sectors**, ELE0
centred at 12 o'clock, increasing **clockwise**, ELE11 at the centre.

| Ring electrode | Clockwise angle from top | Tap / stationary hold function |
|---|---|---|
| ELE0 | 0° | MENU |
| ELE1 | 32.7° | Scroll-only |
| ELE2 | 65.5° | Scroll-only |
| ELE3 | 98.2° | NEXT |
| ELE4 | 130.9° | Scroll-only |
| ELE5 | 163.6° | PLAY/PAUSE |
| ELE6 | 196.4° | PLAY/PAUSE |
| ELE7 | 229.1° | Scroll-only |
| ELE8 | 261.8° | PREV |
| ELE9 | 294.5° | Scroll-only |
| ELE10 | 327.3° | Scroll-only |
| ELE11 | Centre, not ring | SELECT (immediate edges) |

| Gesture | Decoder output |
|---|---|
| Compass touch, release within 350 ms, excursion ≤25° | Button DOWN then UP (click), no scroll |
| Compass stationary hold ≥500 ms | DOWN at 500 ms, UP on release; UI decides long press |
| Compass touch moves >25° before hold commits | Scroll only; flush ticks accumulated since first movement, no button |
| Starts on non-compass ring pad | Scroll immediately from first movement |
| Clockwise / counter-clockwise slide | Positive / negative ticks, wrap-safe across 359°→0° |
| Centre press / release | SELECT DOWN / UP; cancel ring candidate/hold and suppress ring while centre touched |

Release between 350 and 500 ms produces no event. A committed hold stays a
button until release (later movement does not scroll). UI long-press timing
starts at the emitted DOWN, so the default 600 ms UI threshold fires about
1100 ms after initial ring contact. NEXT/PREV seek, MENU goes home; PLAY's
long action is whatever the UI implements (no new power menu is added).

Defaults in `input_clickwheel_default_config`: 24 ticks/rev, 4° reversal
hysteresis, 25° movement threshold, 350 ms tap, 500 ms hold, delta threshold
6 sensor counts. Weighted circular centroid uses baseline−filtered (clamped
at zero) on touched ring electrodes. Both apps poll at approximately 200 Hz.
The config also maps clockwise ring electrode order, centre and per-electrode
functions; no decoder changes are needed for another board layout.

## microSD (SPI1)

| Module pin | Proton pin | Notes |
|---|---|---|
| VCC | **5 V (VBUS)** | Our module has an AMS1117-3.3 regulator and a 74LVC125 buffer, so it needs 5 V in; at 3V3 the card gets ~2.2 V and mounts fail with FR_NOT_READY. The data pins stay 3.3 V. A bare module without a regulator goes on 3V3 instead |
| GND | GND | Common ground |
| MISO / DO | GP8 | SPI1 RX |
| CS / SS | GP9 | Active-low GPIO; 10 kΩ pull-up to 3V3 recommended |
| SCK / CLK | GP10 | SPI1 clock |
| MOSI / DI | GP11 | SPI1 TX |
| CD | NC | No card detect |

Initialisation uses a slow clock, then 25 MHz. Use `storage_set_spi_hz()` to
lower the running clock for marginal wiring. `sd_test` prints mount/card
diagnostics and walks the card without writing it.
Mount retries retain the existing 25 / 12.5 / 5 / 1 MHz sequence. The SD
hardware config explicitly supplies `spi1`; the vendor driver uses that
instance for transfers and divider changes, never the display's SPI0.

### Music format

Use **MP3, 44.1/48 kHz, CBR or VBR up to 320 kbps**. ID3v2.3 tags are
recommended; use embedded **baseline JPEG** covers (not progressive JPEG).
Format the card **FAT32 or exFAT**. Supported library limits: up to **12 folder
levels, 512 songs, 128 albums and 128 artists**. The current fake player does
not decode audio; the SD scan → flash pack → demo fallback is unchanged.

## Optional TTP223 test slider (`touchpad_test` only)

| Module pin | Proton pin | Notes |
|---|---|---|
| VCC (all three) | 3V3 | Default momentary, active-high modules |
| GND (all three) | GND | Common ground |
| OUT, pad 1 | GP27 | LEFT |
| OUT, pad 2 | GP28 | MIDDLE |
| OUT, pad 3 | GP29 | RIGHT |
| SELECT / MENU / PLAY-PAUSE pads | NC | Unused, GPIO sentinel 0xFF |

Do not touch pads during power-up self-calibration. The modules drive both
levels, so internal pulls are disabled. PiPod itself does not initialise or
poll these optional test pads.

## On-board pins

| Module pin | Proton pin | Notes |
|---|---|---|
| Debug probe UART RX | GP0 | UART0 TX, 115200 8N1 |
| Debug probe UART TX | GP1 | UART0 RX |
| SELECT button | GP21 | Active high, on-board pull-down |
| MENU button | GP26 | Active high, on-board pull-down |
| LEDs | GP22–25 | Unused |

No external PREV/NEXT buttons remain. UART: `w`/`k`/Up = wheel −1,
`s`/`j`/Down = wheel +1, Enter/`e` = select, `m`/Esc = menu,
space = play/pause, `n`/Right = next, `p`/Left = previous, `h` = home,
`P` = performance report.

## TLV320DAC3100 reserved pins — not wired yet

| Module pin / signal | Proton pin | Notes |
|---|---|---|
| Headphone detect | GP13 | Reserved; not wired yet |
| DAC reset | GP15 | Reserved; not wired yet |
| I2C0 SDA | GP16 | Reserved; not wired yet |
| I2C0 SCL | GP17 | Reserved; not wired yet |
| I2S DIN | GP18 | Reserved; not wired yet |
| I2S WCLK | GP19 | Reserved; not wired yet |
| I2S BCLK | GP20 | Reserved; not wired yet |

Do not drive or initialise these signals before the DAC wiring task.

## Rewiring checklist (power off first)

1. Remove all six old TTP223 connections on GP2–7 and external PREV/NEXT
   buttons on GP8/GP9. These pins now carry display, MPR121 and SD signals.
2. Move display SCK GP34 → GP2, MOSI GP35 → GP3, DC GP36 → GP4,
   RST GP37 → GP5, CS GP33 → GP14. Disconnect old MISO GP32 and PWM GP38;
   connect BL to 3V3. Leave SDA-0 NC.
3. Move SD MISO GP28 → GP8, CS GP29 → GP9, SCK GP30 → GP10,
   MOSI GP31 → GP11.
4. Wire MPR121 to GP6/GP7/GP12, ADD to GND; inspect its back jumper.
5. Optionally reconnect only three TTP223 pads to GP27–29 for `touchpad_test`.
6. Keep on-board GP21/GP26 unchanged. Leave GP13, GP15–20 reserved.
7. Check continuity, 3V3/GND and shorts before powering up. Flash the correct
   display UF2 and inspect UART diagnostics; test SD separately with `sd_test`.

## Build

Select `PIPOD_PINS=BREADBOARD` (default); all owned hardware targets use the
same header. A future `PIPOD_PINS=PCB` requires `boards/pipod_pcb_pins.h`.

```sh
cmake -S . -B build_c1 -G Ninja \
  -DCMAKE_MAKE_PROGRAM=C:/Users/ekagr/AppData/Local/Temp/banyancode/ninja/ninja.exe \
  -DPICO_SDK_PATH=D:/362Project/tools/pico-sdk \
  -DPICO_TOOLCHAIN_PATH=D:/362Project/tools/arm-gnu \
  -DPICO_PLATFORM=rp2350-arm-s -DPICO_BOARD=proton \
  -Dpicotool_DIR=D:/362Project/tools/picotool/picotool
cmake --build build_c1
```

UF2s are under `build_c1/apps/<app>/`. Firmware size checks retain the flash
pack boundary at offset 0x100000. Flash only the intended display image.
