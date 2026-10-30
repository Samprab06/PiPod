# PiPod — iPod-style music player UI for the Purdue Proton (RP2350)

A framebuffer-less, 30 FPS iPod-classic UI and ILI9341 display driver for the
Purdue Proton (RP2350B, 520 KB SRAM, no PSRAM). The UI runs on core 1 and owns
the panel; the audio decoder + I2S run on core 0 and talk to it through a small
cross-core API (`lib/ui/ui.h`). There is no full framebuffer: dirty rectangles
are rendered into two 320x16 RGB565 strips and pushed with one DMA channel.

Two applications:

| App | Purpose |
|---|---|
| `display_test` | ILI9341 bring-up: readback self-test, orientation frame, solid fills, colour bars, checkerboard, SPI clock/fill/blit benchmarks, 30 FPS bouncing square. |
| `pipod` | The real UI driven by a deterministic fake library (8 artists, 14 albums, 166 songs, 3 playlists) and a fake playback engine, with an audio-jitter meter and `[perf]` counters. |

---

## Hardware

- **Board:** Purdue Proton, RP2350B (QFN-80), 48 GPIOs, 520 KB SRAM, 16 MB QSPI
  flash, 12 MHz crystal, `clk_sys` 150 MHz (not overclocked).
- **Display:** 2.2" ILI9341 SPI TFT, 240x320 native, driven in **landscape
  320x240**, 4-wire SPI, RGB565, backlight PWM on GP38.
- **Controls (breadboard demo):** 7 active-high buttons with **external
  pull-downs** (RP2350-E9 makes internal pull-downs unreliable), plus a UART
  keyboard over the on-board debug probe.
- Full wiring, power and signal-integrity notes: [`docs/wiring.md`](docs/wiring.md).

| Signal | GPIO |
|---|---|
| Display MISO / CS / SCK / MOSI / DC / RST / BL | GP32 / GP33 / GP34 / GP35 / GP36 / GP37 / GP38 |
| Wheel CW / CCW, play/pause, prev, next | GP2 / GP3 / GP4 / GP8 / GP9 |
| Select (on-board) / Menu (on-board) | GP21 / GP26 |
| UART0 TX / RX to the debug probe (115200 8N1) | GP0 / GP1 |

---

## Repository layout

```
.
├── CMakeLists.txt              top-level build (PICO_BOARD_HEADER_DIRS = boards/)
├── pico_sdk_import.cmake       copied from the Pico SDK
├── boards/proton.h             custom board header (RP2350B, UART0, LEDs, clocks)
├── lib/
│   ├── display/                ILI9341 driver: SPI + DMA + panel commands
│   │   ├── ili9341.c/.h
│   │   └── ili9341_config.h    pins / SPI clock / panel size (single source of truth)
│   ├── gfx/                    RGB565 canvas primitives, fonts, dirty-rect renderer
│   │   ├── gfx.c, gfx.h
│   │   ├── gfx_render.c/.h     strip renderer + dirty rectangles
│   │   └── fonts/              generated Inter faces (4bpp alpha)
│   ├── ui/                     screens, animations, input queues, public ui.h
│   ├── input/                  hardware -> abstract UI events
│   │   ├── input_buttons.c/.h  breadboard buttons + wheel pseudo-buttons
│   │   ├── input_uart.c/.h     keyboard over UART0
│   │   └── input_wheel.c/.h    capacitive-wheel angle -> ticks (MPR121 glue)
│   └── ui_demo_assets/         generated 120x120 demo album art (const, flash)
├── apps/
│   ├── display_test/           driver validation + benchmarks
│   └── pipod/                UI demo; sim_library / sim_player / audio_load_sim
├── assets/
│   ├── fonts/Inter-Variable.ttf + OFL.txt
│   └── demo_art/art_0..5.png   source PNGs for the generated covers
├── scripts/                    font/image/art generators (see scripts/README.md)
├── docs/
│   ├── wiring.md               physical wiring + bring-up checklist
│   ├── api.md                  integration guide for the other workstreams
│   └── performance.md          SPI theory + measurement procedures (results pending)
├── tools/                      vendored toolchain/SDK/picotool (gitignored)
└── build/                      CMake build tree (gitignored)
```

---

## Prerequisites

The build is cross-compiled with the Pico SDK for `rp2350-arm-s` and the custom
board header `boards/proton.h`.

| Component | Version used here | Expected location |
|---|---|---|
| CMake + Ninja | CMake >= 3.13, Ninja | on `PATH` |
| Arm GNU Toolchain (arm-none-eabi) | **14.2.Rel1** (gcc 14.2.1, 2024-11-19) | `tools/arm-gnu/` (`bin/arm-none-eabi-gcc`) |
| Raspberry Pi Pico SDK | **2.3.1** (with submodules) | `tools/pico-sdk/` |
| picotool | **2.3.1** | `tools/picotool/picotool/` (CMake package + `picotool.exe`) |
| Python + Pillow (assets only) | Python **3.11**, Pillow **11.2** | system Python |

`tools/` and `build/` are **not committed** (`tools/` is gitignored because it
vendors ~2 GB of toolchain/SDK). To install them:

1. **Arm GNU Toolchain 14.2.rel1** — download the `arm-none-eabi` Windows/Linux
   build from Arm, extract so that `tools/arm-gnu/bin/arm-none-eabi-gcc` exists.
2. **Pico SDK 2.3.1** — clone `raspberrypi/pico-sdk` at tag `2.3.1` into
   `tools/pico-sdk` and run `git submodule update --init` (tinyusb, mbedtls,
   etc. are needed by `pico_sdk_init`).
3. **picotool 2.3.1** — download the release for your OS and extract it so that
   `tools/picotool/picotool/picotoolConfig.cmake` and `picotool.exe` exist
   (that directory is what `-Dpicotool_DIR` points at).

Any other locations work too: pass the corresponding paths to CMake below. If
you set the `PICO_SDK_PATH` environment variable instead of `-DPICO_SDK_PATH`,
`pico_sdk_import.cmake` picks it up.

---

## Configure and build

From the repository root (bash syntax; use the absolute paths for your checkout):

```sh
cmake -S . -B build -G Ninja \
  -DPICO_SDK_PATH=D:/362Project/tools/pico-sdk \
  -DPICO_TOOLCHAIN_PATH=D:/362Project/tools/arm-gnu \
  -Dpicotool_DIR=D:/362Project/tools/picotool/picotool \
  -DPICO_PLATFORM=rp2350-arm-s \
  -DPICO_BOARD=proton

cmake --build build --target display_test pipod
```

Notes:

- `-DPICO_BOARD=proton` selects `boards/proton.h`. The top-level
  `CMakeLists.txt` adds `boards/` to `PICO_BOARD_HEADER_DIRS` **before**
  `pico_sdk_init()`, so no extra flag is needed for the custom board header.
- `-DPICO_PLATFORM=rp2350-arm-s` is the Arm (not RISC-V) RP2350 build.
- The build defaults to `Release` in the existing tree; add
  `-DCMAKE_BUILD_TYPE=Debug` if you want symbols.
- `apps/pipod` is skipped with a status message if the `ui` target is not
  configured (it is present in this tree).

Outputs:

```
build/apps/display_test/display_test.{elf,uf2,bin,hex,dis}
build/apps/pipod/pipod.{elf,uf2,bin,hex,dis}
```

---

## Flash and run

1. **`display_test` first.** Hold `BOOTSEL`, plug USB, and drag
   `build/apps/display_test/display_test.uf2` onto the `RP2350` mass-storage
   drive; or with a debug probe:

   ```sh
   tools/picotool/picotool/picotool.exe load -x build/apps/display_test/display_test.elf
   ```

   Open the probe's UART at **115200 8N1** (GP0 TX / GP1 RX). Both apps route
   stdio to UART0 only; USB CDC is disabled and shows nothing.

   Expect `=== ILI9341 display_test (Purdue Proton) ===`, the self-test block,
   solid fills, colour bars, a white frame with **TL red / TR green / BL blue /
   BR yellow** corner markers, a checkerboard, the SPI benchmarks and a bouncing
   square. `docs/wiring.md` has the full PASS/FAIL triage table.

2. **`pipod` second.** Flash `build/apps/pipod/pipod.uf2` the same way.
   Expect the display init/ID line, the pin banner, then a `[perf]` block every
   5 s.

Controls (buttons and UART keys) are listed in
[`apps/pipod/README.md`](apps/pipod/README.md). In short: wheel = scroll /
volume, SELECT = activate, MENU = back (long = home), PLAY_PAUSE = toggle,
NEXT/PREV = track change (long = seek). UART: `w`/`s` wheel, `e` select,
`m` menu, space pause, `n`/`p` next/prev, `h` home, `P` perf report.

---

## Documentation

| Document | Contents |
|---|---|
| [`docs/api.md`](docs/api.md) | Integration guide: architecture, threading rules, startup sequence, command/state API, input adapters (including MPR121), `ui_library_t`, driver/gfx summaries, resource contract. **Read this first if you are integrating.** |
| [`docs/performance.md`](docs/performance.md) | SPI clock arithmetic, achievable-clock caveats, 30 FPS rendering design, benchmark procedures, audio-jitter method, memory footprint. Hardware result tables are still empty placeholders. |
| [`docs/wiring.md`](docs/wiring.md) | Physical wiring, module VCC/backlight caveats, signal integrity, bring-up checklist and triage. |
| [`apps/pipod/README.md`](apps/pipod/README.md) | Demo build/flash, controls, `[perf]` line reference. |
| [`scripts/README.md`](scripts/README.md) | Asset generators, font metrics/flash costs, host syntax check. |

---

## Regenerating assets

Fonts and demo art are generated; do not edit `lib/gfx/fonts/*` or
`lib/ui_demo_assets/*` by hand.

```sh
python -I scripts/regen_assets.py            # fonts + demo art + preview
python -I scripts/regen_assets.py --fonts    # fonts only (+ fonts.h / fonts.cmake)
python -I scripts/regen_assets.py --art      # demo art only (PNG + C)
```

Requires Python 3.11 + Pillow 11.2, no numpy. Details and per-script flags:
[`scripts/README.md`](scripts/README.md).

To add a real cover image:

```sh
python -I scripts/gen_image.py --in cover.jpg --name album_cover \
    --size 120x120 --dither fs --out-dir lib/ui_demo_assets
```

Album art must be RGB565, row-major, native `uint16_t`, ideally 120x120 (other
sizes are scaled by the UI).

---

## Licenses and provenance

- **Inter font** by Rasmus Andersson, **SIL Open Font License 1.1**. The license
  text is `assets/fonts/OFL.txt` and must travel with the TTF; the generated
  bitmaps in `lib/gfx/fonts/` are derived from it and carry the same license.
- **Demo album art** is procedurally generated by `scripts/gen_demo_art.py`
  (deterministic seeds, no third-party imagery). Source PNGs are in
  `assets/demo_art/`.
- The Pico SDK and third-party SDK submodules keep their own licenses; they are
  vendored under `tools/` and not part of this repository's sources.
- No top-level license file exists for this project yet; treat the source as
  course/project work until the team adds one.

---

## Scope

This workstream owns the **display driver, graphics primitives, UI and the
input adapters**. It does **not** implement:

- audio decoding or the I2S/PIO output (the `pipod` "player" and
  `audio_load_sim` are fakes for testing);
- the SD card filesystem or the music-library index (the UI only calls the
  `ui_library_t` provider; see `apps/pipod/sim_library.c` for the contract);
- the MPR121 driver or the capacitive-touch hardware (only the angle -> tick
  glue in `lib/input/input_wheel.c` is here);
- the physical PCB, power and enclosure.

**Current status:** the firmware builds and both apps are ready to flash; host
unit tests cover the renderer and the wheel math. The display and UI have **not
yet been measured on hardware**, so the result tables in
[`docs/performance.md`](docs/performance.md) are intentionally empty placeholders
for the first board run.
