#include "settings.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "synth.h"

static const char *TAG = "settings";

#define NAMESPACE       "therminesp"
#define KEY_MODE        "mode"
#define KEY_SCALE       "scale"
#define KEY_ROOT        "root"
#define KEY_GLIDE_MS    "glide_ms"       /* glide seconds * 1000, stored as i32 */
#define KEY_CLEAN_SAW   "clean_saw"
#define WRITE_DEBOUNCE_MS 2000

static nvs_handle_t s_handle;
static TaskHandle_t s_task;

static void settings_write_now(void)
{
    nvs_set_u8(s_handle, KEY_MODE, (uint8_t)synth_get_mode());
    nvs_set_u8(s_handle, KEY_SCALE, (uint8_t)synth_get_scale());
    nvs_set_i32(s_handle, KEY_ROOT, synth_get_root());
    nvs_set_i32(s_handle, KEY_GLIDE_MS, (int32_t)(synth_get_glide() * 1000.0f + 0.5f));
    nvs_set_u8(s_handle, KEY_CLEAN_SAW, (uint8_t)synth_get_clean_wave());
    esp_err_t err = nvs_commit(s_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commit failed: %s", esp_err_to_name(err));
    }
}

static void settings_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* each further tap postpones the write by resetting this wait */
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WRITE_DEBOUNCE_MS)) > 0) {
        }
        settings_write_now();
    }
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_open(NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(settings_task, "settings", 3072, NULL, 3, &s_task, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void settings_load(void)
{
    if (!s_task) {
        return;
    }

    uint8_t u8;
    int32_t i32;

    if (nvs_get_u8(s_handle, KEY_MODE, &u8) == ESP_OK) {
        synth_set_mode((synth_mode_t)u8);
    }
    if (nvs_get_u8(s_handle, KEY_SCALE, &u8) == ESP_OK) {
        synth_set_scale((synth_scale_t)u8);
    }
    if (nvs_get_i32(s_handle, KEY_ROOT, &i32) == ESP_OK) {
        synth_set_root(i32);
    }
    if (nvs_get_i32(s_handle, KEY_GLIDE_MS, &i32) == ESP_OK) {
        if (i32 < 0) i32 = 0;
        synth_set_glide(i32 / 1000.0f);
    }
    if (nvs_get_u8(s_handle, KEY_CLEAN_SAW, &u8) == ESP_OK) {
        synth_set_clean_wave((bool)u8);
    }
}

void settings_mark_dirty(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}
