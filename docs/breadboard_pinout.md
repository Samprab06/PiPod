# Breadboard pinout and bring-up

Wiring for the PiPod breadboard: two display builds (ILI9341 and ST7796S), the
microSD card, the six TTP223 touch pads, the physical buttons and the two
stand-alone test apps.

The authoritative pin map is
[`boards/pipod_breadboard_pins.h`](../boards/pipod_breadboard_pins.h) (`PIPOD_PIN_*`
macros). The application reaches it through
[`apps/pipod/pipod_pins.h`](../apps/pipod/pipod_pins.h); the display drivers get
the same numbers through their runtime config structs at init and the SD driver
through `STORAGE_SD_PIN_*` definitions. If a pin moves, it moves in the header
first. A compile-time `_Static_assert` rejects two signals on the same GPIO.

Board: Purdue Proton, RP2350B (48 GPIOs), `clk_sys` 150 MHz, 16 MB flash.
Logic is **3.3 V only**. UART0 (GP0 TX / GP1 RX) goes to the on-board debug
probe at **115200 8N1**.

## 1. Display builds

Both panels share the same SPI0 control pins; only the ILI9341 has a MISO and a
PWM backlight.

### 1a. ILI9341, 320x240 landscape (`pipod`)

| Module pin | Proton | RP2350 | Notes |
|---|---|---|---|
| `VCC` | `3V3` | — | Check the module `J1` jumper; see [wiring.md](wiring.md) §2. |
| `GND` | `GND` | — | Common ground, tie next to the SPI wires. |
| `CS` | `GP33` | SIO | Held low for a whole command+data transaction. |
| `RESET` | `GP37` | SIO | Active low, pulsed by the driver. |
| `DC` / `RS` | `GP36` | SIO | 0 = command, 1 = data. |
| `SDI` / `MOSI` | `GP35` | SPI0 TX | 16-bit pixel frames, MSB first. |
| `SCK` | `GP34` | SPI0 SCK | Mode 0. Default clock 75 MHz. |
| `LED` | `GP38` | PWM11 A | Backlight PWM, starts at 100 %. |
| `SDO` / `MISO` | `GP32` | SPI0 RX | Optional; only needed for ID/register readback. |
| `T_*`, `SD_*` | — | — | Not used; leave unconnected. |

### 1b. ST7796S, 480x320 landscape (`pipod_st7796`)

| Module pin | Proton | RP2350 | Notes |
|---|---|---|---|
| `VCC` | `3V3` | — | 3.3 V only. |
| `GND` | `GND` | — | Common ground. |
| `CS` | `GP33` | SIO | Same as the ILI9341 build. |
| `RST` | `GP37` | SIO | Active low. |
| `DC` | `GP36` | SIO | 0 = command, 1 = data. |
| `SDA` (`MOSI`) | `GP35` | SPI0 TX | 16-bit pixel frames. |
| `SCL` (`SCK`) | `GP34` | SPI0 SCK | Mode 0. Default clock 37.5 MHz. |
| `BL` | `3V3` | — | **Tied to 3.3 V**, always on; backlight control is a no-op. |
| `SDA-0` | — | — | Not connected on this module. |

The ST7796 module has **no MISO/SDO**, so there is no register or pixel
readback. The firmware prints `no readback: the module has no MISO`; that is
expected, not a failure.

## 2. microSD card (SPI1)

| microSD breakout | Proton | Notes |
|---|---|---|
| `VCC` | `3V3` | 3.3 V module only; no level shifter. |
| `GND` | `GND` | Short lead, common ground. |
| `MISO` / `DO` | `GP28` | SPI1 RX. |
| `CS` / `SS` | `GP29` | Plain GPIO, active low. |
| `SCK` / `CLK` | `GP30` | SPI1 SCK. |
| `MOSI` / `DI` | `GP31` | SPI1 TX. |
| `CD` | — | No card detect (`use_card_detect = false`). |

A 10 kΩ pull-up from `CS` to `3V3` is recommended by the driver vendor (the
card may drive `CS` during power-up). It is optional for one card on a short
bus. The card is initialized at 400 kHz and then switched to 25 MHz; lower it
with `-DSTORAGE_SD_SPI_HZ=12500000` or `storage_set_spi_hz()` if the wiring is
marginal.

### Card format

From the vendored FatFs configuration
(`third_party/no-OS-FatFS/src/include/ffconf.h`):

* **FAT32** is recommended; **exFAT is supported** (`FF_FS_EXFAT = 1`).
  FAT12/FAT16 also mount.
* Long file names up to 255 UTF-8 characters (`FF_USE_LFN = 3`,
  `FF_LFN_UNICODE = 2`); non-ASCII names work when written as UTF-8.
* One volume on an MBR partition table, or a superfloppy with no partition
  table. The first volume mounts as `0:`.
* No journaling or replay: unmount / power off only when no file is open.
* The PiPod only ever reads the card (`f_write()` and friends are never
  called).

## 3. TTP223 touch pads

Six momentary capacitive switches. Each module has `VCC`, `GND` and `OUT`.

| Signal | Module OUT -> | UI action |
|---|---|---|
| slider LEFT | `GP2` | slider step, one wheel tick (with the UI's smooth scroll) |
| slider MIDDLE | `GP3` | slider step |
| slider RIGHT | `GP4` | slider step |
| SELECT | `GP5` | `UI_BTN_SELECT` click / long press |
| MENU | `GP6` | `UI_BTN_MENU` |
| PLAY/PAUSE | `GP7` | `UI_BTN_PLAY_PAUSE` |

Power each module from `3V3` and `GND`; `OUT` goes straight to the GPIO (no
series resistor needed, but 100 Ω in series is harmless if the lead is long).

Notes:

* Default jumpers = **momentary, active high** (OUT high while touched). If a
  module is configured active low, build with `-DPIPOD_TOUCH_ACTIVE_HIGH=0`.
* **Do not touch any pad during power-up**: the modules self-calibrate then,
  and a finger on a pad at that moment becomes the new "released" baseline.
  Both `pipod` and `touchpad_test` print a reminder at boot.
* The driver leaves the internal pulls **off**. The modules drive both levels
  themselves, and the RP2350-E9 erratum makes internal pull-downs unreliable.
* The slider is not a position sensor: only the order in which LEFT/MIDDLE/
  RIGHT activate is decoded. Adjacent activations within 400 ms are one step;
  holding the end pad repeats after 500 ms, then every 150 ms.

## 4. Physical buttons

Active high with **external 10 kΩ pull-downs** (the firmware disables internal
pulls because of RP2350-E9). The two on-board Proton buttons already have
pull-downs fitted.

| Signal | GPIO | UI action | Pull-down |
|---|---|---|---|
| PREV | `GP8` | `UI_BTN_PREV` | external 10k-47k |
| NEXT | `GP9` | `UI_BTN_NEXT` | external 10k-47k |
| SELECT | `GP21` | `UI_BTN_SELECT` | on-board |
| MENU | `GP26` | `UI_BTN_MENU` | on-board |

The touch pads replace the old GP2/GP3/GP4 wheel and play/pause buttons, so the
breadboard build passes its own button map (`apps/pipod/main.c`) and never uses
`input_buttons_default_map()`.

## 5. Reserved pins and free pins

| GPIO | Use |
|---|---|
| `GP0` / `GP1` | UART0 TX / RX, on-board debug probe |
| `GP2`-`GP7` | TTP223 pads |
| `GP8` / `GP9` | PREV / NEXT buttons |
| `GP10`-`GP20` | free |
| `GP21` | on-board SELECT |
| `GP22`-`GP25` | on-board LEDs (red, yellow, green, blue), unused |
| `GP26` | on-board MENU |
| `GP27` | free |
| `GP28`-`GP31` | microSD on SPI1 |
| `GP32`-`GP38` | display SPI0 (`GP32`/`GP38` ILI9341 only) |
| `GP39`-`GP47` | free; `GP40`-`GP47` are the RP2350B ADC pins |

## 6. Power notes

* Supply the Proton from USB; do not back-feed `3V3` from another source.
* Every module here is 3.3 V logic. Never drive a signal pin from 5 V.
* ILI9341 `VCC`: check the module's `J1` jumper. With `J1` bridged, feed
  `3V3` only; with `J1` open the module has its own regulator and can take 5 V.
  Never feed 5 V into `VCC` with `J1` bridged.
* The ST7796 backlight is tied to `3V3` and is always on; budget its current
  (typically 100-200 mA) in the USB supply.
* Keep `SCK`/`MOSI` leads under ~10 cm and run a ground return next to `SCK`.
  A 33 Ω series resistor on `SCK` at the Proton end is a cheap damper if you
  see speckled pixels.
* Never hot-plug the display or the SD card while powered.

## 7. Build

Configure once from the repository root (`D:/362Project`):

```sh
cmake -S . -B build_int2 -G Ninja \
  -DCMAKE_MAKE_PROGRAM=C:/Users/ekagr/AppData/Local/Temp/banyancode/ninja/ninja.exe \
  -DPICO_SDK_PATH=D:/362Project/tools/pico-sdk \
  -DPICO_TOOLCHAIN_PATH=D:/362Project/tools/arm-gnu \
  -DPICO_PLATFORM=rp2350-arm-s -DPICO_BOARD=proton
```

Then build the image you need:

```sh
cmake --build build_int2 --target pipod           # ILI9341, 320x240
cmake --build build_int2 --target pipod_st7796    # ST7796S, 480x320
cmake --build build_int2 --target sd_test         # microSD bring-up (no display)
cmake --build build_int2 --target touchpad_test   # TTP223 bring-up (no display)
```

Outputs land in `build_int2/apps/<app>/` (`<app>.elf`, `.uf2`, `.bin`, `.hex`,
`.dis`). Flash one UF2 at a time:

| UF2 | Use |
|---|---|
| `build_int2/apps/pipod/pipod.uf2` | full PiPod UI on the ILI9341 |
| `build_int2/apps/pipod/pipod_st7796.uf2` | full PiPod UI on the ST7796S |
| `build_int2/apps/sd_test/sd_test.uf2` | SD card test only |
| `build_int2/apps/touchpad_test/touchpad_test.uf2` | touch pad test only |

Hold `BOOTSEL`, plug the board in, and copy the UF2 to the `RP2350` drive (or
`picotool load -x <path>.elf`). All four use **UART0 only** for stdio; open the
debug probe's serial port at 115200 8N1.

### Firmware size

`pipod` and `pipod_st7796` both run
`cmake/check_firmware_size.cmake`, which fails the build if the image would
overlap the 1 MB media-pack region at `0x00100000`. The 480x320 precomputed
asset set is large, so `pipod_st7796` leaves out the six generated demo covers
(`PIPOD_DEMO_ART=0`) and uses the UI's built-in placeholder when the demo
tables are the active library. SD-card and flash-pack album art is unaffected.
Current sizes: `pipod` ~744 KB, `pipod_st7796` ~958 KB.

## 8. Library source at boot

`pipod` picks the first usable source on core 0, before core 1 starts (flash
cache writes stall XIP):

1. **microSD card**: mount `0:`, scan `0:/` recursively for
   `.mp3/.flac/.wav/.ogg/.opus`, cache the index in the shared 1 MB flash
   cache. Metadata only; no audio bytes are decoded.
2. **Flash media pack** at `0x00100000` (e.g. `dist/test_songs.uf2`).
3. **Built-in demo tables** (8 artists, 14 albums, 166 songs).

The UART prints `[sd] ...` and `[media] <source>: N tracks, N albums, N
artists from N files`, plus per-file `[scan]` lines. A second boot with the
same card/pack is a cache hit (no tag parsing, no art decodes).

## 9. Future PCB pin header

The PCB revision plugs in by adding `boards/pipod_pcb_pins.h` with the same
`PIPOD_PIN_*` names and rebuilding with `-DPIPOD_PINS=PCB`. Nothing else
changes:

* `apps/pipod/pipod_pins.h` includes whichever header `PIPOD_PINS_HEADER`
  names (set by the top-level `CMakeLists.txt`), so `main.c` and the two test
  apps compile unchanged.
* The display config structs in `main.c` are filled from `PIPOD_PIN_DISPLAY_*`.
* `lib/storage/storage_hw_config.c` uses `STORAGE_SD_PIN_*`, which default to
  the breadboard numbers when no board header is selected and follow
  `PIPOD_PIN_SD_*` when one is.
* The compile-time collision check runs against the new header automatically.

The current build rejects `-DPIPOD_PINS=PCB` with a clear error until that
header exists; it is intentionally not guessed.
