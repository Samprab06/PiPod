# apps/pipod — PiPod UI on the real media pack

Runs the real `lib/ui` on the Purdue Proton, reads the real media library from
the microSD card first, falls back to the 14 MB flash media pack
(`lib/media/media_library.h`), then to the built-in demo tables, and keeps the
fake audio engine, so the screens, input and audio-isolation budget can be
checked before I2S/decoder code exists.

Two images are built from one source: `pipod` for the ILI9341 (320x240) and
`pipod_st7796` for the ST7796S (480x320). The complete breadboard wiring,
pinout, card format and test apps are in
[docs/breadboard_pinout.md](../../docs/breadboard_pinout.md).

The first boot after a card or pack change scans it once; later boots load the
cached index. See "Library source at boot" below.

## Build

From the repository root (`D:/362Project`):

```sh
cmake -S . -B build -G Ninja \
  -DPICO_SDK_PATH=D:/362Project/tools/pico-sdk \
  -DPICO_TOOLCHAIN_PATH=D:/362Project/tools/arm-gnu \
  -Dpicotool_DIR=D:/362Project/tools/picotool/picotool \
  -DPICO_PLATFORM=rp2350-arm-s -DPICO_BOARD=proton
cmake --build build --target pipod           # ILI9341, 320x240
cmake --build build --target pipod_st7796    # ST7796S, 480x320
```

Outputs land in `build/apps/pipod/` (`pipod.uf2`, `pipod_st7796.uf2` plus
`.elf/.bin/.hex/.dis`). `lib/ui`, `lib/media`, `lib/storage`,
`lib/flashstream` and `third_party/no-OS-FatFS` must be present; the app is
skipped at configure time when the required targets do not exist yet.

The board pin map is selected by `PIPOD_PINS` (default `BREADBOARD`, which
resolves `boards/pipod_breadboard_pins.h`). A future PCB revision adds
`boards/pipod_pcb_pins.h` and builds with `-DPIPOD_PINS=PCB`; see
[docs/breadboard_pinout.md](../../docs/breadboard_pinout.md) §9.

The POST_BUILD size check fails the build if an image would grow past 1 MB
(the media pack starts at flash offset 0x100000). The 480x320 asset set is
large, so `pipod_st7796` omits the six generated demo covers
(`PIPOD_DEMO_ART=0`) and draws the UI's placeholder when the demo tables are
active; SD-card and flash-pack art is unaffected.

## Flash and run

Hold BOOTSEL, plug the board in, then either drag the matching UF2
(`build/apps/pipod/pipod.uf2` for the ILI9341, `pipod_st7796.uf2` for the
ST7796S) onto the `RP2350` USB drive or:

```sh
picotool load -x build/apps/pipod/pipod.elf
```

Music can come from either source:

* **microSD card** (preferred): format it FAT32 or exFAT, copy the audio files
  anywhere in the tree and insert it before powering up. The pins are in
  [docs/breadboard_pinout.md](../../docs/breadboard_pinout.md) §2.
* **flash pack**: copy `dist/test_songs.uf2` to the same drive (it lands at
  flash offset 0x100000 and does not touch the firmware). The pack can be
  replaced at any time the same way; the cache key changes with the pack, so
  the next boot rescans.

The on-board debug probe exposes UART0 (GP0 TX, GP1 RX) at **115200 8N1**.
Open it with any serial terminal (`putty -serial COMx`, `screen /dev/ttyACM0
115200`, `minicom`, ...). The banner prints the clock, pin map, the panel name
and the actual SPI clock; on the ILI9341 build `0x009341` is a genuine
ILI9341, and the ST7796 build prints `no readback` because the module has no
MISO.

## Controls

TTP223 touch pads (momentary, active high; do not touch them while the board
powers up, they self-calibrate then):

| Pad | GPIO | UI action |
|---|---|---|
| slider LEFT / MIDDLE / RIGHT | GP2 / GP3 / GP4 | slide one direction = one wheel tick (smooth scroll), hold to repeat |
| SELECT | GP5 | SELECT click / long press |
| MENU | GP6 | MENU |
| PLAY/PAUSE | GP7 | PLAY/PAUSE click |

Physical buttons (active high, **external 10 k pull-downs required** — RP2350-E9
makes internal pull-downs unreliable), plus the two on-board Proton buttons:

| Signal | GPIO | UI action |
|---|---|---|
| previous | GP8 | PREV |
| next | GP9 | NEXT |
| select | GP21 (on-board) | SELECT |
| menu | GP26 (on-board) | MENU |

The old GP2/GP3/GP4 wheel buttons are gone from this build: those pins are the
slider now, and the app passes its own button map instead of
`input_buttons_default_map()`.

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
[perf] display: 307200 bytes  128 xfers  dma busy 900 us
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
- `display` — bytes pushed over SPI and time spent in DMA waits in the window.
- `audio` — the fake I2S interrupt: callbacks seen, jitter (actual vs the ideal
  1 ms schedule), deadlines missed by more than 500 us, callbacks whose jitter
  exceeds 100 us (`over100us`), and the requested busy load. The isolation pass
  criterion while the UI animates is `jitter max` < 100 us **and**
  `over100us 0`; `missed 0` alone is weaker because it only fires above 500 us.
  The counters are reset after the whole report has been written, so the
  blocking 115200-baud UART output itself never shows up as audio jitter.
- `player` — current queue position, track and elapsed/duration.

## Library source at boot

At boot, on core 0 and **before core 1 starts** (flash writes stall XIP, so
they must not overlap the UI), the first usable source wins:

1. **microSD card** (`storage_mount()` then `media_library_scan_sd()` on
   `STORAGE_ROOT`). The card is only read: tags and embedded art come from
   the metadata parser, no audio bytes are decoded.
2. **Flash media pack** at 0x00100000 (the original path:
   `media_pack_open()` / `media_library_scan()`).
3. **Built-in demo tables** (`sim_library.c`: 8 artists, 14 albums, 166 songs,
   3 playlists).

Both scanners share the 1 MB index/cover cache at 0xF00000, so a cache hit
loads the index without parsing tags or decoding JPEGs. `media_library_ui()`
is handed to `ui_init`; it serves artists, albums, songs ("All Songs"
playlist), titles and album art. `sim_player.c` uses `media_library_track()`
for real title/artist/album/duration; the UI resolves the cover from the
library by album id, so Now Playing, the progress bar and NEXT/PREV all run on
the same fake engine with either source.

The UART prints one `[scan] ...` line per file (format, title, artist, album,
duration, sample rate, channels, bitrate, art type/result), then:

```
[sd] SD V2HC card, 30436 MB, SPI 25000000 Hz, product "..."
[media] SD card: 14 tracks, 14 albums, 14 artists from 14 files
[media] scan time <N> ms (UART report included), parses 14, art decodes 10,
        cached covers 14 (7 placeholders), cache 413696 B
```

and a second boot of the same card/pack prints `cache hit` with `parses 0` and
a 0-1 ms scan on the host. On the board the first scan is dominated by the
~404 KB of cover writes (about 100 flash sector erases + 1600 page programs,
several seconds); the scan report and timings include the blocking UART output,
so the pure scan is faster than the printed number.

Without a card and without a pack (or with a pack containing no parseable
audio) the app prints `[media] ...; using the demo library` and uses the
built-in tables.

Audio is still simulated: no decoder or I2S is wired. The byte-stream contract
for the audio workstream is `lib/media/media_stream.h` and
[docs/api.md](../../docs/api.md) "Audio integration".

### Making a pack

```
python scripts/gen_test_media.py            # only needed once (ffmpeg)
python scripts/pack_media.py --in test_media --out test_songs.bin \
       --uf2 test_songs.uf2
```

`pack_media.py` also accepts any folder of `.mp3/.flac/.wav/.ogg/.opus` files;
the capacity is 14 MB (`MEDIA_PACK_MAX`). Drag the UF2 onto the board, or
`picotool load -x test_songs.uf2`.

## Simulated library (fallback)

8 artists, 14 albums, 166 songs (2:30–6:00) and 3 playlists. All names,
durations and playlist membership are fictional and computed from the item id
from const tables — no dynamic allocation. The `pipod` (ILI9341) image cycles
through the six generated 120x120 covers in `lib/ui_demo_assets`; the
`pipod_st7796` image omits them for size and uses the UI's placeholder cover.

## Layout

- `main.c` — display init, SD/pack scan, UI on core 1, input polling, reporting
- `pipod_pins.h` — resolves the board pin map selected by `PIPOD_PINS_HEADER`
- `sim_library.c` — fallback `ui_library_t` provider (const tables)
- `sim_player.c` — library-agnostic fake engine: drains `ui_poll_command()`,
  keeps a play queue, uses `media_library_track()` (or `sim_song_info()`),
  advances in real time, reports via `ui_set_*()`
- `audio_load_sim.c` — 1 ms repeating-timer jitter meter + duty-cycled core-0
  busy work
