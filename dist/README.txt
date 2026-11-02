PiPod firmware and test song pack (Purdue Proton, RP2350B)
==========================================================

Files
  pipod.uf2        firmware: real UI + real media-pack library, simulated audio
  test_songs.uf2   the test song pack, written at flash offset 0x100000
  test_songs.bin   the same pack as a raw image (picotool / inspection)
  display_test.uf2 ILI9341 bring-up and SPI benchmarks
  spi_overclock.uf2 SPI margin sweep
  motion_test.uf2  UI motion/animation demo

Flashing (BOOTSEL drag-and-drop, in this order)
  1. Hold BOOTSEL, plug in USB, drag pipod.uf2 onto the RP2350 drive.
  2. Drag test_songs.uf2 onto the same drive.
  The pack lands at flash offset 0x100000 (14 MB region), so it does not
  overwrite the firmware. Re-flashing pipod.uf2 later does not erase it.
  The scan cache is rebuilt automatically when the pack changes.

Make a pack from your own songs
  python scripts/pack_media.py --in <folder> --out my_songs.bin --uf2 my_songs.uf2
  Capacity is 14 MB. MP3, FLAC, WAV, Ogg Vorbis and Opus are indexed; JPEG
  covers are decoded to 120x120 album art, other formats get a placeholder.
  Drag my_songs.uf2 onto the board instead of test_songs.uf2.

UART scan report (GP0 TX / GP1 RX, 115200 8N1)
  On boot the firmware validates the pack and either loads the scan cache or
  parses every file and decodes one cover per album. The first boot after a
  pack change takes several seconds (flash erase/program of the index and
  covers); later boots detect the same pack and load the cache. It prints one
  line per file with format, title, artist, album, duration, sample rate,
  channels, bitrate and art status, then totals, scan time, parse/art-decode
  counts and cache hit or miss. The second boot of the same pack is a cache hit
  (about 1 ms on the host) with zero tag parses. Without a valid pack the app
  falls back to the built-in demo library and says so on the UART.

Audio is not played yet. Playback on the screen is simulated; the audio
workstream gets the byte-stream contract (media_source_t + lib/flashstream)
from lib/media/media_stream.h and docs/api.md "Audio integration".
