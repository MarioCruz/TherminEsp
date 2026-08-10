/*
 * TherminEsp — visual theremin on ESP32-S31-Korvo.
 *
 * Phase 1: on-device synth (port of Wavr's audio engine) played from the
 * touchscreen. The camera hand tracker replaces touch in a later phase.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp/display.h"
#include "bsp/led.h"
#include "buttons.h"
#include "camera.h"
#include "settings.h"
#include "synth.h"
#include "tracker.h"
#include "ui.h"

static const char *TAG = "theremin";

/* Boot self-test: sweep one voice through every synth mode. Exercises every
 * render branch under real 48 kHz load — a crash or stall here means a broken
 * mode. Audible as a short demo once speakers are connected. */
static void synth_selftest(void)
{
    for (int m = 0; m < SYNTH_MODE_COUNT; m++) {
        synth_set_mode((synth_mode_t)m);
        float last_freq = 0;
        for (int step = 0; step <= 20; step++) {
            last_freq = synth_update_voice(0, step / 20.0f, 0.6f, 1.0f);
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        ESP_LOGI(TAG, "selftest: %-12s ok (top %.0f Hz)",
                 synth_mode_name((synth_mode_t)m), last_freq);
    }
    synth_stop_voice(0);
    synth_set_mode(SYNTH_MODE_FM);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== TherminEsp phase 1 (touch theremin) ===");
    ESP_LOGI(TAG, "PSRAM pool: %u KiB",
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));

    ESP_ERROR_CHECK(bsp_led_init());
    bsp_led_set_rgb(BSP_LED_STATUS, 0, 40, 40);

    if (!bsp_display_start()) {
        ESP_LOGE(TAG, "display init failed");
        bsp_led_set_rgb(BSP_LED_STATUS, 60, 0, 0);
        return;
    }
    bsp_display_backlight_on();

    ESP_ERROR_CHECK(synth_init());
    if (settings_init() != ESP_OK) {
        ESP_LOGW(TAG, "settings persistence unavailable — defaults only, nothing will stick across reboots");
    }
    synth_selftest();
    /* Restore saved mode/scale/root/glide AFTER the self-test, which always
     * ends by resetting to FM — otherwise a persisted non-FM mode would be
     * clobbered right after boot. ui_init() runs after this so its button
     * labels reflect the restored state, not the self-test's FM reset. */
    settings_load();
    ESP_ERROR_CHECK(ui_init());
    buttons_init();     /* non-fatal if unavailable — logs its own warning */

    /* Hand tracking: camera frames -> JPEG decode -> espdet-pico -> voice 0.
     * If either fails, the touchscreen still plays. */
    if (tracker_init() != ESP_OK || camera_start(tracker_on_frame, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "camera/tracker unavailable — touch play still works");
        ui_set_camera_state(false);
    }

    bsp_led_set_rgb(BSP_LED_STATUS, 0, 60, 0);
    ESP_LOGI(TAG, "ready: touch the screen to play");

#ifdef CONFIG_THEREMIN_BOOT_SCREENSHOT
    /* One-shot remote screenshot ~8 s after boot (for docs). */
    vTaskDelay(pdMS_TO_TICKS(8000));
    ui_screenshot_dump();
#endif

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGI(TAG, "heartbeat: free heap %u KiB, free PSRAM %u KiB",
                 (unsigned)(esp_get_free_heap_size() / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}
