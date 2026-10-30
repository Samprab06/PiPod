# apps/pipod — simulated PiPod UI

Runs the real `lib/ui` on the Purdue Proton with a fake music library and a
fake audio engine, so the screens, input and audio-isolation budget can be
checked before the SD card and I2S code exist.

## Build

From the repository root (`D:/362Project`):

```sh
cmake -S . -B build -G Ninja \
  -DPICO_SDK_PATH=D:/362Project/tools/pico-sdk \
  -DPICO_TOOLCHAIN_PATH=D:/362Project/tools/arm-gnu \
  -Dpicotool_DIR=D:/362Project/tools/picotool/picotool \
  -DPICO_PLATFORM=rp2350-arm-s -DPICO_BOARD=proton
cmake --build build --target pipod
```

Outputs land in `build/apps/pipod/` (`pipod.elf`, `.uf2`, `.bin`,
`.hex`, `.dis`). `lib/ui` must be present: the app is skipped at configure
time when the `ui` target does not exist yet.

## Flash and run

Hold BOOTSEL, plug the board in, then either drag
`build/apps/pipod/pipod.uf2` onto the `RP2350` USB drive or:

```sh
picotool load -x build/apps/pipod/pipod.elf
```

The on-board debug probe exposes UART0 (GP0 TX, GP1 RX) at **115200 8N1**.
Open it with any serial terminal (`putty -serial COMx`, `screen /dev/ttyACM0
115200`, `minicom`, ...). The banner prints the clock, pin map, the ILI9341
ID and the actual SPI clock; `0x009341` is a genuine ILI9341.

## Controls

Breadboard buttons (all active high, **external 10 k pull-downs required** —
RP2350-E9 makes internal pull-downs unreliable):

| Signal | GPIO | UI action |
|---|---|---|
| wheel clockwise | GP2 | +1 wheel tick (auto-repeat 250 ms / 80 ms) |
| wheel counter-clockwise | GP3 | -1 wheel tick |
| play/pause | GP4 | PLAY_PAUSE click |
| previous | GP8 | PREV |
| next | GP9 | NEXT |
| select | GP21 (on-board) | SELECT |
| menu | GP26 (on-board) | MENU |

UART keys (sent to the same serial console):

| Key | Action |
|---|---|
| `w`, `k`, Up arrow | wheel -1 (counter-clockwise) |
| `s`, `j`, Down arrow | wheel +1 (clockwise) |
| `e`, Enter | SELECT |
| `m`, Backspace, lone ESC | MENU |
| space | PLAY_PAUSE |
| `n`, Right arrow | NEXT |
| `p`, Left arrow | PREV |
| `h` | go to the home screen |
| `P` | print a performance report immediately |

Tune the busy load with `audio_sim_set_busy_percent()` (default 50 %).

## What the perf lines mean

Printed every 5 s from core 0:

```
[perf] ui: 29.9 fps  cpu 12%  frames 149  idle 1  frame avg 1234 us  max 4000 us ...
[perf] gfx: frames 149  rects 42  bands 300  pixels 153600  last 900 us  max 2200 us
[perf] ili9341: 307200 bytes  128 xfers  dma busy 900 us
[player] playing  3/12  Kestrel North - Amber Hours  0:42/4:10  vol 60
[audio] 1ms irq: cb 5000  jitter avg 2 us  max 37 us  missed 0  over100us 0  busy 50%
```

- `ui fps` — frame slots completed per wall second (`frames + idle_frames` over
  the 5 s window). Target is 30.
- `cpu` — UI-core busy time / wall time, reported by `ui_get_perf()`.
- `frame avg/max` — CPU time per frame that rendered something, including
  input, animation and strip rendering. `max` should stay below the 30 FPS
  budget (33.3 ms) and ideally under one strip's DMA time (81,920 bits /
  37.5 MHz = 2.185 ms).
- `px/frame` — average pixels pushed per rendered frame; idle frames should
  render nothing.
- `dma wait` — time the UI CPU spent waiting for display DMA (should be small;
  the renderer overlaps rendering with DMA).
- `dropped` — input events dropped because the 32-entry UI queue was full.
  A healthy system keeps this at 0 even while spinning the wheel.
- `gfx` — renderer counters: dirty rects, strips (bands) queued, render time.
- `ili9341` — bytes pushed over SPI and time spent in DMA waits in the window.
- `audio` — the fake I2S interrupt: callbacks seen, jitter (actual vs the ideal
  1 ms schedule), deadlines missed by more than 500 us, callbacks whose jitter
  exceeds 100 us (`over100us`), and the requested busy load. The isolation pass
  criterion while the UI animates is `jitter max` < 100 us **and**
  `over100us 0`; `missed 0` alone is weaker because it only fires above 500 us.
  The counters are reset after the whole report has been written, so the
  blocking 115200-baud UART output itself never shows up as audio jitter.
- `player` — current queue position, track and elapsed/duration.

## Simulated library

8 artists, 14 albums, 166 songs (2:30–6:00) and 3 playlists. All names,
durations and playlist membership are fictional and computed from the item id
from const tables — no dynamic allocation. Album art cycles through the six
generated 120x120 covers in `lib/ui_demo_assets`.

## Layout

- `sim_library.c` — `ui_library_t` provider (count / get_item / get_album_art)
- `sim_player.c` — drains `ui_poll_command()`, keeps a play queue, advances
  in real time, reports via `ui_set_*()`
- `audio_load_sim.c` — 1 ms repeating-timer jitter meter + duty-cycled core-0
  busy work
- `main.c` — display init on core 0, UI on core 1, input polling, reporting
