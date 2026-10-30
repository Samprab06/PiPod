# Wiring: display and controls

Physical wiring for the display/UI subsystem on the Purdue Proton. Every GPIO
below matches `lib/display/ili9341_config.h` (the single source of truth) and
`lib/input/input_buttons.c`. Nothing here is optional decoration: if a pin
moves, it moves in those files first.

Where a statement depends on the exact ILI9341 module you bought, it is marked
**module-dependent** — check your board before powering up.

## 1. Hardware in one paragraph

- **Board:** Purdue Proton, RP2350B (QFN-80), 48 GPIOs, 520 KB SRAM, **no
  PSRAM**, 16 MB QSPI flash, 12 MHz crystal, `clk_sys` 150 MHz
  (`boards/proton.h`).
- **Panel:** 2.2" ILI9341 SPI TFT, 240x320 native, driven in **landscape
  320x240**, 4-wire SPI, RGB565, one DMA channel.
- **Module:** the ubiquitous red-PCB 2.2" ILI9341 breakout with the 2x5 header
  `VCC, GND, CS, RESET, DC/RS, SDI(MOSI), SCK, LED, SDO(MISO)` plus, on many
  variants, `T_*` touch and `SD_*` microSD pins. **We use none of the touch or
  SD pins**; leave them unconnected.

## 2. Display wiring

| Module pin | Proton pin | RP2350 function | Notes |
|---|---|---|---|
| `VCC` | `3V3` | — | See the VCC/J1 note below. **Logic is 3.3 V only.** |
| `GND` | `GND` | — | Common ground with the Proton; tie it next to the SPI wires. |
| `CS` | `GP33` | SPI0 CSn / SIO | Firmware drives it as plain SIO and holds it low for a whole command+data transaction, so SPI0's hardware CS is not used. |
| `RESET` | `GP37` | SIO | Active low. `ili9341_init()` pulses it 5 ms high / 20 ms low / 120 ms high. |
| `DC` (`RS`) | `GP36` | SIO | 0 = command, 1 = data. Toggled mid-transaction; do not let anything else drive it. |
| `SDI` (`MOSI`) | `GP35` | SPI0 TX | Pixel data is 16-bit SPI frames, MSB first. |
| `SCK` | `GP34` | SPI0 SCK | SPI mode 0, MSB first. 37.5 MHz by default. |
| `LED` (backlight) | `GP38` | PWM11 A | 8-bit PWM at 150 MHz / (29 * 256) ~= **20.2 kHz** (`lib/display/ili9341.c:197-207`), starts at 100 %. See the LED note below. |
| `SDO` (`MISO`) | `GP32` | SPI0 RX | **Optional.** Only needed for ID/register readback and the self-test; reads run at 6 MHz or less. Leave unconnected if it causes trouble — the panel still works write-only. |
| `T_*` (touch) | — | — | Not used. |
| `SD_*` (card) | — | — | Not used. |

### VCC and the J1 jumper (**module-dependent**)

Most red 2.2" ILI9341 modules carry a small 3.3 V regulator and a `J1` solder
jumper that selects how `VCC` is fed. There is no single standard:

- If `J1` is **bridged**, `VCC` feeds the panel's 3.3 V rail directly. Supply
  **3.3 V** from the Proton and do not exceed it.
- If `J1` is **open**, `VCC` is the regulator input, so it can take **5 V**
  (e.g. from `VBUS`), and the regulator makes the panel rail.

Check your module's silkscreen/marking and, if unsure, buzz `VCC` to the
regulator output with a meter before applying power. The safe default for this
project is **`VCC` -> `3V3` with `J1` bridged**. What is *not* optional: the
signal pins (`CS`, `RESET`, `DC`, `SDI`, `SCK`, `LED`, `SDO`) are 3.3 V logic —
never drive them from 5 V, and never feed 5 V into `VCC` with `J1` bridged.

### Backlight LED pin (**module-dependent**)

`GP38` drives the `LED` pin through PWM. Typical red modules put an NPN
transistor (or small MOSFET) between `LED` and the backlight, so the GPIO only
sees base/gate current and direct drive is fine.

Not every module does. If yours connects `LED` straight to the backlight
anode/strings, the pin can draw far more than a GPIO can source (absolute max
per pin is in the RP2350 datasheet, but treat a few mA as the design limit).
Measure the current, or add an NPN/MOSFET low-side switch and drive its
base/gate from `GP38`. Symptom of a missing driver: dim backlight, a hot GPIO,
or the rail sagging when brightness is raised.

## 3. Breadboard buttons

All buttons are **active high**: the switch connects the GPIO to `3V3`, and a
resistor to `GND` holds the pin low when released. The firmware disables the
internal pulls (`lib/input/input_buttons.c:91-94`) because of **RP2350-E9**:
an input with no external pull can float and read high.

| Signal | GPIO | UI event | Pull-down | Notes |
|---|---|---|---|---|
| Wheel clockwise | `GP2` | `INPUT_WHEEL_CW` | external 10k–47k | Auto-repeats while held. |
| Wheel counter-clockwise | `GP3` | `INPUT_WHEEL_CCW` | external 10k–47k | Auto-repeats while held. |
| Play/pause | `GP4` | `UI_BTN_PLAY_PAUSE` | external 10k–47k | |
| Previous | `GP8` | `UI_BTN_PREV` | external 10k–47k | |
| Next | `GP9` | `UI_BTN_NEXT` | external 10k–47k | |
| Select | `GP21` | `UI_BTN_SELECT` | **on-board 10k fitted** | Upper on-board button. |
| Menu | `GP26` | `UI_BTN_MENU` | **on-board 10k fitted** | Lower on-board button. |

One button:

```
 3V3 ────[ momentary switch ]────┬──── GPIO  (e.g. GP2)
                                 │
                               [10k]
                                 │
                                GND
```

The resistor goes **at the GPIO node**, not after the switch, otherwise the pin
is left floating while the button is pressed. 10k–47k all work; the firmware
debounces with a 5 ms integrator and repeats the wheel at 250 ms then every
80 ms (`lib/input/input_buttons.c:14-16`).

## 4. Pin budget for the whole team

**Display owns `GP32`–`GP38`.** Buttons above are wired, board pins are fixed.
The "proposed" rows are a suggested allocation for the other subsystems and are
**not yet signed off** — confirm with the team before relying on them.

| GPIO | Function | Status |
|---|---|---|
| `GP0` / `GP1` | UART0 TX / RX to the on-board debug probe | **Board-reserved** |
| `GP2` | Wheel CW button | Used (input) |
| `GP3` | Wheel CCW button | Used (input) |
| `GP4` | Play/pause button | Used (input) |
| `GP5` | MPR121 IRQ | *Proposed, needs team sign-off* |
| `GP6` / `GP7` | MPR121 I2C1 SDA / SCL | *Proposed, needs team sign-off* |
| `GP8` | Previous button | Used (input) |
| `GP9` | Next button | Used (input) |
| `GP10`–`GP12` | I2S via PIO | *Proposed, needs team sign-off* |
| `GP13`–`GP20` | — | Free |
| `GP21` | Select button (on-board, 10k pull-down) | **Board-reserved**, used by input |
| `GP22`–`GP25` | LEDs: red, yellow, green, blue | **Board-reserved** |
| `GP26` | Menu button (on-board, 10k pull-down) | **Board-reserved**, used by input |
| `GP27` | — | Free |
| `GP28`–`GP31` | microSD on SPI1: RX, CSn, SCK, TX | *Proposed, needs team sign-off* |
| `GP32` | Display SDO / MISO (SPI0 RX) | **Used by display** |
| `GP33` | Display CS | **Used by display** |
| `GP34` | Display SCK (SPI0 SCK) | **Used by display** |
| `GP35` | Display SDI / MOSI (SPI0 TX) | **Used by display** |
| `GP36` | Display DC / RS | **Used by display** |
| `GP37` | Display RESET | **Used by display** |
| `GP38` | Display backlight, PWM11 A | **Used by display** |
| `GP39` | — | Free |
| `GP40`–`GP47` | — | Free; these are the RP2350B **ADC0–ADC7** pins, so analogue work belongs here |

Rules that follow from the above:

- Do not borrow `GP32`–`GP38`; the display driver reconfigures them at init.
- `GP0`/`GP1` carry the debug-probe UART. `display_test` has USB stdio off, so
  the console only exists there.
- Any new **input** pin needs an external pull-down (RP2350-E9); the firmware
  will not enable an internal one.

## 5. Signal integrity at 37.5 MHz

37.5 MHz is `clk_sys / 4`. On a breadboard that is fast enough to notice
wiring, but it is not exotic:

- Keep `SCK` and `MOSI` leads **under ~10 cm**, and keep them away from the
  backlight and button wiring.
- Run a ground return **next to `SCK`** (adjacent breadboard row or a twisted
  pair). A single ground at the far end of the header is the usual cause of
  speckled pixels.
- Use short, direct jumpers for `CS`, `DC`, `RESET` too; glitches there look
  like random commands.
- A **33 Ω series resistor on `SCK`** (at the Proton end) is a cheap damper if
  you see ringing or occasional corrupt pixels.
- If garbage pixels appear, lower the clock. Either **Settings > Display
  Clock** in `pipod` (18.75 / 25 / 37.5 / 75 MHz), or build with
  `-DILI9341_SPI_HZ=<hz>`, or edit
  `lib/display/ili9341_config.h:43-45`. `display_test` also sweeps 15–75 MHz
  and prints the actual clock.
- **75 MHz is an experimental overclock.** The ILI9341 datasheet rates the
  write clock at 10 MHz and 37.5 MHz is a common overclock; 75 MHz may work on
  one module and corrupt on the next. `display_test` redraws the colour bars
  after the 75 MHz run and asks you to check them for speckles or shifted bars.
- Register reads always drop to 6 MHz or less automatically; that is not a
  tuning knob.
- Never hot-plug the display while powered.

## 6. Bring-up checklist

1. **Wire it** per section 2 with the power off, then check `3V3`–`GND` with a
   meter before plugging USB in.
2. **Flash `display_test.uf2`.** Hold `BOOTSEL`, plug USB, and copy the UF2 to
   the `RP2` mass-storage drive; or use the debug probe (`picotool load -x`).
   The app routes stdio to **UART0 only** (`GP0`/`GP1`,
   `apps/display_test/CMakeLists.txt:8-10`), so open the debug probe's serial
   port at **115200 8N1**. USB CDC will show nothing — that is expected.
3. **Expect this banner:** `=== ILI9341 display_test (Purdue Proton) ===` and
   `INFO  clk_sys=150000000 Hz`.
4. **Read the self-test block** (`apps/display_test/main.c:266-317`):

   | Line | PASS means | If it does not pass |
   |---|---|---|
   | `RDDID` | `0x009341`, a genuine ILI9341 | `0x000000`/`0xFFFFFF` = unprogrammed ID **or no SDO** — informational only; any other ID is a clone with a custom ID. |
   | `MISO/SDO readback` | at least one register read returned real data | `FAIL` is acceptable when `SDO` is not wired; the panel still works write-only and all draw tests stay valid. |
   | `RDMADCTL (0x0B)` | echoes the programmed landscape MADCTL (`0x28`) | Check `SDO`, then `MOSI`/`SCK`. |
   | `RDDCOLMOD (0x0C)` | `0x55` or `0x05` (16 bpp DBI) | Panel did not accept the init sequence. |
   | `RDDPM (0x0A)` | sleep-out and display-on bits set | `RESET`/`DC`/`CS` wiring. |
   | `self-test` | MADCTL, COLMOD and display-on all OK | See the lines above it. |
   | `init` | panel up, 1 DMA channel claimed, backlight 100 % | A `false` here with a good picture just means readback is unavailable. |

5. **Watch the picture:** solid fills R/G/B/W/K, 8 colour bars
   (W,Y,C,G,M,R,B,K), then the orientation frame with **TL red, TR green,
   BL blue, BR yellow** (`apps/display_test/main.c:104-110`). The markers must
   sit exactly in the corners; that also proves window addressing. Then a
   checkerboard, the SPI benchmarks, and a 30 FPS bouncing square.
6. **Triage** if something is off:

   | Symptom | Cause and fix |
   |---|---|
   | Image mirrored or upside down | MADCTL. Use `ili9341_set_rotation()` with the flipped landscape mode; the four MADCTL values are at `lib/display/ili9341.c:53-57`. |
   | Image rotated 90° (portrait) | Same place — the panel is being left in portrait MADCTL. |
   | Red and blue swapped | The panel wants RGB order. Clear `ILI9341_MADCTL_BGR` (`0x08`) from the MADCTL values in `lib/display/ili9341.c:53-57`. |
   | Screen stays white/blank | Backlight is on but no pixels arrived: check `RESET` (`GP37`), `CS` (`GP33`), `DC` (`GP36`) first, then `SCK`/`MOSI`. White with a valid init usually means `DC` is stuck or `CS` is not being asserted. |
   | Backlight off | `LED`/`GP38`. Confirm the pin pulses, then check the module's transistor (see section 2). |
   | Speckles, shifted image, random colours | Clock too high for the wiring: drop to 18.75 or 25 MHz via Settings > Display Clock or `ILI9341_SPI_HZ`, shorten `SCK`/`MOSI`, add the 33 Ω resistor. |
   | Self-test `FAIL`, picture perfect | `SDO` not wired. Expected; ignore. |
   | No serial output at all | Wrong port or baud, or you are watching USB CDC. Use the debug probe UART at 115200. |
