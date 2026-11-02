#!/usr/bin/env python3
"""PiPod RP2354B motherboard schematic definition.

Every value here comes from a datasheet, a reference schematic or a calculation
recorded in the sheet notes. Run: python hw_gen/design.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from kicadgen import Project  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "hardware")

p = Project("PiPod", OUT, "PiPod RP2354B Motherboard", "A", date="2026-11-01")

# Footprints
F_R0603 = "Resistor_SMD:R_0603_1608Metric"
F_R0805 = "Resistor_SMD:R_0805_2012Metric"
F_C0603 = "Capacitor_SMD:C_0603_1608Metric"
F_C0805 = "Capacitor_SMD:C_0805_2012Metric"
F_C1210 = "Capacitor_SMD:C_1210_3225Metric"
F_TP = "TestPoint:TestPoint_Pad_D1.5mm"


def yageo(value, size="0603"):
    code = {
        "0R": "0R", "22R": "22R", "27R": "27R", "33R": "33R", "1k": "1K", "1.78k": "1K78",
        "1.5k": "1K5", "2.2k": "2K2", "4.7k": "4K7", "5.1k": "5K1", "10k": "10K", "47k": "47K",
        "80.6k": "80K6", "100k": "100K", "300k": "300K", "1M": "1M", "1.58k": "1K58",
    }[value]
    if value == "0R":
        return f"RC{size}JR-07{code}L"
    return f"RC{size}FR-07{code}L"


def R(s, ref, value, a, b, size="0603", dnp=False, desc="", status="datasheet-checked"):
    fp = {"0603": F_R0603, "0805": F_R0805}[size]
    s.part(ref, "Device:R", value, fp, {"1": a, "2": b}, mpn=yageo(value, size), mfr="Yageo",
           dnp=dnp, desc=desc or "Resistor", fields={"Verification": status}, group=None)


CAPS = {
    "100n": (F_C0603, "C0603C104K4RACTU", "KEMET"),
    "1u": (F_C0603, "C0603C105K4RACTU", "KEMET"),
    "4.7u": (F_C0603, "C0603C475K8PACTU", "KEMET"),
    "10u": (F_C0805, "EMK212ABJ106KD-T", "Taiyo Yuden"),
    "22u": (F_C0805, "LMK212BJ226MG-T", "Taiyo Yuden"),
    "15p": (F_C0603, "GRM1885C2A150JA01D", "Murata"),
    "100u": (F_C1210, "LMK325BJ107MM-T", "Taiyo Yuden"),
}


def C(s, ref, value, a, b="GND", dnp=False, desc=""):
    fp, mpn, mfr = CAPS[value]
    s.part(ref, "Device:C", value, fp, {"1": a, "2": b}, mpn=mpn, mfr=mfr, dnp=dnp,
           desc=desc or "Ceramic capacitor", fields={"Verification": "datasheet-checked"})


def TP(s, ref, net):
    s.part(ref, "Connector:TestPoint", net, F_TP, {"1": net}, desc="Test point",
           fields={"Verification": "library footprint"})


# ---------------------------------------------------------------------------
# Custom symbols (pin numbers copied from the named datasheet tables)
# ---------------------------------------------------------------------------
p.custom_symbol("TLV320DAC3100", [
    ("2", "IOVDD", "power_in", "L"), ("3", "DVDD", "power_in", "L"), ("17", "AVDD", "power_in", "L"),
    ("28", "HPVDD", "power_in", "L"), ("21", "SPKVDD", "power_in", "L"), ("24", "SPKVDD", "passive", "L"),
    ("31", "~{RESET}", "input", "L"), ("9", "SDA", "bidirectional", "L"), ("10", "SCL", "input", "L"),
    ("5", "DIN", "input", "L"), ("6", "WCLK", "bidirectional", "L"), ("7", "BCLK", "bidirectional", "L"),
    ("8", "MCLK", "input", "L"), ("32", "GPIO1", "bidirectional", "L"),
    ("27", "HPL", "output", "R"), ("30", "HPR", "output", "R"), ("22", "SPKP", "output", "R"),
    ("26", "SPKP", "passive", "R"), ("19", "SPKM", "output", "R"), ("23", "SPKM", "passive", "R"),
    ("12", "MICBIAS", "power_out", "R"), ("11", "VOL/MICDET", "input", "R"), ("13", "AIN1", "input", "R"),
    ("14", "AIN2", "input", "R"), ("4", "NC", "no_connect", "R"), ("15", "NC", "no_connect", "R"),
    ("1", "IOVSS", "power_in", "R"), ("16", "AVSS", "power_in", "R"), ("18", "DVSS", "power_in", "R"),
    ("20", "SPKVSS", "power_in", "R"), ("25", "SPKVSS", "passive", "R"), ("29", "HPVSS", "power_in", "R"),
    ("33", "EP", "passive", "R"),
], footprint="Package_DFN_QFN:Texas_RHB0032E_VQFN-32-1EP_5x5mm_P0.5mm_EP3.45x3.45mm",
    description="Stereo audio DAC with headphone and class-D speaker outputs, TI SLAS671C")

p.custom_symbol("TPS25200", [
    ("6", "IN", "power_in", "L"), ("4", "EN", "input", "L"), ("5", "GND", "power_in", "L"),
    ("7", "PAD", "passive", "L"), ("1", "OUT", "power_out", "R"), ("2", "ILIM", "passive", "R"),
    ("3", "~{FAULT}", "open_collector", "R"),
], footprint="Package_SON:WSON-6-1EP_2x2mm_P0.65mm_EP1x1.6mm",
    description="5 V eFuse with current limit and overvoltage clamp, TI SLVSCJ0F")

p.custom_symbol("SJ2-35954D", [
    ("2", "TIP", "passive", "L"), ("10", "TIP_SW", "passive", "L"), ("3", "RING1", "passive", "L"),
    ("11", "RING1_SW", "passive", "L"), ("4", "RING2", "passive", "L"), ("1", "SLEEVE", "passive", "L"),
], footprint="PiPod:Jack_3.5mm_SameSky_SJ2-35954D-SMT", reference="J",
    description="3.5 mm 4-conductor jack with tip and ring switches, Same Sky SJ2-3595X-SMT")

# ---------------------------------------------------------------------------
# 01 RP2354B core
# ---------------------------------------------------------------------------
s = p.sheet("01_RP2354B_Core", "01_rp2354b_core.kicad_sch")
s.note("RP2354B core. Follows Raspberry Pi 'Hardware design with RP2350' (Aug 2026) and Proton Rev4.\n"
       "No external flash: the RP2354B has 2 MB in package, so QSPI SD0-SD3 and SCLK stay unconnected.\n"
       "QSPI_SS keeps only the BOOTSEL path through 1k, as the guide requires.\n"
       "Regulator: L1 and capacitor values follow the guide. Inductor polarity dot matters.\n"
       "DEVIATION: the team requires 0603 minimum, so C1-C3 and R3 are 0603 where the guide uses 0402; see report.\n"
       "R_XTAL 1k and 15 pF are tuned for ABM8-272-T3 at 3.3 V. Do not substitute the crystal.\n"
       "R1/R2 (27R) sit close to the MCU per the guide. GP21 has CLOCK GPOUT0 for optional MCLK.")
gp = {
    "77": "UART0_TX", "78": "UART0_RX", "79": "HAPTIC_EN", "80": "HAPTIC_TRIG",
    "1": "I2C0_SDA", "2": "I2C0_SCL", "3": "I2S_BCLK", "4": "I2S_LRCLK",
    "6": "I2S_DOUT", "7": "I2S_DIN", "8": "DAC_RST_N", "9": "HP_DET",
    "11": "EXP_GP12", "12": "EXP_GP13", "13": "EXP_GP14", "14": "EXP_GP15",
    "16": "EXP_GP16", "17": "EXP_GP17", "18": "EXP_GP18", "19": "EXP_GP19",
    "20": "SD_CD", "21": "I2S_MCLK", "22": "BTN_MENU", "23": "BTN_PLAY",
    "25": "BTN_PREV", "26": "BTN_NEXT", "27": "BTN_SELECT", "28": "TOUCH_IRQ",
    "36": "SD_MISO", "37": "SD_CS", "38": "SD_SCK", "39": "SD_MOSI",
    "40": "DISP_MISO", "42": "DISP_CS", "43": "DISP_SCK", "44": "DISP_MOSI",
    "45": "DISP_DC", "46": "DISP_RST", "47": "DISP_BL", "48": "CHG_N",
    "49": "VBAT_SENSE", "52": "VSYS_SENSE", "53": "PGOOD_N", "54": "VBUS_SENSE",
    "55": "MUX_ST", "56": "CHG_EN1", "57": "CHG_CE_N", "58": "STATUS_LED",
    # power
    "5": "+3V3", "15": "+3V3", "24": "+3V3", "29": "+3V3", "41": "+3V3", "50": "+3V3",
    "60": "+3V3", "76": "+3V3", "68": "+3V3", "69": "+3V3", "59": "+3V3", "64": "+3V3",
    "61": "VREG_AVDD", "10": "+1V1", "32": "+1V1", "51": "+1V1", "65": "+1V1",
    "63": "VREG_LX", "62": "GND", "81": "GND",
    # control
    "30": "XIN", "31": "XOUT", "33": "SWCLK", "34": "SWDIO", "35": "RUN",
    "66": "USB_DM_MCU", "67": "USB_DP_MCU", "75": "QSPI_SS",
}
s.part("U1", "MCU_RaspberryPi:RP2354B", "RP2354B",
       "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP3.4x3.4mm", gp, nc=("70", "71", "72", "73", "74"),
       mpn="SC1512-A4", mfr="Raspberry Pi", desc="MCU with 2 MB stacked flash, QFN-80",
       fields={"Verification": "datasheet-checked; footprint needs human check"})
s.part("L1", "Device:L", "3.3u", "Inductor_SMD:L_Abracon_AOTA-B201610S", {"1": "VREG_LX", "2": "+1V1"},
       mpn="AOTA-B201610S3R3-101-T", mfr="Abracon", desc="RP2350 core regulator inductor, polarity marked",
       fields={"Verification": "named by Raspberry Pi guide"})
C(s, "C1", "4.7u", "+3V3", desc="VREG_VIN input (guide C6)")
C(s, "C2", "4.7u", "+1V1", desc="VREG output (guide C7)")
C(s, "C3", "4.7u", "VREG_AVDD", desc="VREG_AVDD filter (guide C9)")
R(s, "R3", "33R", "+3V3", "VREG_AVDD", desc="VREG_AVDD filter (guide R3)")
C(s, "C4", "10u", "+3V3", desc="3V3 bulk at MCU")
for i, n in enumerate(["+3V3"] * 10 + ["+1V1"] * 3):
    C(s, f"C{100 + i}", "100n", n, desc="Decoupling, one per power pin; USB_OTP_VDD and QSPI_IOVDD share one per guide")
R(s, "R1", "27R", "USB_DP_MCU", "USB_DP", desc="USB series termination")
R(s, "R2", "27R", "USB_DM_MCU", "USB_DM", desc="USB series termination")
s.part("Y1", "Device:Crystal_GND24", "12MHz", "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm",
       {"1": "XIN", "3": "XTAL_OUT", "2": "GND", "4": "GND"}, mpn="ABM8-272-T3", mfr="Abracon",
       desc="12 MHz crystal named by Raspberry Pi", fields={"Verification": "named by Raspberry Pi guide"})
R(s, "R4", "1k", "XOUT", "XTAL_OUT", desc="Crystal drive limiter (guide R2)")
C(s, "C5", "15p", "XIN", desc="Crystal load")
C(s, "C6", "15p", "XTAL_OUT", desc="Crystal load")
R(s, "R5", "1k", "QSPI_SS", "BOOTSEL_SW", desc="BOOTSEL series resistor (guide R6)")
s.part("SW1", "Switch:SW_Push", "BOOTSEL", "Button_Switch_SMD:SW_SPST_PTS647_Sx38",
       {"1": "BOOTSEL_SW", "2": "GND"}, mpn="PTS647SM38SMTR2LFS", mfr="C&K", desc="Enters USB boot on reset",
       fields={"Verification": "same part as Proton Rev4"})
s.part("SW2", "Switch:SW_Push", "RESET", "Button_Switch_SMD:SW_SPST_PTS647_Sx38",
       {"1": "RUN", "2": "GND"}, mpn="PTS647SM38SMTR2LFS", mfr="C&K", desc="Pulls RUN low",
       fields={"Verification": "same part as Proton Rev4"})
R(s, "R6", "1k", "STATUS_LED", "STATUS_LED_A", desc="Status LED current limit")
s.part("D1", "Device:LED", "GREEN", "LED_SMD:LED_0805_2012Metric", {"1": "GND", "2": "STATUS_LED_A"},
       mpn="LTST-C171KGKT", mfr="Lite-On", desc="Firmware status LED", fields={"Verification": "same part as Proton Rev4"})

# ---------------------------------------------------------------------------
# 02 USB and power entry
# ---------------------------------------------------------------------------
s = p.sheet("02_USB_Power_Entry", "02_usb_power_entry.kicad_sch")
s.note("USB-C sink, 5 V only. 5.1k on each CC identifies a sink. No USB PD.\n"
       "VBUS_RAW feeds two independent paths: the charger IN (R20 removable), and the U3 eFuse fallback.\n"
       "U3 RILIM 80.6k gives 1110-1300 mA per the TPS25200 table.\n"
       "U3 clamps OUT to 5.25-5.55 V and shuts off above 7.6 V; input withstands 20 V.\n"
       "D2 absorbs ESD and short transients on VBUS. It is not an overvoltage regulator.\n"
       "SBU1/SBU2 unused. Shield goes to GND through R12 at the connector.")
s.part("J1", "Connector:USB_C_Receptacle_USB2.0_16P", "USB-C",
       "Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal",
       {"A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "A4": "VBUS_RAW", "A9": "VBUS_RAW",
        "B4": "VBUS_RAW", "B9": "VBUS_RAW", "A5": "USB_CC1", "B5": "USB_CC2", "A6": "USB_CONN_DP",
        "B6": "USB_CONN_DP", "A7": "USB_CONN_DM", "B7": "USB_CONN_DM", "SH": "USB_SHIELD"},
       nc=("A8", "B8"), mpn="USB4105-GF-A", mfr="GCT", desc="USB-C receptacle, USB 2.0",
       fields={"Verification": "library footprint names this part"})
R(s, "R10", "5.1k", "USB_CC1", "GND", desc="CC1 sink pull-down")
R(s, "R11", "5.1k", "USB_CC2", "GND", desc="CC2 sink pull-down")
R(s, "R12", "0R", "USB_SHIELD", "GND", desc="Shield to ground link")
s.part("U2", "Power_Protection:USBLC6-2SC6", "USBLC6-2SC6", "Package_TO_SOT_SMD:SOT-23-6",
       {"1": "USB_CONN_DP", "6": "USB_DP", "3": "USB_CONN_DM", "4": "USB_DM", "5": "VBUS_RAW", "2": "GND"},
       mpn="USBLC6-2SC6", mfr="STMicroelectronics", desc="USB data line ESD protection",
       fields={"Verification": "same family as Proton Rev4; datasheet not re-downloaded"})
s.part("D2", "Device:D_Zener", "SMF5.0A", "Diode_SMD:D_SMF", {"1": "VBUS_RAW", "2": "GND"}, mpn="SMF5.0A",
       mfr="Littelfuse", desc="VBUS transient clamp, 5 V standoff", fields={"Verification": "needs human check"})
C(s, "C20", "4.7u", "VBUS_RAW", desc="VBUS input bulk; total with C21 and C30 stays under the USB 10 uF sink limit")
C(s, "C21", "100n", "VBUS_RAW", desc="U3 input capacitor, TPS25200 requires at least 0.1 uF")
s.part("U3", "PiPod:TPS25200", "TPS25200", "Package_SON:WSON-6-1EP_2x2mm_P0.65mm_EP1x1.6mm",
       {"6": "VBUS_RAW", "4": "U3_EN", "5": "GND", "7": "GND", "1": "USB_5V", "2": "U3_ILIM", "3": "USB_FAULT_N"},
       mpn="TPS25200DRVR", mfr="Texas Instruments", desc="Independent USB fallback eFuse",
       fields={"Verification": "datasheet-checked; custom symbol"})
R(s, "R13", "300k", "VBUS_RAW", "U3_EN", desc="EN tie through 300k per TPS25200 typical application")
R(s, "R14", "80.6k", "U3_ILIM", "GND", desc="Sets 1.2 A limit")
R(s, "R15", "100k", "USB_FAULT_N", "+3V3", desc="Open-drain pull-up")
C(s, "C22", "22u", "USB_5V", desc="U3 output capacitor, TPS25200 typical application")
R(s, "R16", "100k", "VBUS_RAW", "VBUS_SENSE", desc="VBUS divider top: 5 V reads 1.6 V")
R(s, "R17", "47k", "VBUS_SENSE", "GND", desc="VBUS divider bottom")

# ---------------------------------------------------------------------------
# 03 Battery charging
# ---------------------------------------------------------------------------
s = p.sheet("03_Battery_Charging", "03_battery_charging.kicad_sch")
s.note("BQ24074: OVP 10.5 V, OUT regulated to 4.4 V, charges to 4.2 V, TI SLUS810N.\n"
       "R21 ISET 1.78k: ICHG = 890 / 1780 = 500 mA, 0.5C for the assumed 1000 mAh cell.\n"
       "R23 ILIM 1.58k: about 1 A if EN2=1, EN1=0, which this strapping never selects. EN2 is tied low, so only USB100 and USB500 are reachable.\n"
       "Leaving ILIM open disables charging, per the datasheet, so R23 stays fitted.\n"
       "EN1 reset default is low (GPIO and 285k internal pull-down): 100 mA. Firmware sets 500 mA.\n"
       "CHG_CE_N high disables charging. R24 keeps it low until firmware runs.\n"
       "TMR and ITERM left open deliberately: datasheet defaults (default timers, 10% termination).\n"
       "TS has a fixed 10k because the assumed battery has no thermistor, per the datasheet. Battery must be protected.\n"
       "R20 and R25 remove the charger from the circuit. R26 (DNP) keeps battery power if the charger is removed:\n"
       "fit R26 ONLY with U4 removed. Never fit it with U4 in place.\n"
       "MECHANICAL: JST-PH LiPo polarity varies between vendors. Confirm against the real cell.")
R(s, "R20", "0R", "VBUS_RAW", "CHG_IN", size="0805", desc="Charger input isolation link")
C(s, "C30", "1u", "CHG_IN", desc="IN bypass, 1-10 uF per datasheet")
s.part("U4", "Battery_Management:BQ24074RGT", "BQ24074", "Package_DFN_QFN:VQFN-16-1EP_3x3mm_P0.5mm_EP1.6x1.6mm",
       {"1": "CHG_TS", "2": "VBAT", "3": "VBAT", "4": "CHG_CE_N", "5": "GND", "6": "CHG_EN1", "7": "PGOOD_N",
        "8": "GND", "17": "GND", "9": "CHG_N", "10": "CHG_OUT", "11": "CHG_OUT", "12": "CHG_ILIM",
        "13": "CHG_IN", "16": "CHG_ISET"}, nc=("14", "15"),
       mpn="BQ24074RGTR", mfr="Texas Instruments", desc="Li-ion charger with power path",
       fields={"Verification": "datasheet-checked; check EP against RGT0016B drawing"})
R(s, "R21", "1.78k", "CHG_ISET", "GND", desc="Sets 500 mA fast charge")
R(s, "R22", "10k", "CHG_TS", "GND", desc="Fixed TS resistor, no thermistor")
R(s, "R23", "1.58k", "CHG_ILIM", "GND", desc="ILIM must be connected for charging")
R(s, "R24", "100k", "CHG_CE_N", "GND", desc="Charging enabled by default")
R(s, "R27", "10k", "CHG_N", "+3V3", desc="Open-drain pull-up")
R(s, "R28", "10k", "PGOOD_N", "+3V3", desc="Open-drain pull-up")
C(s, "C31", "10u", "CHG_OUT", desc="OUT bypass, 4.7-47 uF per datasheet")
C(s, "C32", "10u", "VBAT", desc="BAT bypass, 4.7-47 uF per datasheet")
R(s, "R25", "0R", "CHG_OUT", "BATT_SYS", size="0805", desc="Charger output isolation link")
R(s, "R26", "0R", "VBAT", "BATT_SYS", size="0805", dnp=True, desc="Battery bypass; fit only with U4 removed")
s.part("J2", "Connector_Generic_MountingPin:Conn_01x02_MountingPin", "BATTERY",
       "Connector_JST:JST_PH_S2B-PH-SM4-TB_1x02-1MP_P2.00mm_Horizontal",
       {"1": "VBAT", "2": "GND", "MP": "GND"}, mpn="S2B-PH-SM4-TB(LF)(SN)", mfr="JST",
       desc="Battery connector, pin 1 positive; confirm cell polarity", fields={"Verification": "provisional"})
R(s, "R29", "1M", "VBAT", "VBAT_SENSE", desc="VBAT divider top")
R(s, "R30", "1M", "VBAT_SENSE", "GND", desc="VBAT divider bottom")
C(s, "C33", "100n", "VBAT_SENSE", desc="ADC reservoir for high-impedance divider")

# ---------------------------------------------------------------------------
# 04 Power selection and regulation
# ---------------------------------------------------------------------------
s = p.sheet("04_Power_Selection_Regulation", "04_power_selection_regulation.kicad_sch")
s.note("U5 priority mode: VIN1 = USB_5V (eFuse fallback), VIN2 = BATT_SYS (charger OUT or battery).\n"
       "R40/R41 set the switch point: 1.0 V x (300k+100k)/100k = 4.0 V, 3.68-4.32 V with VREF tolerance.\n"
       "With USB present the system runs from U3 and does not depend on the charger.\n"
       "Switching is break-before-make (8 us at 5 V); the new channel waits until VSYS falls to its input.\n"
       "C40, C41 hold VSYS through that time. Regulator stays in range because input stays above 1.8 V.\n"
       "U6 TPS63001: 1.8-5.5 V in, 3.3 V out, 1200 mA in buck, up to 800 mA in boost, TI SLVS520C.\n"
       "SW3 pulls U6 EN low to turn the device off. All loads run from 3V3, so they all switch off.\n"
       "Charging continues with SW3 off. Battery still feeds U4 and U5 quiescent current, sense dividers.\n"
       "PS/SYNC default: power save enabled through R44. Fit R45 and remove R44 for forced PWM if audio needs it.")
s.part("U5", "Power_Management:TPS2116DRL", "TPS2116", "Package_TO_SOT_SMD:SOT-583-8",
       {"1": "GND", "2": "VSYS", "7": "VSYS", "3": "USB_5V", "4": "MUX_PR1", "5": "USB_5V", "6": "BATT_SYS",
        "8": "MUX_ST"}, mpn="TPS2116DRLR", mfr="Texas Instruments", desc="Priority power mux",
       fields={"Verification": "datasheet-checked"})
R(s, "R40", "300k", "USB_5V", "MUX_PR1", desc="PR1 divider top")
R(s, "R41", "100k", "MUX_PR1", "GND", desc="PR1 divider bottom")
R(s, "R42", "100k", "MUX_ST", "+3V3", desc="Open-drain pull-up; low means battery path in use")
C(s, "C40", "22u", "VSYS", desc="VSYS bulk")
C(s, "C41", "22u", "VSYS", desc="VSYS bulk")
s.part("U6", "Regulator_Switching:TPS63001", "TPS63001", "Package_SON:Texas_DRC0010J",
       {"5": "VSYS", "8": "VINA", "6": "REG_EN", "7": "REG_PS", "4": "BB_L1", "2": "BB_L2", "1": "+3V3",
        "10": "+3V3", "9": "GND", "3": "GND", "11": "GND"},
       mpn="TPS63001DRCR", mfr="Texas Instruments", desc="3.3 V buck-boost regulator",
       fields={"Verification": "datasheet-checked"})
s.part("L2", "Device:L", "2.2u", "Inductor_SMD:L_1210_3225Metric", {"1": "BB_L1", "2": "BB_L2"},
       mpn="DFE322512F-2R2M=P2", mfr="Murata",
       desc="Buck-boost inductor, 2.6 A Irms; TI's named VLF4012 is obsolete, same value and above the 1.8 A switch rating",
       fields={"Verification": "substitute; check land pattern against Murata drawing"})
C(s, "C42", "10u", "VSYS", desc="U6 input, TI table C1")
C(s, "C43", "10u", "+3V3", desc="U6 output, TI table C2")
C(s, "C44", "10u", "+3V3", desc="U6 output, TI table C2")
C(s, "C45", "100n", "VINA", desc="U6 VINA filter, TI table C3")
R(s, "R43", "100k", "VSYS", "REG_EN", desc="EN pull-up; SW3 grounds it to turn off")
s.part("R46", "Device:R", "100", F_R0603, {"1": "VSYS", "2": "VINA"}, mpn="RC0603FR-07100RL", mfr="Yageo",
       desc="U6 VINA filter, TI table R3", fields={"Verification": "datasheet-checked"})
R(s, "R44", "0R", "REG_PS", "GND", desc="Power save enabled")
R(s, "R45", "0R", "REG_PS", "VINA", dnp=True, desc="Forced PWM option")
s.part("SW3", "Switch:SW_SPDT", "POWER", "Button_Switch_SMD:SW_SPDT_CK_JS102011SAQN",
       {"2": "REG_EN", "1": "GND"}, nc=("3",), mpn="JS102011SAQN", mfr="C&K",
       desc="Power slide switch; signal-level only, switches U6 EN", fields={"Verification": "provisional mechanical"})
R(s, "R47", "1M", "VSYS", "VSYS_SENSE", desc="VSYS divider top")
R(s, "R48", "1M", "VSYS_SENSE", "GND", desc="VSYS divider bottom")
C(s, "C46", "100n", "VSYS_SENSE", desc="ADC reservoir")
for n in ("GND", "VBUS_RAW", "CHG_IN", "BATT_SYS", "VINA", "+1V1", "VREG_AVDD"):
    s.pwr_flag(n)

# ---------------------------------------------------------------------------
# 05 Display
# ---------------------------------------------------------------------------
s = p.sheet("05_Display", "05_display.kicad_sch")
s.note("Connection for a COMPLETE 2.2-inch ILI9341 SPI module, not a bare panel.\n"
       "PROVISIONAL: J3 pin order follows the module pin list given by the team. Confirm against the actual module.\n"
       "LED pin is switched high-side by Q2 (P-FET), driven by Q1 from PWM. Keeps active-high PWM in firmware.\n"
       "R52 is a series link for backlight current; change only if the module has no LED resistor.\n"
       "R50/R51 damp the 75 MHz SPI edges. Keep this net short.")
s.part("J3", "Connector_Generic:Conn_01x09", "DISPLAY", "Connector_PinSocket_2.54mm:PinSocket_1x09_P2.54mm_Vertical",
       {"1": "+3V3", "2": "GND", "3": "DISP_CS", "4": "DISP_RST", "5": "DISP_DC", "6": "DISP_MOSI_R",
        "7": "DISP_SCK_R", "8": "DISP_LED", "9": "DISP_MISO"}, mpn="PPTC091LFBN-RC", mfr="Sullins",
       desc="Display module socket", fields={"Verification": "provisional pin order"})
R(s, "R50", "22R", "DISP_MOSI", "DISP_MOSI_R", desc="SPI edge damping")
R(s, "R51", "22R", "DISP_SCK", "DISP_SCK_R", desc="SPI edge damping")
C(s, "C50", "10u", "+3V3", desc="Display supply bulk")
C(s, "C51", "100n", "+3V3", desc="Display supply decoupling")
s.part("Q1", "Transistor_FET:2N7002", "2N7002", "Package_TO_SOT_SMD:SOT-23",
       {"1": "DISP_BL", "2": "GND", "3": "BL_GATE"}, mpn="2N7002", mfr="Nexperia", desc="Backlight level driver",
       fields={"Verification": "library symbol"})
s.part("Q2", "Transistor_FET:AO3401A", "AO3401A", "Package_TO_SOT_SMD:SOT-23",
       {"1": "BL_GATE", "2": "+3V3", "3": "BL_SW"}, mpn="AO3401A", mfr="Alpha & Omega", desc="Backlight high-side switch",
       fields={"Verification": "library symbol"})
R(s, "R53", "100k", "BL_GATE", "+3V3", desc="Q2 off by default")
R(s, "R54", "100k", "DISP_BL", "GND", desc="Backlight off until firmware runs")
R(s, "R52", "0R", "BL_SW", "DISP_LED", size="0603", desc="Backlight series link")

# ---------------------------------------------------------------------------
# 06 microSD
# ---------------------------------------------------------------------------
s = p.sheet("06_MicroSD", "06_microsd.kicad_sch")
s.note("SPI mode on SPI1. CMD = MOSI, DAT0 = MISO, DAT3 = CS. DAT1 and DAT2 pulled up, unused in SPI mode.\n"
       "R60 lets you unpower the card for debug. Same 3.3 V domain as the MCU, so no level shifter.\n"
       "U7/U8 provide ESD protection at the socket on all four SPI lines.\n"
       "R66 damps SCK. Detect switch: open with no card, closed with a card, so SD_CD reads low when a card is present (Hirose DM3 datasheet).")
s.part("J4", "Connector:Micro_SD_Card_Det_Hirose_DM3AT", "microSD", "Connector_Card:microSD_HC_Hirose_DM3AT-SF-PEJM5",
       {"1": "SD_DAT2", "2": "SD_CS", "3": "SD_MOSI", "4": "SD_3V3", "5": "SD_SCK_C", "6": "GND", "7": "SD_MISO",
        "8": "SD_DAT1", "9": "SD_CD", "10": "GND", "SH": "GND"}, mpn="DM3AT-SF-PEJM5(40)", mfr="Hirose",
       desc="Push-pull microSD socket", fields={"Verification": "library footprint names this part"})
R(s, "R60", "0R", "+3V3", "SD_3V3", size="0603", desc="Card supply link")
s.pwr_flag("SD_3V3")
C(s, "C60", "10u", "SD_3V3", desc="Card supply bulk")
C(s, "C61", "100n", "SD_3V3", desc="Card decoupling")
R(s, "R61", "10k", "SD_CS", "SD_3V3", desc="CS idle high, card stays deselected")
R(s, "R62", "10k", "SD_MOSI", "SD_3V3", desc="CMD pull-up")
R(s, "R63", "10k", "SD_MISO", "SD_3V3", desc="DAT0 pull-up")
R(s, "R64", "10k", "SD_DAT1", "SD_3V3", desc="DAT1 pull-up")
R(s, "R65", "10k", "SD_DAT2", "SD_3V3", desc="DAT2 pull-up")
R(s, "R66", "22R", "SD_SCK", "SD_SCK_C", desc="SCK edge damping")
R(s, "R67", "100k", "SD_CD", "+3V3", desc="Card detect pull-up")
s.part("U7", "Power_Protection:USBLC6-2SC6", "USBLC6-2SC6", "Package_TO_SOT_SMD:SOT-23-6",
       {"1": "SD_CS", "6": "SD_CS", "3": "SD_MOSI", "4": "SD_MOSI", "5": "SD_3V3", "2": "GND"},
       mpn="USBLC6-2SC6", mfr="STMicroelectronics", desc="SD ESD protection",
       fields={"Verification": "same family as Proton Rev4"})
s.part("U8", "Power_Protection:USBLC6-2SC6", "USBLC6-2SC6", "Package_TO_SOT_SMD:SOT-23-6",
       {"1": "SD_MISO", "6": "SD_MISO", "3": "SD_SCK_C", "4": "SD_SCK_C", "5": "SD_3V3", "2": "GND"},
       mpn="USBLC6-2SC6", mfr="STMicroelectronics", desc="SD ESD protection",
       fields={"Verification": "same family as Proton Rev4"})

# ---------------------------------------------------------------------------
# 07 Audio
# ---------------------------------------------------------------------------
s = p.sheet("07_Audio_Codec", "07_audio_codec.kicad_sch")
s.note("TLV320DAC3100 replaces the brief's WM8960 (listed obsolete at Digi-Key). Team approved 2026-11-01.\n"
       "RP2354B is I2S master through PIO. The DAC PLL derives its clock from BCLK, so MCLK is not required.\n"
       "Headphones are AC coupled as the datasheet requires; 100 uF is the Adafruit 6309 value. Never ground HPL or HPR.\n"
       "All 3.3 V DAC pins share 3V3_A, then U10 makes DVDD; TI calls the part tolerant of sequencing. R77 holds RESET low while DVDD comes up, per datasheet.\n"
       "SPKVDD on 3V3_A meets SPKVDD >= AVDD. Speaker pads J6 are DNP and speaker output is reduced at 3.3 V.\n"
       "AIN1, AIN2, GPIO1 unused and left open as on the Adafruit 6309. NC pins 4, 15 per datasheet.\n"
       "Jack: CTIA wiring. Tip L, Ring1 R, Ring2 GND, Sleeve mic. Mic is biased only if firmware enables MICBIAS.\n"
       "HP_DET: with no plug the tip switch connects R78 through R79 to GND (low). A plug opens it (high).\n"
       "ISOLATION: remove R70-R74, R76 and R75 to use the backup module on J8 without contention.")
for ref, a, b in [("R70", "I2S_BCLK", "DAC_BCLK"), ("R71", "I2S_LRCLK", "DAC_WCLK"), ("R72", "I2S_DOUT", "DAC_DIN"),
                  ("R73", "I2C0_SDA", "DAC_SDA_LOCAL"), ("R74", "I2C0_SCL", "DAC_SCL_LOCAL")]:
    R(s, ref, "0R", a, b, desc="DAC isolation link")
R(s, "R75", "0R", "+3V3", "3V3_DAC_LINK", size="0603", desc="DAC supply isolation link")
s.part("FB1", "Device:FerriteBead", "BLM18KG601SN1D", "Inductor_SMD:L_0603_1608Metric",
       {"1": "3V3_DAC_LINK", "2": "3V3_A"}, mpn="BLM18KG601SN1D", mfr="Murata", desc="Audio supply filter",
       fields={"Verification": "check MPN"})
s.pwr_flag("3V3_A")
R(s, "R76", "0R", "DAC_RST_N", "DAC_RST_LOCAL", desc="DAC reset isolation link")
R(s, "R77", "100k", "DAC_RST_LOCAL", "GND", desc="Holds RESET low during DVDD power-up, per datasheet")
s.part("U9", "PiPod:TLV320DAC3100", "TLV320DAC3100", "Package_DFN_QFN:Texas_RHB0032E_VQFN-32-1EP_5x5mm_P0.5mm_EP3.45x3.45mm",
       {"2": "3V3_A", "17": "3V3_A", "28": "3V3_A", "21": "3V3_A", "24": "3V3_A", "3": "1V8_A", "31": "DAC_RST_LOCAL",
        "9": "DAC_SDA_LOCAL", "10": "DAC_SCL_LOCAL", "5": "DAC_DIN", "6": "DAC_WCLK", "7": "DAC_BCLK", "8": "DAC_MCLK",
        "27": "HP_L_DAC", "30": "HP_R_DAC", "22": "SPK_P", "26": "SPK_P", "19": "SPK_M", "23": "SPK_M",
        "12": "MIC_BIAS", "11": "MIC_DET", "1": "GND", "16": "GND", "18": "GND", "20": "GND", "25": "GND",
        "29": "GND", "33": "GND"}, nc=("32", "13", "14", "4", "15"),
       mpn="TLV320DAC3100IRHBR", mfr="Texas Instruments", desc="Stereo DAC, headphone driver, I2C 0x18",
       fields={"Verification": "datasheet-checked; custom symbol"})
R(s, "R80", "0R", "I2S_MCLK", "DAC_MCLK", dnp=True, desc="Optional MCLK from GP21")
R(s, "R81", "100k", "DAC_MCLK", "GND", desc="Keeps unused MCLK input at a defined level")
s.part("U10", "Regulator_Linear:AP2112K-1.8", "AP2112K-1.8", "Package_TO_SOT_SMD:SOT-23-5",
       {"1": "3V3_A", "3": "3V3_A", "2": "GND", "5": "1V8_A"}, nc=("4",), mpn="AP2112K-1.8TRG1", mfr="Diodes Inc.",
       desc="1.8 V DVDD regulator", fields={"Verification": "same family as Proton Rev4"})
C(s, "C70", "1u", "3V3_A", desc="LDO input")
C(s, "C71", "10u", "1V8_A", desc="DVDD bulk, datasheet Figure 7-1")
C(s, "C72", "100n", "1V8_A", desc="DVDD decoupling, datasheet Figure 7-1")
C(s, "C73", "10u", "3V3_A", desc="AVDD bulk")
C(s, "C74", "100n", "3V3_A", desc="AVDD decoupling")
C(s, "C75", "10u", "3V3_A", desc="HPVDD bulk")
C(s, "C76", "100n", "3V3_A", desc="HPVDD decoupling")
C(s, "C77", "10u", "3V3_A", desc="IOVDD bulk")
C(s, "C78", "100n", "3V3_A", desc="IOVDD decoupling")
C(s, "C79", "22u", "3V3_A", desc="SPKVDD bulk, datasheet Figure 7-1")
C(s, "C80", "100n", "3V3_A", desc="SPKVDD decoupling")
C(s, "C81", "100u", "HP_L_DAC", "HP_L", desc="Headphone left DC block")
C(s, "C82", "100u", "HP_R_DAC", "HP_R", desc="Headphone right DC block")
R(s, "R79", "4.7k", "HP_L", "GND", desc="Output bleed, gives HP_DET a low level")
R(s, "R82", "4.7k", "HP_R", "GND", desc="Output bleed")
s.part("J5", "PiPod:SJ2-35954D", "SJ2-35954D-SMT-TR", "PiPod:Jack_3.5mm_SameSky_SJ2-35954D-SMT",
       {"2": "HP_L", "3": "HP_R", "4": "GND", "1": "MIC_IN", "10": "HP_DET_SW"}, nc=("11",),
       mpn="SJ2-35954D-SMT-TR", mfr="Same Sky", desc="4-pole headphone jack",
       fields={"Verification": "custom footprint from drawing; needs human check"})
R(s, "R78", "100k", "HP_DET_SW", "+3V3", desc="Headphone detect pull-up")
R(s, "R83", "1k", "HP_DET_SW", "HP_DET", desc="Limits current into GPIO")
R(s, "R84", "2.2k", "MIC_BIAS", "MIC_IN", desc="Mic bias, as Adafruit 6309")
R(s, "R85", "0R", "MIC_IN", "MIC_DET", desc="Headset detect link, as Adafruit 6309")
s.part("J6", "Connector_Generic_MountingPin:Conn_01x02_MountingPin", "SPEAKER",
       "Connector_JST:JST_PH_S2B-PH-SM4-TB_1x02-1MP_P2.00mm_Horizontal", {"1": "SPK_P", "2": "SPK_M", "MP": "GND"},
       mpn="S2B-PH-SM4-TB(LF)(SN)", mfr="JST", dnp=True, desc="Optional speaker; class-D output, never ground either pin",
       fields={"Verification": "provisional"})

# ---------------------------------------------------------------------------
# 08 Capacitive wheel
# ---------------------------------------------------------------------------
s = p.sheet("08_Capacitive_Wheel", "08_capacitive_wheel.kicad_sch")
s.note("Team decision 2026-11-01: MPR121 stays on an Adafruit 1982 module in sockets J7/J9, I2C through Qwiic.\n"
       "This differs from the brief's integrated controller. Pin order read from Adafruit's board file.\n"
       "Rows sit 15.24 mm apart. J9 pin 1 at x = 1.27 mm, J7 pin 1 at x = 8.89 mm from the module's left edge.\n"
       "R90 ties ADDR to the module's 3V3 output, moving it from 0x5A to 0x5B (no clash with DRV2605L).\n"
       "R91-R93 (DNP) let you hard-wire power and I2C instead of using a Qwiic cable.\n"
       "E1-E12 are placeholders: the layout engineer replaces each with a copper electrode.\n"
       "Wheel diameter and overlay thickness are not final: layout and mechanical dependency.")
ele = {str(i + 1): f"WHEEL_E{11 - i}" for i in range(12)}
ele["13"] = "GND"
s.part("J9", "Connector_Generic:Conn_01x13", "MPR121_ELEC", "Connector_PinSocket_2.54mm:PinSocket_1x13_P2.54mm_Vertical",
       ele, mpn="PPTC131LFBN-RC", mfr="Sullins", desc="Adafruit 1982 electrode row",
       fields={"Verification": "checked against Adafruit board file"})
s.part("J7", "Connector_Generic:Conn_01x07", "MPR121_CTRL", "Connector_PinSocket_2.54mm:PinSocket_1x07_P2.54mm_Vertical",
       {"1": "MPR_VIN", "2": "MPR_3VO", "3": "GND", "4": "MPR_SCL", "5": "MPR_SDA", "6": "TOUCH_IRQ", "7": "MPR_ADDR"},
       mpn="PPTC071LFBN-RC", mfr="Sullins", desc="Adafruit 1982 control row",
       fields={"Verification": "checked against Adafruit board file"})
R(s, "R90", "0R", "MPR_3VO", "MPR_ADDR", desc="Sets address 0x5B")
R(s, "R91", "0R", "+3V3", "MPR_VIN", dnp=True, desc="Optional hard-wired power")
R(s, "R92", "0R", "I2C0_SCL", "MPR_SCL", dnp=True, desc="Optional hard-wired I2C")
R(s, "R93", "0R", "I2C0_SDA", "MPR_SDA", dnp=True, desc="Optional hard-wired I2C")
for i in range(12):
    s.part(f"E{i + 1}", "Connector:TestPoint", f"E{i}", "PiPod:WheelElectrode_Placeholder", {"1": f"WHEEL_E{i}"},
           desc="Wheel electrode placeholder, layout defines the copper", fields={"Verification": "layout dependency"})

# ---------------------------------------------------------------------------
# 09 Buttons and haptics
# ---------------------------------------------------------------------------
s = p.sheet("09_Buttons_Haptics", "09_buttons_haptics.kicad_sch")
s.note("Five independent active-low buttons with 10k pull-ups, so any mechanical layout uses the same firmware.\n"
       "PROVISIONAL: switch type is chosen for the schematic only; mechanics are not final.\n"
       "DRV2605L at 0x5A on 3V3. REG needs 1 uF, VDD needs 1 uF per datasheet. VDD/NC tied to VDD.\n"
       "IN/TRIG must not float: R96 holds it low until firmware drives GP3.\n"
       "ISOLATION: remove R94, R95, R97, R98 and R99 to use a Qwiic DRV2605L breakout instead.\n"
       "Motor is off-board and must suit 3.3 V: confirm its rated voltage and current when selected.")
for i, (ref, n) in enumerate([("SW4", "BTN_MENU"), ("SW5", "BTN_PLAY"), ("SW6", "BTN_PREV"), ("SW7", "BTN_NEXT"), ("SW8", "BTN_SELECT")]):
    s.part(ref, "Switch:SW_Push", n.replace("BTN_", ""), "Button_Switch_SMD:SW_SPST_PTS647_Sx38", {"1": n, "2": "GND"},
           mpn="PTS647SM38SMTR2LFS", mfr="C&K", desc="Provisional user button", fields={"Verification": "provisional mechanical"})
    R(s, f"R{100 + i}", "10k", n, "+3V3", desc="Button pull-up")
R(s, "R94", "0R", "I2C0_SDA", "HAPTIC_SDA_LOCAL", desc="Haptic isolation link")
R(s, "R95", "0R", "I2C0_SCL", "HAPTIC_SCL_LOCAL", desc="Haptic isolation link")
R(s, "R97", "0R", "HAPTIC_EN", "HAPTIC_EN_LOCAL", desc="Haptic isolation link")
R(s, "R98", "0R", "HAPTIC_TRIG", "HAPTIC_TRIG_LOCAL", desc="Haptic isolation link")
R(s, "R99", "0R", "+3V3", "HAPTIC_VDD", size="0603", desc="Haptic supply link")
s.pwr_flag("HAPTIC_VDD")
R(s, "R96", "100k", "HAPTIC_TRIG_LOCAL", "GND", desc="IN/TRIG defined low")
R(s, "R108", "100k", "HAPTIC_EN_LOCAL", "GND", desc="Driver disabled until firmware runs")
s.part("U11", "Driver:DRV2605LDGS", "DRV2605L", "Package_SO:TSSOP-10_3x3mm_P0.5mm",
       {"1": "HAPTIC_REG", "2": "HAPTIC_SCL_LOCAL", "3": "HAPTIC_SDA_LOCAL", "4": "HAPTIC_TRIG_LOCAL",
        "5": "HAPTIC_EN_LOCAL", "6": "HAPTIC_VDD", "7": "MOTOR_P", "8": "GND", "9": "MOTOR_N", "10": "HAPTIC_VDD"},
       mpn="DRV2605LDGST", mfr="Texas Instruments", desc="Haptic driver, I2C 0x5A; DRV2605LDGSR is the same part on a larger reel",
       fields={"Verification": "datasheet-checked; check land pattern"})
C(s, "C90", "1u", "HAPTIC_VDD", desc="VDD bypass, required")
C(s, "C91", "1u", "HAPTIC_REG", desc="REG capacitor, required")
s.part("J10", "Connector_Generic_MountingPin:Conn_01x02_MountingPin", "MOTOR",
       "Connector_JST:JST_SH_SM02B-SRSS-TB_1x02-1MP_P1.00mm_Horizontal",
       {"1": "MOTOR_P", "2": "MOTOR_N", "MP": "GND"}, mpn="SM02B-SRSS-TB(LF)(SN)", mfr="JST",
       desc="ERM motor connector", fields={"Verification": "library footprint"})

# ---------------------------------------------------------------------------
# 10 Expansion and backup
# ---------------------------------------------------------------------------
s = p.sheet("10_Expansion_Backup", "10_expansion_backup.kicad_sch")
s.note("Main I2C pull-ups R110/R111 sit on the MCU side, so the bus keeps working with onboard parts removed.\n"
       "Qwiic: 1 GND, 2 3V3, 3 SDA, 4 SCL. J11 connects the MPR121 module; J12 is free.\n"
       "4.7k in parallel with the module's 10k gives about 3.2k. Add up all breakouts' pull-ups when fitting more.\n"
       "J8 mirrors the Waveshare WM8960 header pin for pin (from its schematic). Confirm the module before use.\n"
       "Isolate the onboard DAC (sheet 07) before driving audio through J8.\n"
       "Waveshare's codec uses address 0x1A and its own 24 MHz clock.")
R(s, "R110", "4.7k", "I2C0_SDA", "+3V3", desc="I2C pull-up")
R(s, "R111", "4.7k", "I2C0_SCL", "+3V3", desc="I2C pull-up")
for ref in ("J11", "J12"):
    s.part(ref, "Connector_Generic_MountingPin:Conn_01x04_MountingPin", "QWIIC",
           "Connector_JST:JST_SH_SM04B-SRSS-TB_1x04-1MP_P1.00mm_Horizontal",
           {"1": "GND", "2": "+3V3", "3": "I2C0_SDA", "4": "I2C0_SCL", "MP": "GND"},
           mpn="SM04B-SRSS-TB(LF)(SN)", mfr="JST", desc="Qwiic connector", fields={"Verification": "library footprint; brief's MPN"})
s.part("J8", "Connector_Generic:Conn_02x08_Odd_Even", "AUDIO_BACKUP", "Connector_PinHeader_2.54mm:PinHeader_2x08_P2.54mm_Vertical",
       {"1": "+3V3", "2": "+3V3", "3": "GND", "4": "GND", "5": "I2C0_SDA", "7": "I2C0_SCL", "9": "I2S_BCLK", "10": "I2S_BCLK",
        "11": "I2S_LRCLK", "12": "I2S_LRCLK", "13": "I2S_DOUT", "14": "I2S_DIN", "15": "AUD_MCLK_HDR", "16": "AUD_MCLK_HDR"},
       nc=("6", "8"), mpn="PREC008DAAN-RC", mfr="Sullins", desc="Waveshare WM8960 backup header",
       fields={"Verification": "from module schematic; provisional"})
R(s, "R112", "0R", "I2S_MCLK", "AUD_MCLK_HDR", dnp=True, desc="Optional MCLK link; leave open with the module's own clock")
s.part("J13", "Connector_Generic:Conn_01x10", "EXPANSION", "Connector_PinHeader_2.54mm:PinHeader_1x10_P2.54mm_Vertical",
       {"1": "+3V3", "2": "EXP_GP12", "3": "EXP_GP13", "4": "EXP_GP14", "5": "EXP_GP15", "6": "EXP_GP16",
        "7": "EXP_GP17", "8": "EXP_GP18", "9": "EXP_GP19", "10": "GND"}, mpn="PREC010SAAN-RC", mfr="Sullins",
       desc="Spare GPIO and full HSTX bank", fields={"Verification": "library footprint"})

# ---------------------------------------------------------------------------
# 11 Debug and test points
# ---------------------------------------------------------------------------
s = p.sheet("11_Debug_Testpoints", "11_debug_testpoints.kicad_sch")
s.note("J14: 3V3 reference, SWCLK, SWDIO, GND, UART0 TX, UART0 RX. Works with the Proton's debug probe by jumpers.\n"
       "Do not back-power through J14 pin 1: it is a reference for the probe.")
s.part("J14", "Connector_Generic:Conn_01x06", "DEBUG", "Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical",
       {"1": "+3V3", "2": "SWCLK", "3": "SWDIO", "4": "GND", "5": "UART0_TX", "6": "UART0_RX"},
       mpn="PREC006SAAN-RC", mfr="Sullins", desc="SWD and UART debug header", fields={"Verification": "library footprint"})
for i, n in enumerate(["VBUS_RAW", "USB_5V", "VBAT", "CHG_OUT", "VSYS", "+3V3", "+1V1", "3V3_A", "1V8_A", "GND",
                       "SWDIO", "SWCLK", "RUN", "I2C0_SDA", "I2C0_SCL", "DISP_SCK", "DISP_MOSI", "DISP_CS", "SD_SCK",
                       "SD_MOSI", "SD_MISO", "SD_CS", "I2S_BCLK", "I2S_LRCLK", "I2S_DOUT", "TOUCH_IRQ", "HAPTIC_EN",
                       "HP_L", "HP_R", "USB_FAULT_N", "MUX_ST"]):
    TP(s, f"TP{i + 1}", n)

p.write()
p.export_bom(os.path.join(OUT, "PiPod_BOM.csv"))
print("generated", OUT)
