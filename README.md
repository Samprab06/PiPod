# pipod

ipod style ui for the purdue proton (rp2350b) with a 2.2 inch ili9341 spi screen.
menus, now playing, volume and so on. audio is faked for now, the real decoder is
a different workstream.

## wiring

| screen pin | proton |
|---|---|
| VCC | 3V3 |
| GND | GND |
| CS | GP33 |
| RESET | GP37 |
| DC | GP36 |
| SDI / MOSI | GP35 |
| SCK | GP34 |
| LED | GP38 |
| SDO / MISO | GP32 (optional, only needed for readback) |

gp0 and gp1 go to the debugger RX/TX, that's where the serial comes out.

## flashing

hold bootsel, plug in usb, drag dist/display_test.uf2 onto the rp2350 drive
first. you should get colour bars, a checkerboard and a bouncing square, if not
check the wiring. then drag dist/pipod.uf2 the same way.

serial is on the debug probe com port at 115200. usb cdc is off so the usb port
itself won't show anything.

## keys

w/s scroll
e select
m back
space play pause
n/p next prev
h home
P perf report

## building

needs arm gcc 14.2, pico sdk 2.3.1 and picotool. put them anywhere and point
cmake at them.

    cmake -S . -B build -G Ninja \
      -DPICO_SDK_PATH=D:/362Project/tools/pico-sdk \
      -DPICO_TOOLCHAIN_PATH=D:/362Project/tools/arm-gnu \
      -Dpicotool_DIR=D:/362Project/tools/picotool/picotool \
      -DPICO_PLATFORM=rp2350-arm-s \
      -DPICO_BOARD=proton

    cmake --build build --target display_test pipod

the uf2s land in build/apps/display_test and build/apps/pipod.

## folders

lib/display is the ili9341 driver, lib/gfx is drawing and fonts, lib/ui is the
screens, lib/input is buttons and the uart keyboard.

apps/display_test is the bring up test, apps/pipod is the real ui. scripts has
the font and image generators, docs has the notes.

more detail in docs/wiring.md, docs/api.md and docs/performance.md.

font is inter, the ofl license is in assets/fonts.
