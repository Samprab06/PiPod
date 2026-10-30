/*
 * Image format for lib/gfx.
 *
 * Images are plain RGB565, row-major, native uint16_t, usually `const` in
 * flash. No palettes, no alpha channel: images are always drawn opaque and
 * are clipped by the canvas.
 */

#ifndef GFX_IMAGE_H
#define GFX_IMAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** An RGB565 image: width x height pixels, row-major, no padding. */
typedef struct {
    uint16_t width, height;   /**< Size in pixels. */
    const uint16_t *pixels;   /**< width * height RGB565 pixels, row-major. */
} gfx_image_t;

#ifdef __cplusplus
}
#endif

#endif /* GFX_IMAGE_H */
