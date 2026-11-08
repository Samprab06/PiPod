/*
 * Compile exactly one precomputed asset set.
 *
 * ui_layout.h resolves the panel size from GFX_SCREEN_W (gfx_config.h) and the
 * PIPOD_DISPLAY_ST7796 define, the same selection the renderer and display
 * driver use. Selecting here keeps CMake free of a second, possibly divergent
 * size switch and guarantees the asset geometry matches the layout constants
 * that lib/ui draws with.
 *
 * Both generated sets are `const` arrays in flash; only the chosen one is ever
 * compiled, so the image contains a single set (~351 KB at 320x240, ~717 KB at
 * 480x320).
 */

#include "ui_layout.h"

#if UI_LAYOUT_LARGE
#include "ui_assets_480.c"
#else
#include "ui_assets.c"
#endif
