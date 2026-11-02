# Layout handoff notes

Target: OSH Park 4-layer, hand assembly. Their published rules are 5 mil trace and space, 10 mil (0.254 mm) minimum drill, 4 mil annular ring, no blind or buried vias. Set these in Board Setup at the start, as OSH Park asks.

Suggested stackup (not yet approved): L1 components and signals, L2 ground, L3 power and secondary routing, L4 signals. Check OSH Park's fixed stackup before calculating USB impedance.

**Mechanical dependencies, not decided here:** wheel diameter, overlay thickness, battery size and position, enclosure and mounting holes, button arrangement, and display position.

## Hand assembly

- The leadless packages need solder paste and reflow or hot air, not an iron alone: U1 (0.4 mm QFN-80), U9 (0.5 mm QFN-32), U4 (QFN-16), U6 (VSON-10), U3 (WSON-6). Plan for a stencil. Inspect for shorts on U1 under magnification.
- Do not use thermal vias under exposed pads unless their drills are at least 0.254 mm. The chosen footprints have no via pattern, so add them on the board. Vias in pads can wick solder during reflow.
- Keep enough space around 0603 parts for rework. Test the regulator and power rails before fitting U1 where you can.

## MCU support (sheet 01)

- **Regulator:** follow the Raspberry Pi layout for C1 (VREG_VIN), C2, L1, C3 and R3 as closely as you can. The guide warns that other layouts are at your own risk and uses 0402 parts here. This board uses 0603 at the team's request, so the loop will be larger. Keep it as tight as possible and verify +1V1 under load.
- **L1:** the polarity dot and direction matter. Copy the guide's orientation. Don't swap the inductor for a similar one.
- **Decoupling:** place each 100 nF right beside its power pin, on the back side under the chip if needed.
- **Crystal:** keep Y1, C5, C6 and R4 tight against XIN and XOUT, with no other signals underneath.
- **QSPI pins 70-74:** leave unconnected. Don't route anything to them. Keep the BOOTSEL trace from pin 75 short.
- **Exposed pad:** connect to ground with enough vias for the 3.4 mm pad.

## Power (sheets 02-04)

- **U6 buck-boost:** follow the TPS63001 layout example. Keep C42, the L2 loop, and C43/C44 tight. Connect GND and PGND at one point as the datasheet shows. L2 uses a generic 1210 footprint; check it against the Murata drawing.
- **U4 charger:** connect its thermal pad to the ground plane; it's the main heat path. Keep C30, C31, C32 close. Keep R21 (ISET) short and away from switching nodes.
- **U5 and U3:** short, wide power paths. These nets carry up to about 1.2 A. Keep R20, R25, R26 accessible for rework.
- **Width guide:** VBUS_RAW, USB_5V, CHG_OUT, BATT_SYS, VBAT and VSYS need traces or pours rated for at least 1.5 A.

## USB (sheet 02)

- Route D+/D- as a pair with solid ground on L2 beneath. Place R1 and R2 near the MCU and U2 near J1.
- J1 uses plated slots of 0.6 mm. OSH Park's 4-layer page lists a 20 mil drilled-slot minimum, and a forum post from OSH Park says plated slots aren't officially supported but can be made. Confirm with OSH Park before ordering.
- J1 goes on a board edge.

## Audio (sheet 07)

- Place U9 and its capacitors away from U6, L2, SPI and USB.
- Keep the headphone traces short and away from digital. Connect the jack's ground (pin 4) directly with a short path back to U9's HPVSS and ground pad.
- The DAC uses a single ground net, as on Adafruit's board. Protect it through placement, not a split plane.
- J5 on a board edge. The jack's nose overhangs per the Same Sky drawing, so check the footprint's cutout note. The footprint needs a human check against the drawing before ordering.
- C81 and C82 are 1210 MLCCs, which can be microphonic. Don't place them under the motor or buttons.

## Capacitive wheel (sheet 08)

- **Recommended:** 12 electrodes in a ring, `WHEEL_E0` to `WHEEL_E11` in order around the circle. Firmware interpolates between neighbors, so ordering matters.
- Replace placeholders E1-E12 with your copper shapes. No mask openings needed under an overlay.
- Place J9 and J7 as close to the wheel as possible. Traces must be short and similar in length. NXP guidance limits how much parasitic capacitance each channel can tolerate; the module's self-calibration has to absorb it.
- Sockets must be 15.24 mm apart, with J9 pin 1 at x = 1.27 mm and J7 pin 1 at x = 8.89 mm from the module's left edge, per Adafruit's board file. Check the mating orientation with a real module.
- Don't route SPI, USB or switching nodes in or near the wheel. Avoid solid ground directly under the electrodes; use a hatched pour around them if needed, and tune this with your overlay.
- Keep the motor and its wires away from the wheel.

## Display (sheet 05)

- Keep DISP_SCK and DISP_MOSI short, since they run at 75 MHz. Place R50 and R51 near U1. Keep a solid return under them.
- J3 position depends on the module. **Pin order is provisional**, so confirm it with the real module before ordering.

## microSD (sheet 06)

- Place U7 and U8 at the socket. Connect the shield pads to ground. Keep SD_SCK short. Follow the socket's keep-out in the footprint.

## Haptics (sheet 09)

- Keep the MOTOR_P and MOTOR_N traces wide enough for about 200 mA and run them as a pair. Place C90 and C91 at U11.

## Connectors and test points

- J1 and J5 go on board edges. SW3 goes where the enclosure can reach it. J2 needs room for the battery lead.
- Keep J14 and the test points reachable with the display and battery fitted, where you can. Test points are 1.5 mm pads.
- J11 connects to the MPR121 module with a short Qwiic cable, so keep it near J7.
