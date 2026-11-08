/*
 * Screen geometry for lib/gfx.
 *
 * The renderer is size-agnostic: strip buffers and all dirty-rect clamping use
 * GFX_SCREEN_W x GFX_SCREEN_H from here, not a hard-coded panel size. The
 * default follows the compile-time panel selection (PIPOD_DISPLAY_ST7796),
 * which must match the define used by lib/display/display.h.
 *
 * Override both macros before including this header to build for another
 * panel; the strip buffers are sized from GFX_SCREEN_W.
 */

#ifndef GFX_CONFIG_H
#define GFX_CONFIG_H

#ifndef PIPOD_DISPLAY_ST7796
#define PIPOD_DISPLAY_ST7796 0
#endif

#if PIPOD_DISPLAY_ST7796
/** Landscape width of the ST7796 module in pixels. */
#define GFX_SCREEN_W 480
/** Landscape height of the ST7796 module in pixels. */
#define GFX_SCREEN_H 320
#else
/** Landscape width of the ILI9341 panel in pixels. */
#define GFX_SCREEN_W 320
/** Landscape height of the ILI9341 panel in pixels. */
#define GFX_SCREEN_H 240
#endif

#endif /* GFX_CONFIG_H */
