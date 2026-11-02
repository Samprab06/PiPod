/**
 * \file flashstream.h
 * \brief Arbitration for the single RP2350 XIP streaming unit.
 *
 * The RP2350 has exactly one XIP stream unit (XIP_CTRL STREAM_ADDR /
 * STREAM_CTR, FIFO read through the XIP_AUX port, DREQ_XIP_STREAM, datasheet
 * 4.4.3). It streams flash in the background without stalling code fetches, so
 * it is worth sharing, but two users must never program it at the same time.
 *
 * Users:
 *   - FLASHSTREAM_OWNER_DISPLAY: lib/display streams precomputed UI assets and
 *     album art from flash to the panel (one blit at a time).
 *   - FLASHSTREAM_OWNER_AUDIO: the audio path streams flash data into a decoder
 *     or an I2S feed.
 *
 * Priority: AUDIO wins. The display only holds the unit for one blit and
 * releases it immediately (ili9341_blit_flash_async() is fully synchronous).
 * Long display blits are split into chunks of at most 8 KB of pixel data; at
 * every chunk boundary the driver checks flashstream_audio_pending() and, when
 * audio is waiting, releases the unit and yields (re-claiming it afterwards).
 * Audio therefore waits at most one chunk: 8 KB at 75 MHz SPI is about 0.9 ms.
 *
 * Threading: flashstream_init() must run once at boot, before the second core
 * is started. try_claim()/release()/busy() are then safe from any core and
 * from interrupt context (a hardware spin lock protects them); none of them
 * block.
 */

#ifndef FLASHSTREAM_H
#define FLASHSTREAM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Current owner of the XIP stream unit. */
typedef enum {
    FLASHSTREAM_OWNER_NONE = 0, /**< Unit is free. */
    FLASHSTREAM_OWNER_DISPLAY = 1, /**< lib/display (flash -> panel). */
    FLASHSTREAM_OWNER_AUDIO = 2, /**< audio / decoder path. */
} flashstream_owner_t;

/**
 * \brief Claim the hardware spin lock used by the arbiter.
 *
 * Idempotent. Call once before any other flashstream function (ili9341_init()
 * calls it too).
 */
void flashstream_init(void);

/**
 * \brief Try to become the owner of the stream unit. Never blocks.
 *
 * Fails when the unit is already owned, and for FLASHSTREAM_OWNER_DISPLAY also
 * when an audio request is pending (audio priority). A second claim by the
 * current owner fails as well: the ownership is not recursive.
 *
 * \param owner Requesting owner; FLASHSTREAM_OWNER_NONE is invalid.
 * \return true when the caller now owns the unit.
 */
bool flashstream_try_claim(flashstream_owner_t owner);

/**
 * \brief Release the unit if \p owner is the current owner.
 *
 * Releasing with the wrong owner (or when free) is a no-op.
 *
 * \param owner Owner that is done with the unit.
 */
void flashstream_release(flashstream_owner_t owner);

/**
 * \brief True while the unit is owned by anybody.
 *
 * \return true when a stream is (or may be) programmed.
 */
bool flashstream_busy(void);

/**
 * \brief Current owner (FLASHSTREAM_OWNER_NONE when free).
 */
flashstream_owner_t flashstream_owner(void);

/**
 * \brief Tell the arbiter whether audio is waiting for the unit.
 *
 * The audio side sets this before it starts polling try_claim(AUDIO) and
 * clears it once it owns the unit (or gives up). The display reads it between
 * chunks to yield early. Any core, non-blocking.
 *
 * \param pending true while audio waits for the unit.
 */
void flashstream_audio_request(bool pending);

/**
 * \brief True while audio has signalled that it waits for the unit.
 */
bool flashstream_audio_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* FLASHSTREAM_H */
