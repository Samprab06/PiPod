/*
 * Board header for the Purdue Proton (RP2350B, QFN-80, 48 GPIOs).
 *
 * Based on pico2.h from the Raspberry Pi Pico SDK
 * (Copyright (c) 2020 Raspberry Pi (Trading) Ltd., SPDX-License-Identifier: BSD-3-Clause).
 *
 * NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
 *       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
 */

#ifndef _BOARDS_PROTON_H
#define _BOARDS_PROTON_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define PURDUE_PROTON 1

// --- RP2350 VARIANT ---
// RP2350B: QFN-80 package, 48 GPIOs, 520 KB SRAM.
#define PICO_RP2350A 0

// --- UART ---
// GP0/GP1 go to the on-board debug probe.
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED ---
// GP22..GP25 are the R, Y, G, B LEDs; default to the red one.
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 22
#endif
// no PICO_DEFAULT_WS2812_PIN
// no PICO_SMPS_MODE_PIN (GP23 is an LED on this board)

// --- XOSC ---
// 12 MHz crystal. XOSC_HZ defaults to 12 MHz in the SDK; keep the SDK
// start-up delay multiplier explicit for slow crystals.
#ifndef PICO_XOSC_STARTUP_DELAY_MULTIPLIER
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64
#endif

// --- FLASH ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
