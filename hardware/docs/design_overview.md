# PiPod motherboard design overview

Status: schematic draft for review. Not ready for fabrication. Open items are in `verification_report.md`.

Build plan: 4-layer boards from OSH Park, hand-assembled by the team, all components 0603 or larger.

## Architecture

```
USB-C J1 ─┬─ U2 ESD ─ 27R ─ RP2354B USB
          └─ VBUS_RAW ─┬─ R20 ─ U4 BQ24074 ─ R25 ─ BATT_SYS ─ U5 VIN2 ─┐
                       │          │ BAT                                 ├─ VSYS ─ U6 TPS63001 ─ +3V3
                       │       J2 battery                               │
                       └─ U3 TPS25200 eFuse ─ USB_5V ─ U5 VIN1 ─────────┘

+3V3 ─┬─ RP2354B core (internal regulator makes +1V1)
      ├─ J3 display module, Q1/Q2 backlight switch
      ├─ J4 microSD through R60
      ├─ U11 DRV2605L through R99 ─ J10 motor
      ├─ J11, J12 Qwiic ─ Adafruit 1982 MPR121 module in J7/J9 ─ wheel electrodes
      └─ FB1 ─ 3V3_A ─ U9 TLV320DAC3100, U10 1.8 V LDO ─ J5 headphone jack
```

| Sheet | Contents |
|---|---|
| 01 RP2354B Core | MCU, regulator parts, crystal, BOOTSEL, RESET, status LED |
| 02 USB Power Entry | USB-C, ESD, CC resistors, TVS, fallback eFuse, VBUS sense |
| 03 Battery Charging | BQ24074, battery connector, isolation links, VBAT sense |
| 04 Power Selection Regulation | Mux, buck-boost, power switch, VSYS sense |
| 05 Display | Display module socket, backlight switch |
| 06 MicroSD | Socket, pull-ups, ESD |
| 07 Audio Codec | DAC, 1.8 V LDO, headphone jack, speaker option |
| 08 Capacitive Wheel | Adafruit module sockets, electrode nets |
| 09 Buttons Haptics | Five buttons, DRV2605L, motor connector |
| 10 Expansion Backup | I2C pull-ups, Qwiic, Waveshare backup header, spare GPIO |
| 11 Debug Testpoints | SWD and UART header, test points |

## Changes from the brief, all approved by the team

| Brief | This design | Reason |
|---|---|---|
| WM8960 codec | TLV320DAC3100, Waveshare WM8960 on a backup header | Digi-Key lists the WM8960 as obsolete and no longer manufactured |
| Integrated MPR121 | Adafruit 1982 module in sockets, I2C over Qwiic | NXP has discontinued the MPR121; team decision |
| GPIO table | Display GP32-38, microSD GP28-31 | Matches current firmware, frees HSTX |
| Assembly | Hand assembly, 0603 minimum | Team decision |

The DAC is playback only. It has no I2S recording path, which the brief does not need.

## Power rails

| Rail | Source | Nominal | Range | Loads |
|---|---|---|---|---|
| `VBUS_RAW` | USB-C | 5 V | up to the 20 V U3 rating in a fault; the TVS clamps transients | U3, U4 through R20, sense divider |
| `USB_5V` | U3 | 5 V | clamped 5.25-5.55 V | U5 VIN1 |
| `CHG_IN` | R20 | 5 V | operates 4.35-10.5 V | U4 |
| `CHG_OUT` | U4 OUT | 4.4 V on USB, VBAT on battery | | R25 |
| `VBAT` | Cell | 3.7 V | about 3.0-4.2 V, set by the cell's protection | U4 BAT |
| `BATT_SYS` | R25 | as above | | U5 VIN2 |
| `VSYS` | U5 | 3.0-5.5 V | | U6, sense divider |
| `+3V3` | U6 | 3.3 V | | Everything digital |
| `+1V1` | RP2354B internal regulator | 1.1 V | | DVDD only |
| `VREG_AVDD` | 33 ohm from `+3V3` | 3.3 V | | Regulator analog |
| `3V3_A` | FB1 from `+3V3` | 3.3 V | | DAC analog, headphone, I/O, speaker |
| `1V8_A` | U10 | 1.8 V | | DAC DVDD |
| `SD_3V3` | R60 | 3.3 V | | microSD |
| `HAPTIC_VDD` | R99 | 3.3 V | | DRV2605L |

## Current budget

These are design allowances, not measurements. Replace them with bench numbers after bring-up.

| Load on `+3V3` | Typical allowance | Peak allowance | Basis |
|---|---|---|---|
| RP2354B | 60 mA | 150 mA | Datasheet table 1693 shows about 15 mA for simple examples; allowance covers both cores at 150 MHz and pin drive |
| Display logic and backlight | 60 mA | 100 mA | Estimate; measure on your module |
| TLV320DAC3100 plus headphones | 30 mA | 100 mA | Datasheet: 25.62 mW playback, 30 mW per channel into 16 ohm |
| microSD | 40 mA | 150 mA | Estimate for a typical card during reads and writes |
| DRV2605L and motor | 0 mA | 100 mA | Depends on the motor; bursts only |
| Adafruit MPR121 module | 1 mA | 1 mA | Estimate |
| LED, pull-ups, misc | 3 mA | 5 mA | Calculation |
| **Total** | **about 195 mA** | **about 605 mA** | |

U6 is rated for 1200 mA in buck mode and up to 800 mA in boost mode at 3.3 V. Peak leaves about 25% in boost, and that peak assumes everything draws its maximum at once.

Estimated battery life: 195 mA × 3.3 V / 85% efficiency is about 0.76 W, so a 3.7 Wh (1000 mAh) cell lasts about 4.9 hours. The 85% is an assumption, so treat this as a rough estimate.

## Charger settings

| Setting | Value | Calculation |
|---|---|---|
| Fast charge | 500 mA | R21 = 890 AΩ / 0.5 A = 1.78 kΩ |
| Input limit | 100 mA at reset, 500 mA when firmware sets `CHG_EN1` high | EN2 tied low, datasheet table 7-2 |
| ILIM resistor | 1.58 kΩ, about 1 A | Unused in this strapping but required, because an open ILIM disables charging |
| Termination | 10% default | ITERM open, allowed by the datasheet |
| Safety timers | Default | TMR open, allowed by the datasheet |
| Battery temperature | Disabled with 10 kΩ on TS | The assumed cell has no thermistor |

## USB and fallback behavior

With USB connected, the system runs through U3 and U5 VIN1 and does not depend on the charger. On battery, it runs through U4, R25 and U5 VIN2. U5 selects USB when `USB_5V` is above about 4.0 V (3.68-4.32 V with reference tolerance).

| Scenario | Result |
|---|---|
| USB absent, healthy battery | Runs from battery through U4 and U5 VIN2 |
| USB present, healthy battery | Runs from USB through U3; U4 charges independently |
| USB present, battery disconnected | Runs from USB through U3 |
| USB present, battery depleted | Runs from USB immediately; U4 pre-charges the cell |
| Charging disabled, USB connected | Runs from USB; `CHG_CE_N` high disables charging |
| Charger output failed open | Runs from USB; no battery operation when USB is removed |
| Charger physically removed | Remove U4, R20 and R25. USB still runs the system. Fitting R26 restores battery power without charging |
| Charger OUT shorted to ground | With USB present, U5 stays on VIN1 and blocks reverse current, so the system keeps running. **Without USB there is no power.** Remove R25 to isolate it |
| Charger IN shorted to ground | **Not survived automatically.** It pulls down `VBUS_RAW` through R20, so USB power fails until R20 is removed |
| Charger BAT shorted to ground | **Not protected by this board.** It relies on the cell's own protection circuit. Remove the battery |
| USB inserted during playback | U5 switches break-before-make, 8 us typical; VSYS capacitors hold up the regulator |
| USB removed during playback | VSYS falls until it reaches BATT_SYS, then U5 connects the battery. Roughly 40-135 us at 0.8 A with 54 uF, calculated from a 5 V start. Needs a bench check |

U3 is not a protection against a charger fault unless the fault is on its own output path. No part here is a safety-rated isolation device.

**USB current:** U3 limits at 1110-1300 mA. System peak plus 500 mA charging can reach about 1 A, which is above a default 500 mA USB 2.0 port. The board can't read the port's advertised current because CC is not wired to the MCU. Mitigations: the charger starts at 100 mA, firmware picks 500 mA, and BQ24074 VIN-DPM reduces charging if VBUS sags. Check this on the bench with a plain USB 2.0 port.

**Low battery:** firmware reads `VBAT_SENSE`, warns, then shuts down cleanly (suggested 3.3 V warning and 3.1 V shutdown; tune against the real cell). U6 keeps regulating until VINA drops to its 1.5-1.8 V lockout, so the cell's protection circuit normally acts first.

**Off state:** SW3 grounds U6 EN, which turns off `+3V3` and every load on it. U4 and U5 stay powered and the sense dividers still draw from `VBAT` and `VSYS`, about 2 uA each, so storage drain stays in the low microamp range. Unplug the battery for long-term storage. Charging continues with SW3 off.

## Audio clock plan

The RP2354B is the I2S master, using PIO on GP6-8. The DAC needs no MCLK: its PLL uses BCLK as its input, as Adafruit documents for the same chip on its 6309 board.

At 44.1 kHz with 16-bit stereo, BCLK is 32 × 44.1 kHz = 1.4112 MHz. Firmware must set the PLL and divider registers for this rate (datasheet section 6.3.11).

The PIO divider is fractional, so BCLK has some jitter from a 150 MHz system clock. The DAC PLL filters some of that, but how audible it is hasn't been measured. Test it on the Adafruit 6309 before the PCB is ordered. If it's audible, either choose a system clock that divides exactly or fit R80 to supply MCLK from GP21.

**Waveshare backup module:** its 24 MHz oscillator drives the WM8960's MCLK, so no MCLK comes from the RP2354B. Its PLL has to make the 44.1 kHz clocks from 24 MHz, and the codec can be the I2S master. PIO then takes BCLK and LRCLK as inputs. The firmware needs a separate driver for this path.

## I2C

Bus: I2C0 on GP4/GP5 with 4.7 kΩ pull-ups on the MCU side.

| Device | Address | Where |
|---|---|---|
| TLV320DAC3100 | 0x18 | U9, datasheet "001 1000" |
| WM8960 on Waveshare backup | 0x1A | J8; isolate U9 first |
| DRV2605L | 0x5A | U11 |
| MPR121 module | 0x5B | R90 ties ADDR to the module's 3.3 V output; default would clash at 0x5A |
| Qwiic expansion | Anything else | J12. A LIS3DH would need 0x19 to avoid the DAC |

The module's 10 kΩ pull-up in parallel with 4.7 kΩ is about 3.2 kΩ. That is suitable up to 400 kHz with roughly 100 pF, by calculation. Check the rise time if you add more breakouts, because each adds pull-ups.

## Backup and isolation

| Function | To disconnect the onboard part | Replacement |
|---|---|---|
| Audio DAC | Remove R70, R71, R72, R73, R74, R75, R76 | Waveshare WM8960 on J8 |
| Haptics | Remove R94, R95, R97, R98, R99 | DRV2605L breakout on Qwiic (motor moves to it) |
| Touch | Unplug the module from J7 and J9 | Another compatible module, or fit R91-R93 to wire power and I2C through the sockets |
| Charger | Remove R20, R25; fit R26 only with U4 removed | Battery runs without charging |
| microSD | Remove R60 to unpower the card | SPI test points |

Zero-ohm links are for removal only. They don't limit current or protect against faults. With R75 removed, also check for current into an unpowered U9 from any signals still connected, which is why R70-R74 and R76 come off together.

## Files and tools

- Open `hardware/PiPod.kicad_pro` in KiCad 10. Custom parts live in `hardware/lib/` with relative paths.
- OSH Park says it processes uploaded `.kicad_pcb` files with its latest stable KiCad, currently listed as 9.x. A KiCad 10 board may not upload directly, so export Gerbers instead.

## References

- [RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
- [Hardware design with RP2350](https://datasheets.raspberrypi.com/rp2350/hardware-design-with-rp2350.pdf)
- [TLV320DAC3100](https://www.ti.com/lit/ds/symlink/tlv320dac3100.pdf)
- [BQ24074](https://www.ti.com/lit/ds/symlink/bq24074.pdf)
- [TPS2116](https://www.ti.com/lit/ds/symlink/tps2116.pdf)
- [TPS25200](https://www.ti.com/lit/ds/symlink/tps25200.pdf)
- [TPS63001](https://www.ti.com/lit/ds/symlink/tps63001.pdf)
- [DRV2605L](https://www.ti.com/lit/ds/symlink/drv2605l.pdf)
- [SJ2-3595X-SMT jack](https://www.sameskydevices.com/product/resource/sj2-3595x-smt.pdf)
- [USB4105 drawing](https://gct.co/files/drawings/usb4105.pdf)
- [Adafruit MPR121 board files](https://github.com/adafruit/Adafruit-MPR121-PCB)
- [Adafruit TLV320DAC3100 board files](https://github.com/adafruit/Adafruit-TLV320DAC3100-I2S-DAC-PCB)
- [Waveshare WM8960 schematic](https://files.waveshare.com/upload/b/b6/WM8960_Audio_Board_Schematic..pdf)
- [OSH Park 4-layer service](https://docs.oshpark.com/services/four-layer)
