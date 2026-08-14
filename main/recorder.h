/*
 * TherminEsp WAV recorder — captures synth output PCM blocks to a .wav file
 * on the microSD card. Start/stop via long-press VOLUP physical button.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the SD card and prepare for recording. Non-fatal if no card. */
esp_err_t recorder_init(void);

/* Start recording to a new WAV file (rec_NNN.wav). No-op if already recording
 * or if the SD card isn't mounted. */
void recorder_start(void);

/* Stop recording and finalize the WAV header. No-op if not recording. */
void recorder_stop(void);

/* Toggle start/stop. */
void recorder_toggle(void);

/* Feed PCM data (16-bit stereo interleaved, 48 kHz). Called from the synth
 * render task every block. Writes to the open file if recording is active.
 * Must be fast — the synth task is real-time. */
void recorder_feed(const int16_t *samples, size_t num_bytes);

/* Is a recording currently in progress? */
bool recorder_is_recording(void);

#ifdef __cplusplus
}
#endif
