#include "buttons.h"

#include <stdint.h>

#include "esp_log.h"

#include "bsp/button.h"
#include "recorder.h"
#include "ui.h"

static const char *TAG = "buttons";

/* MODE/SET/VOLUP/VOLDOWN are the Korvo BSP's generic button names (this
 * board's silkscreen is a generic media layout); repurposed here as
 * mode / scale / root / glide, matching the touchscreen buttons' order. */
static void button_cb(void *btn, void *user_data)
{
    switch ((bsp_button_t)(intptr_t)user_data) {
    case BSP_BUTTON_MODE:    ui_cycle_mode();  break;
    case BSP_BUTTON_SET:     ui_cycle_scale(); break;
    case BSP_BUTTON_VOLUP:   ui_cycle_root();  break;
    case BSP_BUTTON_VOLDOWN: ui_cycle_glide(); break;
    default: break;
    }
}

/* Long-press on MODE toggles sine/saw when on Clean Wave — mirrors the
 * touchscreen mode button's long-press behavior. */
static void mode_long_cb(void *btn, void *user_data)
{
    ui_toggle_clean_wave();
}

/* Long-press on SET toggles between Theremin and Piano keyboard modes. */
static void set_long_cb(void *btn, void *user_data)
{
    ui_toggle_app_mode();
}

/* Long-press on VOLUP toggles WAV recording to the SD card. */
static void volup_long_cb(void *btn, void *user_data)
{
    recorder_toggle();
}

esp_err_t buttons_init(void)
{
    button_handle_t btns[BSP_BUTTON_NUM];
    int btn_cnt = 0;
    esp_err_t err = bsp_iot_button_create(btns, &btn_cnt, BSP_BUTTON_NUM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "physical buttons unavailable (touch still works): %s", esp_err_to_name(err));
        return err;
    }
    for (int i = 0; i < btn_cnt; i++) {
        iot_button_register_cb(btns[i], BUTTON_PRESS_DOWN, NULL, button_cb, (void *)(intptr_t)i);
    }
    /* long-press on MODE (index BSP_BUTTON_MODE) toggles Clean Wave's sine/saw */
    if (btn_cnt > (int)BSP_BUTTON_MODE) {
        iot_button_register_cb(btns[BSP_BUTTON_MODE], BUTTON_LONG_PRESS_START, NULL, mode_long_cb, NULL);
    }
    /* long-press on SET toggles between Theremin and Piano keyboard */
    if (btn_cnt > (int)BSP_BUTTON_SET) {
        iot_button_register_cb(btns[BSP_BUTTON_SET], BUTTON_LONG_PRESS_START, NULL, set_long_cb, NULL);
    }
    /* long-press on VOLUP toggles WAV recording to SD card */
    if (btn_cnt > (int)BSP_BUTTON_VOLUP) {
        iot_button_register_cb(btns[BSP_BUTTON_VOLUP], BUTTON_LONG_PRESS_START, NULL, volup_long_cb, NULL);
    }
    ESP_LOGI(TAG, "%d physical buttons registered", btn_cnt);
    return ESP_OK;
}
