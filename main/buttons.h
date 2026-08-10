/*
 * TherminEsp physical controls — the Korvo board's 4-button ADC ladder as a
 * touch-free alternative to the on-screen buttons.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the 4 physical buttons. Call after ui_init() (the button
 * handlers drive ui_cycle_*()). Non-fatal if the buttons can't be created —
 * logs a warning and returns; the touchscreen still works. */
esp_err_t buttons_init(void);

#ifdef __cplusplus
}
#endif
