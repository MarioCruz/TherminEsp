/*
 * TherminEsp hand tracker — espdet-pico inference over decoded camera frames,
 * mapped to synth voices 0/1 (up to two tracked hands).
 */
#pragma once

#include "esp_err.h"
#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float score_thr;      /* espdet-pico confidence floor */
    float area_closed;    /* box-area fraction -> openness = 0 (fist) */
    float area_open;      /* box-area fraction -> openness = 1 (open hand) */
    float active_margin;  /* FOV edge fraction excluded, remapped to full [0,1] */
    int   miss_limit;     /* consecutive undetected frames before a note drops */
    float ema_alpha;      /* position/openness smoothing, 0..1 */
} tracker_tuning_t;

/* Allocate decode/inference buffers and start the tracker task. */
esp_err_t tracker_init(void);

/* Camera frame callback — hand this to camera_start(). */
void tracker_on_frame(const camera_frame_view_t *frame, void *ctx);

/* Current tuning — defaults match the values proven during the 2026-08-08/09
 * field-tuning session (see tracker.cpp). */
tracker_tuning_t tracker_get_tuning(void);

/* Apply new tuning immediately — takes effect on the next processed frame.
 * Safe to call from any task. */
void tracker_set_tuning(const tracker_tuning_t *tuning);

#ifdef __cplusplus
}
#endif
