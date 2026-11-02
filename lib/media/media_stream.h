/**
 * @file media_stream.h
 * @brief Audio integration contract: how the decoder reads a track.
 *
 * This header is documentation plus a few constants; it contains no code. It
 * is the agreement between lib/media (this workstream) and the audio
 * workstream (decoder + I2S + WM8960). Audio playback itself is NOT part of
 * this workstream: pipod runs a simulated player, and nothing here is wired to
 * an I2S peripheral yet.
 *
 * ---------------------------------------------------------------------------
 * 1. Compressed input
 * ---------------------------------------------------------------------------
 * media_library_track() (media_library.h) fills a media_track_info_t whose
 * `source` is a media_source_t over exactly the audio payload of the file:
 *
 *   source.size      == audio_size (bytes of compressed/payload audio)
 *   source.xip_base  == first audio byte in memory-mapped flash (or NULL when
 *                       the backend has no contiguous mapping)
 *   source.read()    == memcpy helper for the same range
 *
 * The pack lives in the 14 MB media region at MEDIA_PACK_OFFSET and is
 * memory-mapped, so a decoder should:
 *   - use source.xip_base + offset for 32-bit aligned bulk reads and DMA;
 *   - use source.read() only for small headers (MP3 frame header, FLAC frame
 *     CRC) or when xip_base is NULL (a future SD card backend);
 *   - never assume the payload starts at file offset 0 (tags/headers precede
 *     it) and never read past source.size.
 *
 * The source is valid until the next media_library_track() call on the same
 * library (the library owns one audio view), so copy base/size into the
 * decoder state.
 *
 * ---------------------------------------------------------------------------
 * 2. XIP streaming unit
 * ---------------------------------------------------------------------------
 * Flash -> RAM bulk transfers should go through the RP2350 XIP streaming unit
 * (lib/flashstream, datasheet 4.4.3) exactly like the display path:
 *
 *   flashstream_audio_request(true);              // tell the display to yield
 *   while (!flashstream_try_claim(FLASHSTREAM_OWNER_AUDIO)) {
 *       sleep_us(50);                              // display owns it for <= 1 chunk
 *   }
 *   ... program DREQ_XIP_STREAM against source.xip_base + offset ...
 *   flashstream_release(FLASHSTREAM_OWNER_AUDIO);
 *   flashstream_audio_request(false);
 *
 * Audio has priority: lib/display checks flashstream_audio_pending() between
 * 8 KB chunks and releases the unit, so the wait is bounded (about 0.9 ms at
 * 75 MHz SPI). Never program xip_ctrl_hw directly, and never hold the unit
 * while the decoder is not transferring.
 *
 * CPU memcpy from XIP is fine for small reads; it stalls code fetch on both
 * cores only while the copy runs. Do not do it in an I2S DMA IRQ.
 *
 * ---------------------------------------------------------------------------
 * 3. Buffering and DMA
 * ---------------------------------------------------------------------------
 * - Decoder input: at least two buffers of >= 4.6 KB each for MP3 (one
 *   MPEG-1 Layer III frame is at most 1441 bytes, but decoders want a window;
 *   the task's minimum is 2 x 4.6 KB per MP3 frame). Size the pair so a full
 *   frame plus the next frame header always fits while DMA refills the other
 *   half. FLAC frame sizes vary; use the same >= 4.6 KB pairs and a frame
 *   header parser to know how much to fetch.
 * - PCM output: ping-pong DMA into the PIO I2S TX FIFO (or a PIO program that
 *   shifts 16-bit stereo samples). One DMA channel per half-buffer; refill the
 *   half the DMA just finished from the decoder. Keep the IRQ on core 0.
 * - Never write flash during playback: an erase/program stalls all XIP,
 *   including the decoder's input and the UI's code. The library index and
 *   covers are written only during the boot scan (see media_library.h).
 *
 * ---------------------------------------------------------------------------
 * 4. Clocks / WM8960 wiring recommendation
 * ---------------------------------------------------------------------------
 * The board has a 12 MHz crystal and clk_sys = 150 MHz. Exact audio clocks:
 *
 *   44.1 kHz family: MCLK = 11.2896 MHz = 256 fs
 *   48 kHz family:   MCLK = 12.288 MHz   = 256 fs
 *
 * Neither is an integer multiple of 12 MHz, and the RP2350 PLLs are integer-N
 * from the 12 MHz crystal, so the RP2350 cannot generate them exactly. Prefer:
 *
 *   1. WM8960 as I2S *master* from its own crystal/PLL (if the board/module
 *      has one): the codec drives BCLK/LRCLK/MCLK, the RP2350 is I2S slave and
 *      there is no jitter from fractional PIO clock dividers. This is the
 *      recommendation.
 *   2. Otherwise the RP2350 provides MCLK through a clock GPOUT pin and the
 *      codec's PLL derives the exact rates. Route MCLK on GP13 or GP15 (both
 *      free, adjacent to the proposed I2S pins). Do NOT use GP21/GP26 (the
 *      on-board SELECT/MENU buttons) or GP22..GP25 (the on-board LEDs).
 *      Proposed I2S pins are GP10..GP12 (see docs/wiring.md); MCLK on GP13 or
 *      GP15 leaves GP14/GP16..GP20 free.
 *
 * If the codec must slave to a fractional PIO divider instead, expect a small
 * rate error (e.g. 44100 Hz from a 150 MHz / integer divider is not exact);
 * the UI does not care, but long recordings/gapless playback will drift.
 *
 * ---------------------------------------------------------------------------
 * 5. Minimal integration sequence
 * ---------------------------------------------------------------------------
 * @code
 *   media_track_info_t ti;
 *   if (!media_library_track(&lib, track_id, &ti)) return;
 *   decoder_open(&ti.source, ti.format, ti.sample_rate, ti.channels);
 *   ui_set_now_playing(&track);          // title/artist/album/duration_ms
 *   ui_set_play_state(UI_PLAY_PLAYING);
 *
 *   for (;;) {
 *       ui_command_t cmd;
 *       while (ui_poll_command(&cmd)) { ... }   // PLAY_LIST, SEEK, volume, ...
 *       decoder_fill(&ti.source);               // XIP -> input pair -> PCM
 *       i2s_service();                          // ping-pong PCM DMA
 *       ui_set_position(decoder_position_ms());
 *       sleep_us(500);
 *   }
 * @endcode
 *
 * Command handling, queue semantics and the meaning of each UI_CMD_* are in
 * docs/api.md section 3; reporting state is section 4.
 */
#ifndef MEDIA_STREAM_H
#define MEDIA_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

/** Minimum decoder input buffer size per MP3 frame (bytes). */
#define MEDIA_STREAM_MP3_FRAME_MIN 4710u
/** Number of input buffers (ping-pong) the decoder should keep. */
#define MEDIA_STREAM_INPUT_BUFFERS 2u

/** MCLK for the 44.1 kHz family (256 fs), Hz. */
#define MEDIA_STREAM_MCLK_44K1 11289600u
/** MCLK for the 48 kHz family (256 fs), Hz. */
#define MEDIA_STREAM_MCLK_48K  12288000u

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_STREAM_H */
