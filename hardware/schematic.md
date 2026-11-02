# Schematic work: agent handoff

Everything another agent needs to continue the PiPod schematic. Written 2026-11-01. Read this first, then `docs/verification_report.md`.

## Task and source documents

- **Goal:** a complete, reviewed KiCad schematic for an RP2354B iPod-style music player. Deadline 2026-11-04. PCB layout, firmware and enclosure are out of scope.
- **Brief:** `D:\Downloads\writing-block.md` on the user's PC, not in the repo. Sections 21 and 26 are the checklists. Section 25 has rules, including not inventing data.
- **Proton reference:** `D:\Downloads\Proton-KiCAD-Rev4.zip`. It has a Proton BOM, RP2350 footprints and the debug probe design.
- **User's instruction:** don't assume anything; quiz before major decisions.
- **Repo:** `https://github.com/Samprab06/PiPod`, local at `D:\362Project`.

## Working rules from the user

- Ask before any commit or push. After approval, a BanyanCode agent makes the commit under `Ekagra <ekagra2006@gmail.com>`, with no Claude attribution or Co-Authored-By line. The user's rule overrides any default attribution text.
- Don't open PRs unless asked.
- Delegate through BanyanCode with model `opencode-go/deepseek-v4.1-flash` (`deepseek/...` fails). Use `coder` for implementation, `scout` for read-only and `researcher` for web lookups. The lead plans and reviews. Run workers in parallel only on separate files.
- Don't poll task status; polling cost about $7 in one session. Set a long background timer, check once.
- BanyanCode can't write files when it's a read-only agent. Researchers report in their result summary.

## Decisions and who made them

All were made by the user unless marked "lead".

| Topic | Decision |
|---|---|
| Fabrication | OSH Park 4-layer, no assembly service, team hand assembles and buys parts. **This replaced an earlier MacroFab turnkey plan** |
| Part size | 0603 minimum, stated by the user. Applied to all passives; ICs use their packages |
| Quantity | 2-5 boards, budget flexible |
| KiCad | Version 10 (10.0.8 installed at `C:\Program Files\KiCad\10.0`; 9.0.7 also present). Start fresh and reuse only values that check out |
| Audio | TLV320DAC3100 on the board, Adafruit 6309 for testing, Waveshare WM8960 on a backup header |
| Headphones | 4-pole jack, plus DNP speaker pads |
| Touch | Adafruit 1982 module in empty header sockets, PCB electrodes to the sockets, I2C over Qwiic |
| Haptics | DRV2605L on the board, Qwiic backup |
| Power | USB-C to BQ24074 and separate fallback, TPS2116, buck-boost to 3.3 V |
| Switch | Slide switch. Lead chose to implement it on U6 EN, not in the power path, and reported it |
| Battery | Assume 1000 mAh protected LiPo, JST-PH, about 500 mA charge, provisional |
| Display | 9-pin socket for the 2.2-inch module, MOSFET backlight, no FPC |
| Pin map | Lead proposed, user approved. Lead later changed provisional pins GP11, GP44-47 and reported them |
| Optional parts | User gave no preference; lead treated that as none |
| If time runs short | Cut documentation depth first |

## Current state

Branch `temp`, created from local `hardware-schematic`, which branched from `main` at `8ec3b1d`. No PR. `main` doesn't have this work.

| Item | State |
|---|---|
| Schematic | Root plus 11 sheets, 218 parts, 164 nets, 652 pins |
| ERC | 0 violations with `kicad-cli sch erc --severity-all` |
| Netlist vs. design | 0 mismatches |
| Footprint pads vs. symbol pins | 218 parts, 0 mismatches |
| Reviewed in KiCad app | **Not by an agent.** The user opened it around 21:53-22:13 local; no feedback yet |
| Hardware | Nothing built |
| Fabrication | **Not cleared** |

ERC was last run from the generator's output. After that, the title block and note dates were corrected from 2026-11-02 to 2026-11-01 by direct text replacement in the `.kicad_sch` files, and ERC was re-run. KiCad also changed `PiPod.kicad_pro` and `PiPod.kicad_prl` when the user opened it at 22:13.

## How the files were made

The KiCad files were generated from Python, **which is not committed**. It lives in `D:\362Project\hw_gen\`, excluded through `.git/info/exclude`, to match the user's earlier wish that the repo hold only main project files. If you're on another machine, you don't have it: edit the KiCad files directly.

| File | Purpose |
|---|---|
| `kicadgen.py` | Writes KiCad 10 files. Reads and flattens KiCad library symbols, stub wire and label on every pin, shelf-packs parts, deterministic UUIDs. Written by BanyanCode, lightly patched by the lead |
| `design.py` | **The circuit source of truth.** Every part, net, value and note. Written by the lead |
| `check.py`, `verify.py` | ERC, netlist and footprint checks. `python verify.py` **regenerates first** |
| `gen_pinmap.py` | Writes `docs/pin_map.md` and asserts it matches U1's nets |
| `gen_bom.py` | Groups `design.py`'s per-part export into `PiPod_BOM.csv`. Must run after each regenerate; uses `_parts.csv` as a cache |
| `selftest.py` | Generator acceptance test |

**Warning:** regenerating overwrites `hardware/*.kicad_sch`, `PiPod.kicad_pro`, and the BOM. It already overwrote sheet 04 once while the user had it open. It's safe only when nobody has hand-edited the KiCad files and no `~*.lck` files exist. If someone has edited them, the KiCad files become the source of truth and `design.py` is out of date. Also note that `design.py` and the committed `.kicad_sch` should now match on dates.

The generator uses local labels for nets on one sheet and global labels for nets on several. Custom parts: `TLV320DAC3100`, `TPS25200` and `SJ2-35954D` symbols; `Jack_3.5mm_SameSky_SJ2-35954D-SMT` and `WheelElectrode_Placeholder` footprints. Everything else comes from KiCad 10's libraries.

Datasheet text used for the review was in a session scratchpad and may be gone. URLs are in `docs/design_overview.md`, and `pdftotext -layout` works on them.

## Circuit summary by sheet

**01 core (U1 RP2354B, `SC1512-A4`).** Follows Raspberry Pi's "Hardware design with RP2350". L1 3.3 uH Abracon AOTA-B201610S3R3-101-T; C1, C2, C3 4.7 uF; R3 33 Ω; 100 nF per power pin with USB_OTP_VDD and QSPI_IOVDD sharing one. Y1 ABM8-272-T3, 15 pF, R4 1 kΩ on XOUT. R1, R2 27 Ω USB. QSPI 70-74 no-connect because flash is in the package. QSPI_SS through R5 1 kΩ to BOOTSEL switch. No QSPI pull-up. RUN to reset switch. GP47 status LED.

**02 USB.** USB4105-GF-A. 5.1 kΩ CC. U2 USBLC6-2SC6. D2 SMF5.0A. C20 4.7 uF, kept low to stay under USB's 10 uF sink limit. U3 TPS25200: EN through 300 kΩ, RILIM 80.6 kΩ (1110-1300 mA), output `USB_5V` with C22 22 uF. VBUS sense 100 kΩ/47 kΩ to GP43.

**03 charger.** U4 BQ24074: OVP 10.5 V, OUT 4.4 V. R21 1.78 kΩ = 500 mA. R23 1.58 kΩ ILIM must exist because an open ILIM disables charging, even though EN2 tied low never selects that mode. EN1 from GP45: low at reset = 100 mA, firmware sets 500 mA. CE from GP46 with R24 pull-down. TMR and ITERM open (datasheet defaults). TS 10 kΩ fixed. R20 and R25 isolate the charger. R26 DNP bypass, **only with U4 removed**. J2 battery. VBAT sense 1 MΩ/1 MΩ to GP40.

**04 power.** U5 TPS2116 priority mode: VIN1 `USB_5V`, VIN2 `BATT_SYS`, MODE to VIN1, PR1 300 kΩ/100 kΩ = 4.0 V switch point. ST to GP44. U6 TPS63001 from VSYS: VINA through 100 Ω and 100 nF, FB to output, 10 uF in, 2×10 uF out, L2 2.2 uH Murata DFE322512F-2R2M=P2 (TI's named VLF4012 is obsolete). PS/SYNC to GND through R44, R45 DNP for forced PWM. SW3 grounds EN. VSYS sense to GP41.

**05 display.** J3 1×9 socket: 1 VCC, 2 GND, 3 CS, 4 RST, 5 DC, 6 MOSI, 7 SCK, 8 LED, 9 MISO, **provisional**. 22 Ω on SCK and MOSI. Q1 2N7002 drives Q2 AO3401A high-side. PWM stays active-high, matching firmware. R52 0 Ω backlight link.

**06 microSD.** DM3AT-SF-PEJM5. SPI1. 10 kΩ pull-ups on CS, CMD, DAT0-2. R66 22 Ω SCK. U7, U8 USBLC6 ESD. Detect switch closes with a card, per Hirose, so SD_CD reads low with a card.

**07 audio.** U9 TLV320DAC3100 at 0x18, RHB0032E footprint. FB1 filters `3V3_A`. U10 AP2112K-1.8 for DVDD. R77 pulls RESET low during startup. C81, C82 100 uF headphone coupling (Adafruit's value). J5 SJ2-35954D-SMT-TR: pin 2 tip L, 3 ring1 R, 4 ring2 GND, 1 sleeve mic (CTIA). R84 2.2 kΩ mic bias and R85 to VOL/MICDET, as Adafruit does. HP_DET from tip switch to GP11. J6 DNP speaker. Isolation links R70-R76. Adafruit 6309's schematic was the reference.

**08 wheel.** J9 13-pin: 1-12 = WHEEL_E11 down to E0, 13 GND. J7 7-pin: 1 VIN, 2 3V3 out, 3 GND, 4 SCL, 5 SDA, 6 INT, 7 ADDR. Read from Adafruit's `.brd`: rows 15.24 mm apart, J9 pin 1 at x = 1.27, J7 pin 1 at x = 8.89, board 33×19 mm. R90 ADDR to 3V3 out for 0x5B. R91-R93 DNP for hard-wired power and I2C. E1-E12 placeholders.

**09 buttons and haptics.** Five active-low switches with 10 kΩ pull-ups on GP22-26. U11 DRV2605L at 0x5A on 3V3 through R99. REG and VDD 1 uF. EN GP2, IN/TRIG GP3, both with 100 kΩ pull-downs. Isolation R94, R95, R97, R98, R99. J10 motor.

**10 expansion.** R110, R111 4.7 kΩ I2C pull-ups. J11, J12 Qwiic: 1 GND, 2 3V3, 3 SDA, 4 SCL. J8 2×8 mirrors the Waveshare header: 1, 2 3V3; 3, 4 GND; 5 SDA; 7 SCL; 9, 10 BCLK; 11, 12 LRCLK; 13 DAC data; 14 ADC data; 15 MCLK_RX; 16 MCLK_TX; 6, 8 NC. J13 GP12-19.

**11 debug.** J14: 1 3V3 reference, 2 SWCLK, 3 SWDIO, 4 GND, 5 TX, 6 RX. TP1-31.

## Research findings

- WM8960: Digi-Key says obsolete and no longer manufactured. Cirrus's own page was not reached.
- MPR121: NXP forum says discontinued, rights with Resurgent Semiconductor. Stock existed mainly through aggregators.
- Waveshare module: 24 MHz oscillator to MCLK; 3.3 V; 100 uF headphone coupling; TRRS jack.
- TLV320DAC3100: TI lists Active; PLL can use BCLK per Adafruit; playback only.
- RP2350 stepping A4 is the production part and the E9 pull-down problem is fixed from A3. Buttons here use external pull-ups anyway.
- ADC is on GP40-47 only on QFN-80.
- MCLK cannot come exactly from PIO on GP9. GP21 has GPOUT0.
- OSH Park 4-layer: 5 mil trace and space, 10 mil drill, 4 mil annular ring, 20 mil drilled slot minimum. Plated slots unofficial per a forum post. Their docs list KiCad 9.x processing.
- Capacitors originally picked from Proton or Murata were obsolete, NRND or out of stock; the current ones in `design.py` came from Digi-Key's substitutes. Digi-Key rate-limits automated fetches; `digikey.ca` worked when `.com` returned 403.

## Datasheet review outcome

A BanyanCode `scout` compared U1, U3, U4, U5, U6, U9 and U11 with their datasheets. Results and lead's dispositions are in `docs/verification_report.md`. It wrongly reported a GPIO numbering conflict because it confused GPIO numbers with package pins; don't repeat that. Datasheet tables extracted with `pdftotext` can be offset by a row, as with the TLV320DAC3100 table, so trust the pinout diagram.

## Open work, in priority order

1. Get user feedback from KiCad review. Fix what they report.
2. Display pin order, LED resistor. Needs a photo from the user.
3. Battery and motor selection.
4. Check footprints against drawings: J5, L2, U4, U1, U3, U11, J1, J4, connectors.
5. Confirm C22, GP38-39, GP41-46 and the USBLC6 and SMF5.0A datasheets.
6. Bench test Adafruit 6309 audio and the 1982 module on the Proton.
7. Recheck stock and buy hard-to-get parts: see `integration_guide.md`.
8. Layout (separate engineer).

Don't claim anything is verified unless a tool or datasheet read supports it, and say which. The BOM's Verification Status should never be upgraded without a reason.

## Known weak spots

- The sheets are readable but look machine-placed.
- USB current can exceed a default 500 mA port during charging; mitigated but untested.
- Regulator capacitors at 0603 depart from Raspberry Pi's layout.
- A charger IN short takes USB down until R20 is removed. A BAT short relies on the cell's protection.
- Current budget and battery life are estimates.

## Related firmware

On GitHub branch `display-ui`, not in `main`: ILI9341 driver at 75 MHz, UI, metadata parser, flash song pack, flash-to-display DMA, motion test, SPI sweep, UF2s in `dist/`. Simulator and specs (`sim/`, `docs/design.md`, `docs/ui_spec.md`, `docs/media_design.md`) stay local and excluded. Current firmware runs on the RP2350B Proton: the display uses GP32-38; breadboard buttons GP2, 3, 4, 8, 9 change on this board. The media code assumes 16 MB flash, which needs rework for the RP2354B's 2 MB.

## Repo notes

`temp` deletes `hardware/~PiPod.kicad_sch.lck` and untracks the broken `hardware/.history` link that came from `main`. `.gitignore` now excludes `hardware/.history/`, `*.lck`, `*.kicad_prl`, `fp-info-cache` and backups. `PiPod.kicad_prl` stays tracked from `main` until someone removes it.
