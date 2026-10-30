/**
 * \file gfx_render.h
 * \brief Dirty-rectangle manager and strip renderer for the ILI9341.
 *
 * There is no framebuffer. The scene is redrawn on demand into one of two
 * 320 x GFX_STRIP_H RGB565 strip buffers (2 x 10 KB), and each band is pushed
 * to the panel with ili9341_blit_async().
 *
 * Typical frame:
 * \code
 * gfx_invalidate((gfx_rect_t){0, 40, 320, 24});
 * gfx_render(scene_draw, &scene);
 * // ... sleep until the next frame
 * \endcode
 *
 * The draw callback paints the WHOLE scene; it must paint every pixel of the
 * band it is given (clipping takes care of the rest). Everything outside
 * canvas->area is discarded, so the same function works for a 320 x 16 band, a
 * small dirty rect band, or the whole panel.
 *
 * Double-buffer invariant: ili9341_blit_async() waits for the previous transfer
 * before starting a new one, so while buffer N is on the wire the renderer is
 * free to write buffer N ^ 1. gfx_render() keeps the parity across calls and
 * never starts a transfer from the buffer that is still in flight. The final
 * transfer is deliberately NOT awaited; the next ili9341_blit_async() (or an
 * explicit ili9341_wait_idle()) completes it.
 */

#ifndef GFX_RENDER_H
#define GFX_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Height of one strip buffer in pixels. */
#define GFX_STRIP_H 16

/** Maximum number of tracked dirty rectangles. */
#define GFX_MAX_DIRTY 8

/**
 * \brief Scene drawing callback.
 *
 * Draws the complete scene into \p c, relying on the canvas clip. Must not
 * call gfx_invalidate() (the renderer clears the dirty list afterwards).
 *
 * \param c Band canvas; area == clip == the band being rendered.
 * \param user Opaque pointer from gfx_render().
 */
typedef void (*gfx_draw_fn)(gfx_canvas_t *c, void *user);

/**
 * \brief Initialise the renderer state (buffers, dirty list, statistics).
 *
 * Call once after ili9341_init(); it does not touch the panel.
 */
void gfx_render_init(void);

/**
 * \brief Mark a screen rectangle as needing a redraw.
 *
 * The rectangle is clamped to the panel. It is merged into an existing dirty
 * rect when the union costs at most 1.3x the two separate areas. When the list
 * is full, the pair whose union adds the least area is merged first.
 *
 * \param r Rectangle in screen coordinates; empty or off-screen rects are
 *          ignored.
 */
void gfx_invalidate(gfx_rect_t r);

/** \brief Mark the whole panel as needing a redraw. */
void gfx_invalidate_all(void);

/** \brief True when at least one dirty rectangle is pending. */
bool gfx_has_dirty(void);

/**
 * \brief Render every dirty rectangle and start pushing the bands to the panel.
 *
 * Each dirty rect is split into bands of at most GFX_STRIP_H rows (stride =
 * rect width). For every band the canvas area and clip are set to the band,
 * \p draw is called, and the band is handed to ili9341_blit_async(). Rendering
 * of the next band overlaps the DMA of the previous one. The dirty list is
 * cleared before returning and the last DMA transfer is left running.
 *
 * \param draw Scene callback; must not be NULL.
 * \param user Passed through to \p draw.
 */
void gfx_render(gfx_draw_fn draw, void *user);

/** Renderer statistics (since gfx_render_reset_stats()). */
typedef struct {
    uint32_t frames;         /**< gfx_render() calls that had work to do. */
    uint32_t rects;          /**< Dirty rectangles processed. */
    uint32_t bands;          /**< Bands rendered and queued for DMA. */
    uint32_t pixels;         /**< Pixels rendered into strip buffers. */
    uint32_t last_render_us; /**< Duration of the last gfx_render() call, us. */
    uint32_t max_render_us;  /**< Longest gfx_render() call seen, us. */
} gfx_render_stats_t;

/** \brief Copy the statistics. \param s Destination; must not be NULL. */
void gfx_render_get_stats(gfx_render_stats_t *s);

/** \brief Zero the statistics. */
void gfx_render_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* GFX_RENDER_H */
