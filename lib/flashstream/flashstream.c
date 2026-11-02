/*
 * Arbitration for the single RP2350 XIP streaming unit.
 *
 * One hardware spin lock (claimed lazily so it works before multicore start)
 * protects two fields: the current owner and the "audio is waiting" flag. The
 * critical sections are a handful of instructions, so the lock is never held
 * across a DMA transfer: the display drops it between chunks, see
 * ili9341_blit_flash_async().
 */

#include "flashstream.h"

#include <stddef.h>

#include "hardware/sync.h"
#include "pico/stdlib.h"

/* spin_lock_init() / spin_lock_instance() live in hardware/sync.h on both
 * rp2040 and rp2350; a NULL lock means flashstream_init() has not run yet. */
static spin_lock_t *s_lock;

static volatile flashstream_owner_t s_owner = FLASHSTREAM_OWNER_NONE;
static volatile bool s_audio_pending;

void flashstream_init(void) {
    if (s_lock != NULL) {
        return;
    }
    const uint lock_num = spin_lock_claim_unused(true);
    s_lock = spin_lock_instance(lock_num);
    s_owner = FLASHSTREAM_OWNER_NONE;
    s_audio_pending = false;
}

static bool claim_locked(flashstream_owner_t owner) {
    switch (owner) {
        case FLASHSTREAM_OWNER_AUDIO:
            /* Audio priority: nothing outranks it except an owner in the
             * middle of a transfer, which never holds the lock across one. */
            if (s_owner != FLASHSTREAM_OWNER_NONE) {
                return false;
            }
            s_owner = owner;
            return true;
        case FLASHSTREAM_OWNER_DISPLAY:
            /* Yield to a waiting audio user even when the unit looks free:
             * it may be about to poll again, and it must not starve. */
            if (s_owner != FLASHSTREAM_OWNER_NONE || s_audio_pending) {
                return false;
            }
            s_owner = owner;
            return true;
        default:
            return false;
    }
}

bool flashstream_try_claim(flashstream_owner_t owner) {
    if (s_lock == NULL || owner == FLASHSTREAM_OWNER_NONE) {
        return false;
    }
    const uint32_t save = spin_lock_blocking(s_lock);
    const bool ok = claim_locked(owner);
    spin_unlock(s_lock, save);
    return ok;
}

void flashstream_release(flashstream_owner_t owner) {
    if (s_lock == NULL || owner == FLASHSTREAM_OWNER_NONE) {
        return;
    }
    const uint32_t save = spin_lock_blocking(s_lock);
    if (s_owner == owner) {
        s_owner = FLASHSTREAM_OWNER_NONE;
    }
    spin_unlock(s_lock, save);
}

bool flashstream_busy(void) {
    if (s_lock == NULL) {
        return false;
    }
    const uint32_t save = spin_lock_blocking(s_lock);
    const bool busy = (s_owner != FLASHSTREAM_OWNER_NONE);
    spin_unlock(s_lock, save);
    return busy;
}

flashstream_owner_t flashstream_owner(void) {
    if (s_lock == NULL) {
        return FLASHSTREAM_OWNER_NONE;
    }
    const uint32_t save = spin_lock_blocking(s_lock);
    const flashstream_owner_t owner = s_owner;
    spin_unlock(s_lock, save);
    return owner;
}

void flashstream_audio_request(bool pending) {
    if (s_lock == NULL) {
        return;
    }
    const uint32_t save = spin_lock_blocking(s_lock);
    s_audio_pending = pending;
    spin_unlock(s_lock, save);
}

bool flashstream_audio_pending(void) {
    if (s_lock == NULL) {
        return false;
    }
    const uint32_t save = spin_lock_blocking(s_lock);
    const bool pending = s_audio_pending;
    spin_unlock(s_lock, save);
    return pending;
}
