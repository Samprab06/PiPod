# RP2354B pin map

All GPIO are in the 3.3 V IOVDD domain. Functions were read from the RP2350 datasheet GPIO function table (F1 SPI, F2 UART, F3 I2C, F4 PWM). Package pins are from KiCad's RP2354B symbol and are cross-checked against U1 in the schematic by the generator.

| GPIO | Pin | Net | Peripheral | Function | Direction | Domain | Status |
|---|---|---|---|---|---|---|---|
| GP0 | 77 | `UART0_TX` | UART0 | UART0 TX (F2) | Out | 3.3 V | Fixed |
| GP1 | 78 | `UART0_RX` | UART0 | UART0 RX (F2) | In | 3.3 V | Fixed |
| GP2 | 79 | `HAPTIC_EN` | Haptics | SIO | Out | 3.3 V | Provisional |
| GP3 | 80 | `HAPTIC_TRIG` | Haptics | SIO | Out | 3.3 V | Provisional |
| GP4 | 1 | `I2C0_SDA` | I2C0 | I2C0 SDA (F3) | Bidir | 3.3 V | Fixed |
| GP5 | 2 | `I2C0_SCL` | I2C0 | I2C0 SCL (F3) | Bidir | 3.3 V | Fixed |
| GP6 | 3 | `I2S_BCLK` | Audio | PIO | Out (in with Waveshare master) | 3.3 V | Fixed |
| GP7 | 4 | `I2S_LRCLK` | Audio | PIO | Out (in with Waveshare master) | 3.3 V | Fixed |
| GP8 | 6 | `I2S_DOUT` | Audio | PIO | Out | 3.3 V | Fixed |
| GP9 | 7 | `I2S_DIN` | Audio backup | PIO, backup module only | In | 3.3 V | Provisional |
| GP10 | 8 | `DAC_RST_N` | Audio | SIO | Out | 3.3 V | Provisional |
| GP11 | 9 | `HP_DET` | Audio | SIO, jack tip switch | In | 3.3 V | Provisional |
| GP12 | 11 | `EXP_GP12` | Expansion | Spare, HSTX bank | - | 3.3 V | Reserved |
| GP13 | 12 | `EXP_GP13` | Expansion | Spare, HSTX bank | - | 3.3 V | Reserved |
| GP14 | 13 | `EXP_GP14` | Expansion | Spare, HSTX bank | - | 3.3 V | Reserved |
| GP15 | 14 | `EXP_GP15` | Expansion | Spare, HSTX bank | - | 3.3 V | Reserved |
| GP16 | 16 | `EXP_GP16` | Expansion | Spare, HSTX bank | - | 3.3 V | Reserved |
| GP17 | 17 | `EXP_GP17` | Expansion | Spare, HSTX bank | - | 3.3 V | Reserved |
| GP18 | 18 | `EXP_GP18` | Expansion | Spare, HSTX bank | - | 3.3 V | Reserved |
| GP19 | 19 | `EXP_GP19` | Expansion | Spare, HSTX bank | - | 3.3 V | Reserved |
| GP20 | 20 | `SD_CD` | microSD | SIO | In | 3.3 V | Provisional |
| GP21 | 21 | `I2S_MCLK` | Audio | CLOCK GPOUT0, optional, link DNP | Out | 3.3 V | Provisional |
| GP22 | 22 | `BTN_MENU` | Buttons | SIO, active low | In | 3.3 V | Fixed |
| GP23 | 23 | `BTN_PLAY` | Buttons | SIO, active low | In | 3.3 V | Fixed |
| GP24 | 25 | `BTN_PREV` | Buttons | SIO, active low | In | 3.3 V | Fixed |
| GP25 | 26 | `BTN_NEXT` | Buttons | SIO, active low | In | 3.3 V | Fixed |
| GP26 | 27 | `BTN_SELECT` | Buttons | SIO, active low | In | 3.3 V | Fixed |
| GP27 | 28 | `TOUCH_IRQ` | Wheel | SIO, from module INT | In | 3.3 V | Fixed |
| GP28 | 36 | `SD_MISO` | microSD | SPI1 RX (F1) | In | 3.3 V | Fixed |
| GP29 | 37 | `SD_CS` | microSD | SPI1 CSn (F1) or SIO | Out | 3.3 V | Fixed |
| GP30 | 38 | `SD_SCK` | microSD | SPI1 SCK (F1) | Out | 3.3 V | Fixed |
| GP31 | 39 | `SD_MOSI` | microSD | SPI1 TX (F1) | Out | 3.3 V | Fixed |
| GP32 | 40 | `DISP_MISO` | Display | SPI0 RX (F1) | In | 3.3 V | Fixed |
| GP33 | 42 | `DISP_CS` | Display | SPI0 CSn (F1) or SIO | Out | 3.3 V | Fixed |
| GP34 | 43 | `DISP_SCK` | Display | SPI0 SCK (F1) | Out | 3.3 V | Fixed |
| GP35 | 44 | `DISP_MOSI` | Display | SPI0 TX (F1) | Out | 3.3 V | Fixed |
| GP36 | 45 | `DISP_DC` | Display | SIO | Out | 3.3 V | Fixed |
| GP37 | 46 | `DISP_RST` | Display | SIO | Out | 3.3 V | Fixed |
| GP38 | 47 | `DISP_BL` | Display | PWM11 A (F4) | Out | 3.3 V | Fixed |
| GP39 | 48 | `CHG_N` | Power | SIO, open-drain input | In | 3.3 V | Provisional |
| GP40 | 49 | `VBAT_SENSE` | Power | ADC0, 1:2 divider | Analog | 3.3 V | Provisional |
| GP41 | 52 | `VSYS_SENSE` | Power | ADC1, 1:2 divider | Analog | 3.3 V | Provisional |
| GP42 | 53 | `PGOOD_N` | Power | SIO, open-drain input | In | 3.3 V | Provisional |
| GP43 | 54 | `VBUS_SENSE` | Power | ADC3, 100k/47k divider | Analog | 3.3 V | Provisional |
| GP44 | 55 | `MUX_ST` | Power | SIO; low means battery path active | In | 3.3 V | Provisional |
| GP45 | 56 | `CHG_EN1` | Power | SIO; low 100 mA, high 500 mA | Out | 3.3 V | Provisional |
| GP46 | 57 | `CHG_CE_N` | Power | SIO; high disables charging | Out | 3.3 V | Provisional |
| GP47 | 58 | `STATUS_LED` | Debug | SIO; also QMI CS1n if ever needed | Out | 3.3 V | Provisional |

## Non-GPIO MCU pins

| Pin | Name | Connection |
|---|---|---|
| 5, 15, 24, 29, 41, 50, 60, 76 | IOVDD | `+3V3`, 100 nF each |
| 10, 32, 51 | DVDD | `+1V1`, 100 nF each |
| 59 | ADC_AVDD | `+3V3`, 100 nF |
| 61 | VREG_AVDD | `+3V3` through 33 ohm, 4.7 uF |
| 62 | VREG_PGND | `GND` |
| 63 | VREG_LX | 3.3 uH to `+1V1` |
| 64 | VREG_VIN | `+3V3`, 4.7 uF |
| 65 | VREG_FB | `+1V1` |
| 66, 67 | USB_DM, USB_DP | 27 ohm series, then USBLC6 and J1 |
| 68, 69 | USB_OTP_VDD, QSPI_IOVDD | `+3V3`, one shared 100 nF per the Raspberry Pi guide |
| 70-74 | QSPI_SD3, SCLK, SD0, SD2, SD1 | No connect; in-package flash uses these |
| 75 | QSPI_SS | 1k to BOOTSEL switch; no pull-up |
| 30, 31 | XIN, XOUT | Crystal; XOUT through 1k |
| 33, 34 | SWCLK, SWDIO | J14 |
| 35 | RUN | Reset switch and test point |
| 81 | Exposed pad | `GND` |

## Changes from the brief's proposed table

- Display moved to GP32-38, the pins the firmware already runs at 75 MHz. This also frees the HSTX bank, GP12-19.
- microSD moved to GP28-31 on SPI1. Both moves were approved by the team.
- GP9 is now the backup module's data input. MCLK moved to GP21, which has a real clock output; a PIO clock on GP9 could not produce audio rates exactly.
- Haptics moved to GP2/GP3 from GP28/GP29.
- After approval, four provisional or spare pins changed during detailed design: GP11 is `HP_DET` instead of DAC_INT; GP44 is `MUX_ST` instead of PWR_SW_SENSE (the slide switch turns the regulator off, so firmware could never read it); GP45 and GP46 now control the charger; GP47 drives a status LED.

## ADC note

On the QFN-80 only GP40-47 reach the ADC. GP26-29 do not, unlike the RP2040 and QFN-60 parts.
