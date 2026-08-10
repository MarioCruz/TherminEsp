/*
 * TherminEsp camera pipeline — feeds OV3660 frames to the hand tracker.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A camera frame delivered to the consumer callback. is_jpeg is always true
 * today (the only capture mode this DVP driver accepts — see camera.c);
 * data/width/height still describe whatever was actually negotiated, so
 * consumers should check them rather than assume a fixed resolution. */
typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    bool is_jpeg;
} camera_frame_view_t;

typedef void (*camera_frame_cb_t)(const camera_frame_view_t *frame, void *ctx);

/* Open the camera at 1280x720 JPEG and start the capture task. cb runs in
 * the capture task for every frame; keep it quick or copy out. */
esp_err_t camera_start(camera_frame_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
