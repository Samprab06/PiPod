/**
 * @file sim_library.h
 * @brief Deterministic simulated music library for the UI demo.
 *
 * Everything lives in const tables or is computed from the item id on the fly,
 * so the whole library costs a few hundred bytes of RAM. Names are fictional.
 *
 * Ids:
 *   artists   1..8        albums   101..114
 *   songs     1001..1166  playlists 201..203
 */

#ifndef SIM_LIBRARY_H
#define SIM_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_ARTIST_COUNT   8
#define SIM_ALBUM_COUNT    14
#define SIM_SONG_COUNT     166
#define SIM_PLAYLIST_COUNT 3

#define SIM_FIRST_ARTIST_ID   1u
#define SIM_FIRST_ALBUM_ID    101u
#define SIM_FIRST_SONG_ID     1001u
#define SIM_FIRST_PLAYLIST_ID 201u

/** Library provider to hand to ui_init(). */
const ui_library_t *sim_library_get(void);

/** Number of songs in a song-producing list scope (SONGS or PLAYLIST_SONGS). */
uint32_t sim_song_count(ui_list_kind_t list, uint32_t parent_id);

/** Song id at @p index in a song-producing list, or 0 if the index is bad. */
uint32_t sim_song_id_at(ui_list_kind_t list, uint32_t parent_id, uint32_t index);

/** Fill track metadata for a song id. queue_index/queue_length are left 0. */
bool sim_song_info(uint32_t song_id, ui_track_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SIM_LIBRARY_H */
