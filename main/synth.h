/*
 * TherminEsp synth engine — C port of Wavr's audio-engine.js.
 * Renders 48 kHz stereo 16-bit into the ES8389 codec from a dedicated task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 0,1 = up to two camera-tracked hands (duet); 2 = touchscreen, kept
 * separate so touch and a tracked hand can sound together. */
#define SYNTH_NUM_VOICES 3
#define SYNTH_VOICE_TOUCH (SYNTH_NUM_VOICES - 1)

typedef enum {
    SYNTH_MODE_FM = 0,
    SYNTH_MODE_CLEAN,
    SYNTH_MODE_WARM,
    SYNTH_MODE_PAD,
    SYNTH_MODE_THEREMIN,
    SYNTH_MODE_ORGAN,
    SYNTH_MODE_BITCRUSH,
    SYNTH_MODE_COUNT,
} synth_mode_t;

typedef enum {
    SYNTH_SCALE_CHROMATIC = 0,
    SYNTH_SCALE_MAJOR,
    SYNTH_SCALE_MINOR,
    SYNTH_SCALE_PENTATONIC,
    SYNTH_SCALE_PENT_MINOR,
    SYNTH_SCALE_BLUES,
    SYNTH_SCALE_DORIAN,
    SYNTH_SCALE_MIXOLYDIAN,
    SYNTH_SCALE_HARMONIC_MIN,
    SYNTH_SCALE_WHOLE_TONE,
    SYNTH_SCALE_COUNT,
} synth_scale_t;

/* Start the codec output task. Call once after the BSP codec is available. */
esp_err_t synth_init(void);

/* Drive a voice. freq_norm/vol_norm/openness in [0,1]. Activates the voice on
 * first call. Returns the scale-quantized frequency in Hz (for display). */
float synth_update_voice(int id, float freq_norm, float vol_norm, float openness);

/* Fade a voice out (50 ms) and mark it idle. */
void synth_stop_voice(int id);

void synth_set_mode(synth_mode_t mode);
void synth_set_scale(synth_scale_t scale);
void synth_set_root(int midi_note);       /* default 48 = C3 */
void synth_set_glide(float seconds);      /* default 0.08 */
void synth_set_clean_wave(bool saw);      /* false = sine (default), true = sawtooth */

synth_mode_t synth_get_mode(void);
synth_scale_t synth_get_scale(void);
int synth_get_root(void);
float synth_get_glide(void);
bool synth_get_clean_wave(void);
const char *synth_mode_name(synth_mode_t mode);
const char *synth_scale_name(synth_scale_t scale);

/* Note name ("A4") for a frequency. buf must hold >= 8 bytes. */
void synth_note_name(float freq, char *buf);

/* Pitch class name ("C", "C#", ...) for any MIDI note, octave ignored. */
const char *synth_pitch_class_name(int midi_note);

#ifdef __cplusplus
}
#endif
