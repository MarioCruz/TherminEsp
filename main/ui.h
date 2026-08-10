/*
 * TherminEsp LVGL UI — Beach Boys-themed theremin screen with a touch play
 * surface that plays alongside the camera hand tracker, not instead of it.
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

/* Render the current screen and dump it over serial as base64 RGB565
 * (SCREENSHOT BEGIN/END markers). Blocks the caller for ~90 s at 115200. */
void ui_screenshot_dump(void);

/* Call with false if the camera/tracker pipeline failed to start, so the
 * hint text says so instead of silently looking like touch-only was always
 * the plan. No-op (and no need to call) when the camera is healthy. */
void ui_set_camera_state(bool ok);

/* Advance mode/scale/root/glide (or toggle Clean Wave's sine/sawtooth), the
 * same actions the touchscreen buttons perform — updates the synth, the
 * on-screen label, and marks settings dirty for the NVS debounce. Safe to
 * call from any task (physical buttons included): each takes the display
 * lock itself. */
void ui_cycle_mode(void);
void ui_cycle_scale(void);
void ui_cycle_root(void);
void ui_cycle_glide(void);
void ui_toggle_clean_wave(void);

#ifdef __cplusplus
}
#endif
