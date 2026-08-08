/*
 * TherminEsp hand tracker — espdet-pico inference over decoded camera frames,
 * mapped to synth voice 0 (the "camera hand").
 */
#pragma once

#include "esp_err.h"
#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate decode/inference buffers and start the tracker task. */
esp_err_t tracker_init(void);

/* Camera frame callback — hand this to camera_start(). */
void tracker_on_frame(const camera_frame_view_t *frame, void *ctx);

#ifdef __cplusplus
}
#endif
