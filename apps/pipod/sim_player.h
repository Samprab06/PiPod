/**
 * @file sim_player.h
 * @brief Fake audio engine for pipod.
 *
 * Runs on core 0: drains ui_poll_command(), maintains a play queue, advances
 * the position in real time and reports state back to the UI with the
 * ui_set_*() calls. There is no audio hardware: audio_load_sim fakes the
 * core-0 load instead.
 */

#ifndef SIM_PLAYER_H
#define SIM_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initial volume used by both the UI config and the fake engine. */
#define SIM_DEMO_INITIAL_VOLUME 60

/** Reset state and publish initial now-playing/volume/modes/battery values.
 *  Call after ui_init() has run on the UI core. */
void sim_player_init(void);

/** Service UI commands and advance playback. Call every few ms on core 0. */
void sim_player_update(void);

/** Print one compact "[player] ..." status line (queue position, track, time).
 *  Used by the periodic report so track changes do not stall the UART in the
 *  middle of an audio-jitter measurement window. */
void sim_player_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* SIM_PLAYER_H */
