/*
 * TherminEsp LVGL UI — Beach Boys-themed theremin screen with a touch play
 * surface (interim controller until the camera hand tracker lands).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the screen. Call after bsp_display_start() and synth_init(). */
esp_err_t ui_init(void);

/* Show the camera-tracked hand on the play surface. Safe to call from any
 * task (takes the LVGL lock). x/y normalized [0,1] in surface space; freq is
 * the quantized frequency in Hz; vol_norm in [0,1]. present=false clears. */
void ui_hand_update(bool present, float x_norm, float y_norm, float freq, float vol_norm);

/* Live view of the detector input (RGB888, w*h). Safe from any task. */
void ui_preview_update(const uint8_t *rgb888, int w, int h);

#ifdef __cplusplus
}
#endif
