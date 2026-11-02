#!/usr/bin/env python3
"""Writes hardware/docs/pin_map.md and checks it against U1's pins in design.py."""
import os, re
HERE = os.path.dirname(os.path.abspath(__file__))
rows = [
 (0,77,"UART0_TX","UART0","UART0 TX (F2)","Out","Fixed"),(1,78,"UART0_RX","UART0","UART0 RX (F2)","In","Fixed"),
 (2,79,"HAPTIC_EN","Haptics","SIO","Out","Provisional"),(3,80,"HAPTIC_TRIG","Haptics","SIO","Out","Provisional"),
 (4,1,"I2C0_SDA","I2C0","I2C0 SDA (F3)","Bidir","Fixed"),(5,2,"I2C0_SCL","I2C0","I2C0 SCL (F3)","Bidir","Fixed"),
 (6,3,"I2S_BCLK","Audio","PIO","Out (in with Waveshare master)","Fixed"),(7,4,"I2S_LRCLK","Audio","PIO","Out (in with Waveshare master)","Fixed"),
 (8,6,"I2S_DOUT","Audio","PIO","Out","Fixed"),(9,7,"I2S_DIN","Audio backup","PIO, backup module only","In","Provisional"),
 (10,8,"DAC_RST_N","Audio","SIO","Out","Provisional"),(11,9,"HP_DET","Audio","SIO, jack tip switch","In","Provisional"),
]
for g,p in zip(range(12,20),(11,12,13,14,16,17,18,19)):
    rows.append((g,p,f"EXP_GP{g}","Expansion","Spare, HSTX bank","-","Reserved"))
rows += [
 (20,20,"SD_CD","microSD","SIO","In","Provisional"),(21,21,"I2S_MCLK","Audio","CLOCK GPOUT0, optional, link DNP","Out","Provisional"),
 (22,22,"BTN_MENU","Buttons","SIO, active low","In","Fixed"),(23,23,"BTN_PLAY","Buttons","SIO, active low","In","Fixed"),
 (24,25,"BTN_PREV","Buttons","SIO, active low","In","Fixed"),(25,26,"BTN_NEXT","Buttons","SIO, active low","In","Fixed"),
 (26,27,"BTN_SELECT","Buttons","SIO, active low","In","Fixed"),(27,28,"TOUCH_IRQ","Wheel","SIO, from module INT","In","Fixed"),
 (28,36,"SD_MISO","microSD","SPI1 RX (F1)","In","Fixed"),(29,37,"SD_CS","microSD","SPI1 CSn (F1) or SIO","Out","Fixed"),
 (30,38,"SD_SCK","microSD","SPI1 SCK (F1)","Out","Fixed"),(31,39,"SD_MOSI","microSD","SPI1 TX (F1)","Out","Fixed"),
 (32,40,"DISP_MISO","Display","SPI0 RX (F1)","In","Fixed"),(33,42,"DISP_CS","Display","SPI0 CSn (F1) or SIO","Out","Fixed"),
 (34,43,"DISP_SCK","Display","SPI0 SCK (F1)","Out","Fixed"),(35,44,"DISP_MOSI","Display","SPI0 TX (F1)","Out","Fixed"),
 (36,45,"DISP_DC","Display","SIO","Out","Fixed"),(37,46,"DISP_RST","Display","SIO","Out","Fixed"),
 (38,47,"DISP_BL","Display","PWM11 A (F4)","Out","Fixed"),(39,48,"CHG_N","Power","SIO, open-drain input","In","Provisional"),
 (40,49,"VBAT_SENSE","Power","ADC0, 1:2 divider","Analog","Provisional"),(41,52,"VSYS_SENSE","Power","ADC1, 1:2 divider","Analog","Provisional"),
 (42,53,"PGOOD_N","Power","SIO, open-drain input","In","Provisional"),(43,54,"VBUS_SENSE","Power","ADC3, 100k/47k divider","Analog","Provisional"),
 (44,55,"MUX_ST","Power","SIO; low means battery path active","In","Provisional"),(45,56,"CHG_EN1","Power","SIO; low 100 mA, high 500 mA","Out","Provisional"),
 (46,57,"CHG_CE_N","Power","SIO; high disables charging","Out","Provisional"),(47,58,"STATUS_LED","Debug","SIO; also QMI CS1n if ever needed","Out","Provisional"),
]
src = open(os.path.join(HERE,"design.py"),encoding="utf8").read()
gp = dict(re.findall(r'"(\d+)": "([^"]+)"', src[src.index("gp = {"):src.index('s.part("U1"')]))
for g,p,n,*_ in rows:
    assert gp.get(str(p))==n, (g,p,n,gp.get(str(p)))
assert [r[0] for r in rows]==list(range(48))
out=["# RP2354B pin map","",
"All GPIO are in the 3.3 V IOVDD domain. Functions were read from the RP2350 datasheet GPIO function table (F1 SPI, F2 UART, F3 I2C, F4 PWM). Package pins are from KiCad's RP2354B symbol and are cross-checked against U1 in the schematic by the generator.","",
"| GPIO | Pin | Net | Peripheral | Function | Direction | Domain | Status |","|---|---|---|---|---|---|---|---|"]
for g,p,n,per,f,d,st in rows:
    out.append(f"| GP{g} | {p} | `{n}` | {per} | {f} | {d} | 3.3 V | {st} |")
out+=["","## Non-GPIO MCU pins","",
"| Pin | Name | Connection |","|---|---|---|",
"| 5, 15, 24, 29, 41, 50, 60, 76 | IOVDD | `+3V3`, 100 nF each |","| 10, 32, 51 | DVDD | `+1V1`, 100 nF each |",
"| 59 | ADC_AVDD | `+3V3`, 100 nF |","| 61 | VREG_AVDD | `+3V3` through 33 ohm, 4.7 uF |","| 62 | VREG_PGND | `GND` |",
"| 63 | VREG_LX | 3.3 uH to `+1V1` |","| 64 | VREG_VIN | `+3V3`, 4.7 uF |","| 65 | VREG_FB | `+1V1` |",
"| 66, 67 | USB_DM, USB_DP | 27 ohm series, then USBLC6 and J1 |","| 68, 69 | USB_OTP_VDD, QSPI_IOVDD | `+3V3`, one shared 100 nF per the Raspberry Pi guide |",
"| 70-74 | QSPI_SD3, SCLK, SD0, SD2, SD1 | No connect; in-package flash uses these |","| 75 | QSPI_SS | 1k to BOOTSEL switch; no pull-up |",
"| 30, 31 | XIN, XOUT | Crystal; XOUT through 1k |","| 33, 34 | SWCLK, SWDIO | J14 |","| 35 | RUN | Reset switch and test point |","| 81 | Exposed pad | `GND` |","",
"## Changes from the brief's proposed table","",
"- Display moved to GP32-38, the pins the firmware already runs at 75 MHz. This also frees the HSTX bank, GP12-19.",
"- microSD moved to GP28-31 on SPI1. Both moves were approved by the team.",
"- GP9 is now the backup module's data input. MCLK moved to GP21, which has a real clock output; a PIO clock on GP9 could not produce audio rates exactly.",
"- Haptics moved to GP2/GP3 from GP28/GP29.",
"- After approval, four provisional or spare pins changed during detailed design: GP11 is `HP_DET` instead of DAC_INT; GP44 is `MUX_ST` instead of PWR_SW_SENSE (the slide switch turns the regulator off, so firmware could never read it); GP45 and GP46 now control the charger; GP47 drives a status LED.",
"","## ADC note","","On the QFN-80 only GP40-47 reach the ADC. GP26-29 do not, unlike the RP2040 and QFN-60 parts.",""]
open(os.path.join(HERE,"..","hardware","docs","pin_map.md"),"w",encoding="utf8",newline="\n").write("\n".join(out))
print("pin map OK",len(rows))
