# Schematic verification report

Date: 2026-11-01. Tool: KiCad 10.0.8 command-line tools. Revision: A, draft.

**This schematic is not cleared for fabrication.** No hardware has been built. Nothing below is a bench result, and a person still needs to review the schematic in the KiCad app.

## Automated results

| Check | Command or method | Result |
|---|---|---|
| ERC | `kicad-cli sch erc --severity-all hardware/PiPod.kicad_sch` | 0 violations across the root and 11 sheets |
| Netlist | `kicad-cli sch export netlist`, compared pin by pin with the design definition | 652 pins, 164 nets, 0 missing, 0 extra |
| Footprints | Each symbol pin compared with its footprint's pad names | 218 parts, 0 missing footprints, 0 mismatches |
| Drills | Scan of library footprints in the design | None under OSH Park's 0.254 mm minimum |
| 0603 minimum | BOM package list | No 0402 or smaller passive |
| GPIO pins | Generator cross-checks U1 against `pin_map.md`; datasheet pin list compared by hand | Consistent; see note below |

These checks show the files agree with each other. They don't show that the circuit works.

**GPIO note:** the datasheet's text-extracted pin list was read by column. GP0-37 match its QFN-80 column in order, and GP6, GP13, GP40 and GP47 were read from its pinout diagram. GP38-39 and GP41-46 come from KiCad's symbol plus a partial parse of the diagram, so check them when reviewing U1.

**ERC exceptions:** none suppressed. Power flags are placed on rails fed only through passives (GND, VBUS_RAW, CHG_IN, BATT_SYS, VINA, +1V1, VREG_AVDD, SD_3V3, 3V3_A, HAPTIC_VDD). Unused pins carry no-connect flags with reasons in the sheet notes.

## Datasheet review

Every pin and value on U1, U3, U4, U5, U6, U9 and U11 was compared with its datasheet by a separate review pass, then checked by the lead.

| Finding | Outcome |
|---|---|
| Reviewer: R23 sets a mode the strapping never selects | Intentional. An open ILIM disables charging, per the datasheet. Note clarified |
| Reviewer: GPIO numbers disagree with pin numbers | Rejected. GPIO and package pin numbers differ on this package |
| TLV320DAC3100 power-up note incomplete | Note fixed. All 3.3 V pins share a rail. TI calls the part tolerant of sequencing, and R77 holds RESET low as required |
| 100 uF headphone capacitor attributed to the datasheet | Note fixed. The datasheet requires AC coupling; the value comes from Adafruit's board |
| C22 22 uF on U3 | The datasheet's figure shows this value, but its text doesn't state it clearly. Confirm on the PDF |

No net changed as a result of this review.

## Brief section 21: mandatory checks

| Item | Status | Evidence |
|---|---|---|
| No unconnected required MCU power pins | Checked | Netlist; all IOVDD, DVDD and supply pins assigned |
| No missing core regulator parts | Checked | L1, C1-C3, R3 per Raspberry Pi guide, at 0603 |
| No incorrect voltage domains | Partly | All logic is 3.3 V; module logic levels are from vendor pages |
| No I2C address conflicts | Checked | 0x18, 0x1A (backup only), 0x5A, 0x5B |
| No invalid SPI pins | Checked | Datasheet function table: SPI0 GP32-35, SPI1 GP28-31 |
| No conflicting chip selects | Checked | One device per SPI bus |
| No missing codec clocks | Partly | DAC PLL from BCLK per Adafruit's documentation; register setup and jitter untested |
| No incorrect headphone wiring | Checked | AC coupled; jack pins from Same Sky table; CTIA wiring assumed for Apple earphones |
| No incorrect microSD pin numbering | Checked | KiCad's Hirose symbol and footprint; detect behavior from Hirose datasheet |
| No USB-C CC mistakes | Checked | 5.1k on each CC |
| No 5 V on 3.3 V-only inputs | Partly | Sense divider scales VBUS for GP43; reviewed by reading only |
| No backfeed paths | Partly | U5 blocks reverse current; small currents through sense dividers into an unpowered MCU are estimated, not measured |
| No improper charger and battery connections | Checked | Battery only on BAT; R26 is DNP and only fitted with U4 removed |
| No multiple active outputs | Partly | Backup audio needs the removal steps in the overview; the procedure is documented but nothing prevents a mistake |
| No incorrect Qwiic mapping | Checked | Matches Adafruit's board file; confirm the footprint's pin 1 against the JST drawing |
| No duplicate or incompatible pull-ups | Checked | One main pair; module pull-up accounted for |
| No backup connectors lost on isolation | Checked | Qwiic and J8 attach on the MCU side of the links |
| No floating config pins without reason | Checked | TMR and ITERM open per datasheet; reasons in notes |
| No unverified package substitutions | Partly | See footprint items below |
| No reliance on an unverified FPC pinout | Checked | No FPC connector used |

## Brief section 26: acceptance criteria

| Criterion | Status |
|---|---|
| RP2354B support circuitry | Met, with the 0603 deviation below |
| USB-C data and power | Met |
| Battery charging and USB fallback defined | Met; see fault limits in `design_overview.md` |
| Regulated supply for all ICs | Met |
| Display connection defined | Met, pin order provisional |
| microSD connected | Met |
| WM8960 audio and clocks | **Changed with team approval** to TLV320DAC3100; WM8960 is backup only |
| MPR121 electrode nets and addressing | Met through the Adafruit module, **a team-approved change** from an integrated controller |
| Five button inputs | Met, switches provisional |
| Haptics | Met, motor not selected |
| Qwiic and isolation | Met |
| SWD and power debugging | Met |
| All required parts have MPNs and footprints | Partly; off-board parts are not selected |
| No unexplained ERC errors | Met |
| Critical datasheets reviewed | Mostly met; USBLC6 and SMF5.0A rely on library data and listings |
| Opens in KiCad without missing libraries | Partly; loads in `kicad-cli`, **not yet opened in the app** |
| Ready for layout | After the team resolves the first group below |

## Resolve before layout

1. Confirm the display module's pin order for J3 and whether it has an LED resistor.
2. Choose the battery and check its polarity and whether it has protection.
3. Confirm the mechanical items: button arrangement, wheel size, overlay, enclosure.
4. Open the project in KiCad 10 and review all sheets.
5. Test the audio path on an Adafruit 6309 with the RP2350 before committing to the DAC, including PIO BCLK jitter.
6. Plug in an Adafruit 1982 to confirm J7 and J9 orientation.

## Check before ordering boards

| Item | Risk |
|---|---|
| Jack footprint `Jack_3.5mm_SameSky_SJ2-35954D-SMT` | Pad centers measured from a drawing image. Check against the drawing at 1:1 |
| L2 uses a generic 1210 footprint | Not compared with Murata's recommended land pattern |
| U4 uses a 1.6 mm thermal pad footprint | TI's text only shows the RGT0016C layout (1.68 mm); check against RGT0016B |
| U3, U11, U1 footprints | Library or generic; compare with TI and Raspberry Pi drawings |
| Regulator capacitors at 0603 | Raspberry Pi warns against departing from its layout, which uses 0402. Check +1V1 under load |
| Waveshare header J8 | Mapping read from the module's schematic; mating method not confirmed |
| USB slots on J1 | A forum post from OSH Park says plated slots aren't officially supported but can be made; confirm with them |
| OSH Park file support | Their docs list KiCad 9.x processing; send Gerbers from KiCad 10 |

## Assembly and sourcing

- Hand-assembling U1 (0.4 mm QFN-80) and the other leadless parts needs a stencil, reflow or hot air, and magnified inspection. Confirm your team has the equipment.
- **Buy early:** TPS25200DRVR showed only 11 at Digi-Key, TPS2116DRLR 177.
- **Hard to get:** AOTA-B201610S3R3-101-T has no Digi-Key or Mouser offer, only Arrow and Verical. The Raspberry Pi guide names this part specifically. ABM8-272-T3 shows constrained supply.
- Digi-Key links the jack's datasheet from a "discontinued" path while listing it Active. Confirm before relying on it.
- Stock figures are snapshots from 2026-11-01/02. Yageo resistors other than the 80.6k were not individually checked.
- USBLC6 and SMF5.0A datasheets weren't re-read for this review.

## Bench checks once built

- USB insertion and removal during playback: VSYS dip and any reset.
- USB current against a plain 500 mA port while charging.
- U3 and U5 behavior with the charger links removed.
- Current budget and battery life; the numbers in the overview are estimates.
- Wheel sensitivity through the module header and your overlay.

## Firmware changes this board needs

- Update pins for buttons, I2C, microSD and audio.
- New TLV320DAC3100 driver and PIO I2S output.
- The media code assumes 16 MB of flash. On 2 MB, the song pack moves to microSD and the cover cache needs a new flash location.
- Charger control on `CHG_EN1` and `CHG_CE_N`.
