PiPod firmware (Purdue Proton, RP2350B) — build artifacts
=========================================================

pipod.uf2        PiPod music-player UI: real lib/ui + gfx, fake library and
                 fake audio engine (no SD card or I2S yet).
display_test.uf2  ILI9341 driver bring-up: self-test, colour bars, SPI clock
                 benchmarks, 30 FPS bouncing square.

Flash: hold BOOTSEL, plug in USB, drag one .uf2 onto the RP2350 drive.
Serial: debug-probe UART0, 115200 8N1 (GP0 TX / GP1 RX); USB CDC is disabled.
Keys: w/s = wheel, e = SELECT, m = MENU, space = play/pause, n/p = next/prev,
      h = home, P = performance report.
