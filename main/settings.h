/*
 * TherminEsp settings persistence — synth mode/scale/root/glide survive
 * reboot via NVS.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the NVS namespace and starts the debounced-write task. Call once,
 * before settings_load(). Calls nvs_flash_init() itself (erase+retry on the
 * usual first-boot/version errors) since nothing else in the app does. */
esp_err_t settings_init(void);

/* Reads stored values and applies them via synth_set_*. Call after
 * synth_init() — it drives the live synth. Missing keys (first boot) leave
 * the synth's own defaults in place. */
void settings_load(void);

/* Call from UI callbacks whenever a setting changes. Debounced: only writes
 * to flash ~2 s after the last call, so rapid button taps cost one write. */
void settings_mark_dirty(void);

#ifdef __cplusplus
}
#endif
