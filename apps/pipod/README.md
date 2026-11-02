# apps/pipod — PiPod UI on the real media pack

Runs the real `lib/ui` on the Purdue Proton, reads the real media library out
of the 14 MB flash media pack (`lib/media/media_library.h`), and keeps the fake
audio engine, so the screens, input and audio-isolation budget can be checked
before I2S/decoder code exists. When no pack (or no usable track) is present
the app falls back to the built-in demo tables in `sim_library.c`.

The first boot after a pack change scans it once; later boots load the cached
index. See "Media pack library" below.

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
`.hex`, `.dis`). `lib/ui`, `lib/media` and `lib/flashstream` must be present;
the app is skipped at configure time when those targets do not exist yet. The
POST_BUILD size check fails the build if `pipod` would grow past 1 MB (the
media pack starts at flash offset 0x100000).

## Flash and run

Hold BOOTSEL, plug the board in, then either drag
`build/apps/pipod/pipod.uf2` onto the `RP2350` USB drive or:

```sh
picotool load -x build/apps/pipod/pipod.elf
```

Then copy `dist/test_songs.uf2` to the same drive (it lands at flash offset
0x100000 and does not touch the firmware). The pack can be replaced at any
time the same way; the cache key changes with the pack, so the next boot
rescans.

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

## Media pack library

At boot, on core 0 and **before core 1 starts** (flash writes stall XIP, so
they must not overlap the UI):

1. `media_pack_open()` validates the PPAK pack at flash offset 0x100000.
2. If the scan cache already holds this pack's key, the index is loaded from
   the cache: no tags are parsed and no JPEG is decoded (about 1 ms).
3. Otherwise every file is parsed once, one cover per album is decoded to
   120x120 RGB565, and the index + covers are written to the 1 MB cache at
   0xF00000 and committed.
4. `media_library_ui()` is handed to `ui_init`; it serves artists, albums,
   songs ("All Songs" playlist), titles and album art. `sim_player.c` uses
   `media_library_track()` for real title/artist/album/duration/art.

The UART prints one `[scan] ...` line per file (format, title, artist, album,
duration, sample rate, channels, bitrate, art type/result), then:

```
[media] full scan: 14 tracks, 14 albums, 14 artists from 14 files
[media] scan time <N> ms (UART report included), parses 14, art decodes 10,
        cached covers 14 (7 placeholders), cache 413696 B
```

and a second boot of the same pack prints `cache hit` with `parses 0` and a
0-1 ms scan on the host. On the board the first scan is dominated by the
~404 KB of cover writes (about 100 flash sector erases + 1600 page programs,
several seconds); the scan report and timings include the blocking UART output,
so the pure scan is faster than the printed number.

Without a pack (or with a pack containing no parseable audio) the app prints
`[media] no media pack ...; using the demo library` and uses the built-in
tables (8 artists, 14 albums, 166 songs, 3 playlists).

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
from const tables — no dynamic allocation. Album art cycles through the six
generated 120x120 covers in `lib/ui_demo_assets`.

## Layout

- `main.c` — display init, media scan, UI on core 1, input polling, reporting
- `sim_library.c` — fallback `ui_library_t` provider (const tables)
- `sim_player.c` — library-agnostic fake engine: drains `ui_poll_command()`,
  keeps a play queue, uses `media_library_track()` (or `sim_song_info()`),
  advances in real time, reports via `ui_set_*()`
- `audio_load_sim.c` — 1 ms repeating-timer jitter meter + duty-cycled core-0
  busy work
