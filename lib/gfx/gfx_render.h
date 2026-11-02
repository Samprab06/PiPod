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
#define GFX_MAX_DIRTY 16

/** Maximum number of direct layers per frame. */
#define GFX_MAX_DIRECT 10

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

/* ------------------------------------------------------------------------- */
/* Direct layers (flash -> panel DMA)                                        */
/* ------------------------------------------------------------------------- */

/**
 * \brief Start a new frame's direct layer list.
 *
 * Call once before gfx_direct_add() for every frame. The list is independent
 * of the dirty list: layers describe what the scene *would* draw, and the
 * renderer decides which dirty rects can be served by DMA instead.
 */
void gfx_direct_begin_frame(void);

/**
 * \brief Declare that the scene wants \p pixels drawn opaquely at \p r.
 *
 * The layer is only used for a dirty rect that shares the layer's x and width
 * exactly and lies entirely inside its vertical extent; that rect is then sent
 * with ili9341_blit_flash_async() (XIP stream unit) instead of being rendered
 * into strip buffers. A vertical sub-range is handled by offsetting the pixel
 * pointer by rows * stride_px, so partial rows at the top/bottom edge work.
 *
 * Nothing may be drawn on top of a direct layer: the renderer skips the draw
 * callback for the whole rect, so any decoration (frames, badges, text over
 * the image) would have to be part of the layer pixels or fall back to the
 * strip path. Horizontal clipping (a dirty rect narrower or wider than the
 * layer) always falls back to strips.
 *
 * A layer whose pixels are not in XIP flash still works: the driver falls
 * back to plain DMA, which is no faster than a strip blit but equally correct.
 *
 * \param r Screen rectangle the image occupies (width == stride_px).
 * \param pixels w*h RGB565 pixels, row-major, at \p r.
 * \param stride_px Row stride in pixels; must be >= r.w.
 * \return false when the rect is invalid or the list is full.
 */
bool gfx_direct_add(gfx_rect_t r, const uint16_t *pixels, uint16_t stride_px);

/**
 * \brief Enable or disable the direct path at runtime.
 *
 * Disabled, every dirty rect takes the strip path; the pixels are identical,
 * which is what the host simulator uses to verify that precomputed assets and
 * scene drawing agree. Enabled by default.
 *
 * \param on true to use direct layers.
 */
void gfx_direct_set_enabled(bool on);

/**
 * \brief True when direct layers are honoured.
 */
bool gfx_direct_enabled(void);

/* ------------------------------------------------------------------------- */
/* Dirty rectangles and rendering                                            */
/* ------------------------------------------------------------------------- */

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

/**
 * \brief Mark a rectangle without merging it into the existing dirty rects.
 *
 * The list entry is kept separate (unless the list is full, in which case the
 * normal merge path applies). Use it for rectangles that are expected to match
 * a direct layer exactly: merging them with a neighbour would make the union
 * no longer covered by the layer and silently disable the DMA path. Drawing is
 * unaffected either way.
 *
 * \param r Rectangle in screen coordinates; empty or off-screen rects are
 *          ignored.
 */
void gfx_invalidate_exact(gfx_rect_t r);

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
 * Each dirty rect that is exactly covered by a direct layer (same x/width,
 * inside its vertical extent, direct path enabled) is sent in one
 * ili9341_blit_flash_async() call and skipped by the draw callback. Every other
 * rect is split into bands of at most GFX_STRIP_H rows (stride = rect width);
 * for each band the canvas area and clip are set and \p draw is called, then
 * the band is handed to ili9341_blit_async(). Direct blits are synchronous and
 * strip blits are awaited by the next transfer, so the two kinds never overlap
 * on the SPI bus. The dirty list is cleared before returning and the last
 * strip DMA transfer is left running.
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
    uint32_t pixels;         /**< Pixels pushed to the panel (strip + direct). */
    uint32_t last_render_us; /**< Duration of the last gfx_render() call, us. */
    uint32_t max_render_us;  /**< Longest gfx_render() call seen, us. */
    uint32_t direct_pixels;  /**< Pixels sent through a direct layer (flash DMA). */
    uint32_t strip_pixels;   /**< Pixels rendered into strip buffers. */
    uint32_t direct_blits;   /**< Direct rects sent by ili9341_blit_flash_async(). */
    uint32_t direct_fallbacks; /**< Direct rects the driver refused (stream busy). */
} gfx_render_stats_t;

/** \brief Copy the statistics. \param s Destination; must not be NULL. */
void gfx_render_get_stats(gfx_render_stats_t *s);

/** \brief Zero the statistics. */
void gfx_render_reset_stats(void);

/**
 * \brief Restore a statistics snapshot.
 *
 * Diagnostic hook for host tools that re-render the same frame: the simulator
 * compares the direct path with a strip-only pass and then restores the
 * counters so the per-frame statistics stay meaningful.
 *
 * \param s Snapshot to copy back; must not be NULL.
 */
void gfx_render_set_stats(const gfx_render_stats_t *s);

#ifdef __cplusplus
}
#endif

#endif /* GFX_RENDER_H */
