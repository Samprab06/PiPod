/**
 * @file sim_library.c
 * @brief Fictional, deterministic music library served from const tables.
 */

#include "sim_library.h"

#include <stdio.h>
#include <string.h>

#include "demo_art.h"

/* --- static data ------------------------------------------------------- */

static const char *const k_artists[SIM_ARTIST_COUNT] = {
    "Aurelia Vane",
    "The Lantern Bells",
    "Kestrel North",
    "Harborline",
    "Mira Solstice",
    "Meridian Glass",
    "Juniper Static",
    "Atlas Hollow",
};

typedef struct {
    uint8_t  artist;     /**< index into k_artists */
    const char *title;
    uint8_t  songs;
    uint16_t first_song; /**< global song offset */
} sim_album_t;

static const sim_album_t k_albums[SIM_ALBUM_COUNT] = {
    {0, "Amber Hours",      12,   0},
    {0, "Coastline Static", 11,  12},
    {1, "Deep Field",       13,  23},
    {1, "Slow Orbit",       10,  36},
    {2, "Paper Geometries", 14,  46},
    {2, "Ember Sketches",   12,  60},
    {3, "Neon Grid",        11,  72},
    {3, "Midnight Transit", 13,  83},
    {4, "Aurora Lines",     10,  96},
    {4, "Prism Weather",    12, 106},
    {5, "Paper Moons",      14, 118},
    {5, "Quiet Machines",   11, 132},
    {6, "Static Bloom",     13, 143},
    {7, "Northern Rooms",   10, 156},
};

static const char *const k_playlists[SIM_PLAYLIST_COUNT] = {
    "Late Night Drive",
    "Focus Flow",
    "Sunday Kitchen",
};

static const uint8_t k_playlist_songs[SIM_PLAYLIST_COUNT] = {16, 20, 12};

/* Song titles are adjective + noun pairs derived from the song position, so
 * all 166 titles exist without storing 166 strings. Sizes are coprime with
 * the multipliers used below, which keeps titles unique within an album. */
#define WORD_COUNT 24
static const char *const k_word_a[WORD_COUNT] = {
    "Amber",    "Silver",  "Golden",   "Paper",    "Velvet",   "Neon",
    "Quiet",    "Northern","Electric", "Ocean",    "Little",   "Fading",
    "Crystal",  "Slow",    "Wild",     "Soft",     "Hidden",   "Burning",
    "Distant",  "Heavy",   "Gentle",   "Midnight", "Copper",   "Hollow",
};
static const char *const k_word_b[WORD_COUNT] = {
    "Hours",    "Rivers",  "Signals",   "Gardens",  "Trains",   "Moons",
    "Rooms",    "Tides",   "Sparks",    "Letters",  "Windows",  "Fields",
    "Echoes",   "Lines",   "Machines",  "Islands",  "Lanterns", "Static",
    "Weather",  "Roads",   "Dreams",    "Cities",   "Harbors",  "Bells",
};

/* --- helpers ----------------------------------------------------------- */

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (cap == 0u) {
        return;
    }
    while (src[i] != '\0' && i + 1u < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static uint32_t song_duration_ms(uint32_t song_id) {
    /* 2:30 .. 6:00, deterministic per song. */
    return 150000u + (song_id * 7919u) % 210001u;
}

static bool album_from_id(uint32_t album_id, uint32_t *album_index) {
    if (album_id < SIM_FIRST_ALBUM_ID || album_id >= SIM_FIRST_ALBUM_ID + SIM_ALBUM_COUNT) {
        return false;
    }
    *album_index = album_id - SIM_FIRST_ALBUM_ID;
    return true;
}

static uint32_t artist_album_count(uint32_t artist_id) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SIM_ALBUM_COUNT; i++) {
        if ((uint32_t)k_albums[i].artist + SIM_FIRST_ARTIST_ID == artist_id) {
            n++;
        }
    }
    return n;
}

/** Album index of the @p ordinal-th album by @p artist_id, or SIM_ALBUM_COUNT. */
static uint32_t artist_album_index(uint32_t artist_id, uint32_t ordinal) {
    for (uint32_t i = 0; i < SIM_ALBUM_COUNT; i++) {
        if ((uint32_t)k_albums[i].artist + SIM_FIRST_ARTIST_ID != artist_id) {
            continue;
        }
        if (ordinal == 0) {
            return i;
        }
        ordinal--;
    }
    return SIM_ALBUM_COUNT;
}

/** Global song index -> (album index, offset within album). */
static bool global_song(uint32_t global_index, uint32_t *album_index, uint32_t *offset) {
    for (uint32_t a = 0; a < SIM_ALBUM_COUNT; a++) {
        uint32_t first = k_albums[a].first_song;
        if (global_index >= first && global_index < first + k_albums[a].songs) {
            *album_index = a;
            *offset = global_index - first;
            return true;
        }
    }
    return false;
}

static uint32_t playlist_song_count(uint32_t playlist_id) {
    if (playlist_id < SIM_FIRST_PLAYLIST_ID ||
        playlist_id >= SIM_FIRST_PLAYLIST_ID + SIM_PLAYLIST_COUNT) {
        return 0;
    }
    return k_playlist_songs[playlist_id - SIM_FIRST_PLAYLIST_ID];
}

/** Deterministic, duplicate-free playlist selection (37 is coprime to 166). */
static uint32_t playlist_song_id(uint32_t playlist_id, uint32_t index) {
    uint32_t p = playlist_id - SIM_FIRST_PLAYLIST_ID;
    uint32_t n = playlist_song_count(playlist_id);
    if (index >= n) {
        return 0;
    }
    return SIM_FIRST_SONG_ID + (index * 37u + p * 53u + 11u) % SIM_SONG_COUNT;
}

static void song_title(uint32_t global_index, char *dst, size_t cap) {
    uint32_t album = 0;
    uint32_t offset = 0;
    if (!global_song(global_index, &album, &offset)) {
        copy_str(dst, cap, "Unknown");
        return;
    }
    const char *a = k_word_a[(offset * 5u + album * 3u) % WORD_COUNT];
    const char *b = k_word_b[(offset * 7u + album * 11u + 5u) % WORD_COUNT];
    size_t used = 0;
    if (cap == 0u) {
        return;
    }
    while (*a != '\0' && used + 1u < cap) {
        dst[used++] = *a++;
    }
    if (used + 1u < cap) {
        dst[used++] = ' ';
    }
    while (*b != '\0' && used + 1u < cap) {
        dst[used++] = *b++;
    }
    dst[used] = '\0';
}

static void fill_song_item(uint32_t global_index, ui_list_item_t *out) {
    uint32_t album = 0;
    uint32_t offset = 0;
    if (!global_song(global_index, &album, &offset)) {
        return;
    }
    out->id = SIM_FIRST_SONG_ID + global_index;
    song_title(global_index, out->title, sizeof(out->title));
    copy_str(out->subtitle, sizeof(out->subtitle), k_artists[k_albums[album].artist]);
}

/* --- ui_library_t implementation --------------------------------------- */

static uint32_t sim_count(ui_list_kind_t list, uint32_t parent_id, void *ctx) {
    (void)ctx;
    switch (list) {
        case UI_LIST_ARTISTS:
            return SIM_ARTIST_COUNT;

        case UI_LIST_ALBUMS:
            if (parent_id == UI_ID_ALL) {
                return SIM_ALBUM_COUNT;
            }
            return artist_album_count(parent_id);

        case UI_LIST_SONGS:
            if (parent_id == UI_ID_ALL) {
                return SIM_SONG_COUNT;
            }
            {
                uint32_t album = 0;
                if (!album_from_id(parent_id, &album)) {
                    return 0;
                }
                return k_albums[album].songs;
            }

        case UI_LIST_PLAYLISTS:
            return SIM_PLAYLIST_COUNT;

        case UI_LIST_PLAYLIST_SONGS:
            return playlist_song_count(parent_id);

        default:
            return 0;
    }
}

static bool sim_get_item(ui_list_kind_t list, uint32_t parent_id, uint32_t index,
                         ui_list_item_t *out, void *ctx) {
    (void)ctx;
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    switch (list) {
        case UI_LIST_ARTISTS: {
            if (index >= SIM_ARTIST_COUNT) {
                return false;
            }
            out->id = SIM_FIRST_ARTIST_ID + index;
            copy_str(out->title, sizeof(out->title), k_artists[index]);
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%u albums", (unsigned)artist_album_count(out->id));
            copy_str(out->subtitle, sizeof(out->subtitle), tmp);
            return true;
        }

        case UI_LIST_ALBUMS: {
            uint32_t album;
            if (parent_id == UI_ID_ALL) {
                album = index;
            } else {
                album = artist_album_index(parent_id, index);
            }
            if (album >= SIM_ALBUM_COUNT) {
                return false;
            }
            out->id = SIM_FIRST_ALBUM_ID + album;
            copy_str(out->title, sizeof(out->title), k_albums[album].title);
            copy_str(out->subtitle, sizeof(out->subtitle), k_artists[k_albums[album].artist]);
            return true;
        }

        case UI_LIST_SONGS: {
            uint32_t global;
            if (parent_id == UI_ID_ALL) {
                global = index;
            } else {
                uint32_t album = 0;
                if (!album_from_id(parent_id, &album) || index >= k_albums[album].songs) {
                    return false;
                }
                global = k_albums[album].first_song + index;
            }
            if (global >= SIM_SONG_COUNT) {
                return false;
            }
            fill_song_item(global, out);
            return true;
        }

        case UI_LIST_PLAYLISTS: {
            if (index >= SIM_PLAYLIST_COUNT) {
                return false;
            }
            out->id = SIM_FIRST_PLAYLIST_ID + index;
            copy_str(out->title, sizeof(out->title), k_playlists[index]);
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%u songs", (unsigned)k_playlist_songs[index]);
            copy_str(out->subtitle, sizeof(out->subtitle), tmp);
            return true;
        }

        case UI_LIST_PLAYLIST_SONGS: {
            uint32_t song_id = playlist_song_id(parent_id, index);
            if (song_id == 0u) {
                return false;
            }
            fill_song_item(song_id - SIM_FIRST_SONG_ID, out);
            return true;
        }

        default:
            return false;
    }
}

static const gfx_image_t *sim_get_album_art(uint32_t album_id, void *ctx) {
    (void)ctx;
    uint32_t album = 0;
    if (!album_from_id(album_id, &album)) {
        return NULL;
    }
    return demo_art[(album + SIM_FIRST_ALBUM_ID) % DEMO_ART_COUNT];
}

static const ui_library_t k_sim_library = {
    .count = sim_count,
    .get_item = sim_get_item,
    .get_album_art = sim_get_album_art,
    .ctx = NULL,
};

const ui_library_t *sim_library_get(void) {
    return &k_sim_library;
}

/* --- helpers for the fake audio engine -------------------------------- */

uint32_t sim_song_count(ui_list_kind_t list, uint32_t parent_id) {
    return sim_count(list, parent_id, NULL);
}

uint32_t sim_song_id_at(ui_list_kind_t list, uint32_t parent_id, uint32_t index) {
    switch (list) {
        case UI_LIST_SONGS: {
            uint32_t global;
            if (parent_id == UI_ID_ALL) {
                global = index;
            } else {
                uint32_t album = 0;
                if (!album_from_id(parent_id, &album) || index >= k_albums[album].songs) {
                    return 0;
                }
                global = k_albums[album].first_song + index;
            }
            if (global >= SIM_SONG_COUNT) {
                return 0;
            }
            return SIM_FIRST_SONG_ID + global;
        }

        case UI_LIST_PLAYLIST_SONGS:
            return playlist_song_id(parent_id, index);

        default:
            return 0;
    }
}

bool sim_song_info(uint32_t song_id, ui_track_info_t *out) {
    if (out == NULL || song_id < SIM_FIRST_SONG_ID ||
        song_id >= SIM_FIRST_SONG_ID + SIM_SONG_COUNT) {
        return false;
    }
    uint32_t global = song_id - SIM_FIRST_SONG_ID;
    uint32_t album = 0;
    uint32_t offset = 0;
    if (!global_song(global, &album, &offset)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->track_id = song_id;
    out->album_id = SIM_FIRST_ALBUM_ID + album;
    song_title(global, out->title, sizeof(out->title));
    copy_str(out->artist, sizeof(out->artist), k_artists[k_albums[album].artist]);
    copy_str(out->album, sizeof(out->album), k_albums[album].title);
    out->duration_ms = song_duration_ms(song_id);
    return true;
}
