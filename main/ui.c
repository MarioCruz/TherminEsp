#include "ui.h"

#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "lvgl.h"
#include "mbedtls/base64.h"

#include "bsp/display.h"
#include "bsp/led.h"
#include "settings.h"
#include "synth.h"

static const char *TAG = "ui";

/* Beach Boys palette (Wavr SPEC.md) */
#define COLOR_SAND   0xFFF8E7
#define COLOR_CORAL  0xFF6B6B
#define COLOR_TEAL   0x4ECDC4
#define COLOR_SKY    0x45B7D1
#define COLOR_NAVY   0x2C3E50
#define COLOR_WHITE  0xFFFFFF

static lv_obj_t *s_surface;
static lv_obj_t *s_preview;          /* live detector-input view (debug, hidden) */
static uint8_t *s_preview_buf;       /* RGB565 canvas buffer */
static volatile bool s_preview_visible = false;
static lv_obj_t *s_marker;
static lv_obj_t *s_marker_hand1;      /* second marker for duet hand 1 */
static lv_obj_t *s_note_label;
static lv_obj_t *s_freq_label;
static lv_obj_t *s_vol_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_mode_btn_label;
static lv_obj_t *s_scale_btn_label;
static lv_obj_t *s_root_btn_label;
static lv_obj_t *s_glide_btn_label;

/* per-source presence tracking: hint hides when ANY source is active */
static bool s_touch_active;
static bool s_hand0_active;
static bool s_hand1_active;

static const struct { const char *label; float seconds; } GLIDE_PRESETS[] = {
    { "Glide: Snap",  0.0f },
    { "Glide: Fast",  0.08f },
    { "Glide: Slow",  0.3f },
    { "Glide: Drift", 0.8f },
};
#define GLIDE_PRESET_COUNT (sizeof(GLIDE_PRESETS) / sizeof(GLIDE_PRESETS[0]))
static int s_glide_idx = 1;

/* pitch position -> LED hue sweep (coral red at low, sky blue at high) */
static void led_from_pitch(float x_norm)
{
    uint8_t r = (uint8_t)(60.0f * (1.0f - x_norm));
    uint8_t b = (uint8_t)(60.0f * x_norm);
    bsp_led_set_rgb(BSP_LED_STATUS, r, 20, b);
}

/* Show the hint only when no source is actively playing. Must be called
 * under the display lock (or from LVGL context). */
static void hint_update(void)
{
    if (s_touch_active || s_hand0_active || s_hand1_active) {
        lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void surface_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        synth_stop_voice(SYNTH_VOICE_TOUCH);
        lv_obj_add_flag(s_marker, LV_OBJ_FLAG_HIDDEN);
        if (!s_hand0_active) {
            lv_label_set_text(s_note_label, "--");
            lv_label_set_text(s_freq_label, "-- Hz");
            lv_label_set_text(s_vol_label, "vol --");
        }
        s_touch_active = false;
        hint_update();
        bsp_led_set_rgb(BSP_LED_STATUS, 0, 60, 0);
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t area;
    lv_obj_get_coords(s_surface, &area);
    float w = (float)lv_area_get_width(&area);
    float h = (float)lv_area_get_height(&area);
    float x = (p.x - area.x1) / w;
    float y = (p.y - area.y1) / h;
    x = x < 0 ? 0 : (x > 1 ? 1 : x);
    y = y < 0 ? 0 : (y > 1 ? 1 : y);
    float vol = 1.0f - y;

    /* dedicated touch voice: camera hand tracking owns voices 0/1 (up to a
     * two-hand duet), so touch and tracked hands can all sound together
     * instead of fighting over one voice.
     * openness fixed fully open since touch has no fist/open gesture. */
    float freq = synth_update_voice(SYNTH_VOICE_TOUCH, x, vol, 1.0f);

    char note[8];
    synth_note_name(freq, note);
    lv_label_set_text(s_note_label, note);
    lv_label_set_text_fmt(s_freq_label, "%d Hz", (int)freq);
    lv_label_set_text_fmt(s_vol_label, "vol %d%%", (int)(vol * 100));
    s_touch_active = true;
    hint_update();

    lv_obj_clear_flag(s_marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_marker, p.x - area.x1 - 12, p.y - area.y1 - 12);
    led_from_pitch(x);
}

/* "Clean Wave" shows which waveform it's currently playing so the
 * long-press toggle (below) has somewhere to display its state. Caller
 * must hold the display lock. */
static void mode_btn_refresh_label(void)
{
    if (synth_get_mode() == SYNTH_MODE_CLEAN) {
        lv_label_set_text(s_mode_btn_label, synth_get_clean_wave() ? "Clean Saw" : "Clean Sine");
    } else {
        lv_label_set_text(s_mode_btn_label, synth_mode_name(synth_get_mode()));
    }
}

/* ui_cycle_*()/ui_toggle_clean_wave() are the single source of truth for
 * "a control changed": they touch the synth, refresh the label, and mark
 * settings dirty. Both the touchscreen buttons and main/buttons.c (the
 * physical buttons, running on the iot_button library's own task) call
 * these directly instead of duplicating the logic — which means each one
 * can genuinely be entered from two different tasks. The read-modify-write
 * (read the current value, compute next, write it back) is NOT atomic on
 * its own, so the whole sequence — including s_glide_idx, a plain int with
 * no other protection — runs under the display lock, not just the label
 * update at the end. The lock is recursive, so this is free (just a
 * refcount bump) when already called from the LVGL task, and genuinely
 * serializes against the physical-button task the rest of the time. */

void ui_cycle_mode(void)
{
    if (!bsp_display_lock(-1)) {
        return;
    }
    synth_mode_t next = (synth_get_mode() + 1) % SYNTH_MODE_COUNT;
    synth_set_mode(next);
    mode_btn_refresh_label();
    bsp_display_unlock();
    settings_mark_dirty();
    ESP_LOGI(TAG, "mode -> %s", synth_mode_name(next));
}

/* Toggle sine/sawtooth while on Clean Wave; no-op on any other mode. Wired
 * to a long-press on the touchscreen mode button so a short tap always
 * advances mode — there's no dead end where you get stuck on Clean Wave. */
void ui_toggle_clean_wave(void)
{
    if (!bsp_display_lock(-1)) {
        return;
    }
    if (synth_get_mode() != SYNTH_MODE_CLEAN) {
        bsp_display_unlock();
        return;
    }
    synth_set_clean_wave(!synth_get_clean_wave());
    mode_btn_refresh_label();
    bsp_display_unlock();
    settings_mark_dirty();
    ESP_LOGI(TAG, "clean waveform -> %s", synth_get_clean_wave() ? "saw" : "sine");
}

void ui_cycle_scale(void)
{
    if (!bsp_display_lock(-1)) {
        return;
    }
    synth_scale_t next = (synth_get_scale() + 1) % SYNTH_SCALE_COUNT;
    synth_set_scale(next);
    lv_label_set_text(s_scale_btn_label, synth_scale_name(next));
    bsp_display_unlock();
    settings_mark_dirty();
    ESP_LOGI(TAG, "scale -> %s", synth_scale_name(next));
}

void ui_cycle_root(void)
{
    if (!bsp_display_lock(-1)) {
        return;
    }
    int next_root = 48 + ((synth_get_root() % 12) + 1) % 12;
    synth_set_root(next_root);
    lv_label_set_text(s_root_btn_label, synth_pitch_class_name(next_root));
    bsp_display_unlock();
    settings_mark_dirty();
    ESP_LOGI(TAG, "root -> %s", synth_pitch_class_name(next_root));
}

void ui_cycle_glide(void)
{
    if (!bsp_display_lock(-1)) {
        return;
    }
    s_glide_idx = (s_glide_idx + 1) % GLIDE_PRESET_COUNT;
    synth_set_glide(GLIDE_PRESETS[s_glide_idx].seconds);
    lv_label_set_text(s_glide_btn_label, GLIDE_PRESETS[s_glide_idx].label);
    bsp_display_unlock();
    settings_mark_dirty();
    ESP_LOGI(TAG, "glide -> %s", GLIDE_PRESETS[s_glide_idx].label);
}

static void mode_btn_cb(lv_event_t *e)      { ui_cycle_mode(); }
static void mode_btn_long_cb(lv_event_t *e) { ui_toggle_clean_wave(); }
static void scale_btn_cb(lv_event_t *e)     { ui_cycle_scale(); }
static void root_btn_cb(lv_event_t *e)      { ui_cycle_root(); }
static void glide_btn_cb(lv_event_t *e)     { ui_cycle_glide(); }

/* tap the title to toggle the camera debug view */
static void title_cb(lv_event_t *e)
{
    s_preview_visible = !s_preview_visible;
    if (s_preview) {
        if (s_preview_visible) {
            lv_obj_clear_flag(s_preview, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_preview, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, uint32_t color,
                             lv_event_cb_t cb, lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 210, 52);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_center(label);
    *out_label = label;
    return btn;
}

void ui_screenshot_dump(void)
{
    /* Read the RGB panel's framebuffer directly — the screen is static
     * between UI events, so no lock is needed and LVGL is never involved. */
    esp_lcd_panel_handle_t panel = bsp_display_get_panel();
    void *fbs[3] = {};
    if (!panel ||
        esp_lcd_rgb_panel_get_frame_buffer(panel, 3, &fbs[0], &fbs[1], &fbs[2]) != ESP_OK) {
        printf("SCREENSHOT FAIL\n");
        return;
    }
    /* triple buffering: dump every buffer — pick the complete one offline */
    const uint32_t w = BSP_LCD_H_RES;
    const uint32_t h = BSP_LCD_V_RES;
    static unsigned char b64[2400];
    for (int i = 0; i < 3; i++) {
        if (!fbs[i]) {
            continue;
        }
        printf("\nSCREENSHOT BEGIN %u %u %d\n", (unsigned)w, (unsigned)h, i);
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *row = (const uint8_t *)fbs[i] + y * w * 2;
            size_t olen = 0;
            if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &olen, row, w * 2) == 0) {
                b64[olen] = '\0';
                printf("%s\n", b64);
            }
            vTaskDelay(pdMS_TO_TICKS(1));   /* let the UART drain */
        }
        printf("SCREENSHOT END\n");
    }
    printf("SCREENSHOT ALL DONE\n");
}

void ui_preview_update(const uint8_t *rgb888, int w, int h)
{
    if (!s_preview_visible || !s_preview_buf || w != 224 || h != 224) {
        return;
    }
    if (!bsp_display_lock(20)) {
        return;
    }
    uint16_t *dst = (uint16_t *)s_preview_buf;
    const uint8_t *p = rgb888;
    for (int i = 0; i < w * h; i++, p += 3) {
        *dst++ = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
    }
    lv_obj_invalidate(s_preview);
    bsp_display_unlock();
}

void ui_hand_update(int slot, bool present, float x_norm, float y_norm, float freq, float vol_norm)
{
    if (slot < 0 || slot > 1) {
        return;
    }
    if (!bsp_display_lock(50)) {
        return;                      /* skip a frame rather than stall inference */
    }

    lv_obj_t *marker = (slot == 0) ? s_marker : s_marker_hand1;

    if (!present) {
        lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
        if (slot == 0) {
            s_hand0_active = false;
            if (!s_touch_active) {
                lv_label_set_text(s_note_label, "--");
                lv_label_set_text(s_freq_label, "-- Hz");
                lv_label_set_text(s_vol_label, "vol --");
            }
        } else {
            s_hand1_active = false;
        }
        hint_update();
    } else {
        if (slot == 0) {
            s_hand0_active = true;
            char note[8];
            synth_note_name(freq, note);
            lv_label_set_text(s_note_label, note);
            lv_label_set_text_fmt(s_freq_label, "%d Hz", (int)freq);
            lv_label_set_text_fmt(s_vol_label, "vol %d%%", (int)(vol_norm * 100));
        } else {
            s_hand1_active = true;
        }
        hint_update();

        int32_t w = lv_obj_get_width(s_surface);
        int32_t h = lv_obj_get_height(s_surface);
        lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(marker, (int32_t)(x_norm * w) - 12, (int32_t)(y_norm * h) - 12);
    }
    bsp_display_unlock();
}

void ui_set_camera_state(bool ok)
{
    if (ok || !bsp_display_lock(50)) {
        return;
    }
    lv_label_set_text(s_hint_label, "camera off — touch to play\n\nX = pitch    Y = volume");
    bsp_display_unlock();
}

esp_err_t ui_init(void)
{
    bsp_display_lock(-1);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_SAND), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* header */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "TherminEsp");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_CORAL), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 10);
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(title, title_cb, LV_EVENT_CLICKED, NULL);

    /* play surface: left 540x420 */
    s_surface = lv_obj_create(scr);
    lv_obj_set_size(s_surface, 540, 420);
    lv_obj_align(s_surface, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_set_style_bg_color(s_surface, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_radius(s_surface, 12, 0);
    lv_obj_set_style_border_width(s_surface, 2, 0);
    lv_obj_set_style_border_color(s_surface, lv_color_hex(COLOR_TEAL), 0);
    lv_obj_clear_flag(s_surface, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_surface, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_surface, surface_event_cb, LV_EVENT_ALL, NULL);

    s_hint_label = lv_label_create(s_surface);
    lv_label_set_text(s_hint_label, "touch to play\n\nX = pitch    Y = volume");
    lv_obj_set_style_text_font(s_hint_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(COLOR_NAVY), 0);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_hint_label);

    /* live camera view: what the detector sees, bottom-left of the surface */
    s_preview_buf = heap_caps_malloc(224 * 224 * 2, MALLOC_CAP_SPIRAM);
    if (s_preview_buf) {
        s_preview = lv_canvas_create(s_surface);
        lv_canvas_set_buffer(s_preview, s_preview_buf, 224, 224, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(s_preview, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_radius(s_preview, 8, 0);
        lv_obj_clear_flag(s_preview, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_preview, LV_OBJ_FLAG_HIDDEN);
    }

    s_marker = lv_obj_create(s_surface);
    lv_obj_set_size(s_marker, 24, 24);
    lv_obj_set_style_radius(s_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_marker, lv_color_hex(COLOR_CORAL), 0);
    lv_obj_set_style_border_width(s_marker, 0, 0);
    lv_obj_add_flag(s_marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_marker, LV_OBJ_FLAG_CLICKABLE);

    s_marker_hand1 = lv_obj_create(s_surface);
    lv_obj_set_size(s_marker_hand1, 24, 24);
    lv_obj_set_style_radius(s_marker_hand1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_marker_hand1, lv_color_hex(COLOR_TEAL), 0);
    lv_obj_set_style_border_width(s_marker_hand1, 0, 0);
    lv_obj_add_flag(s_marker_hand1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_marker_hand1, LV_OBJ_FLAG_CLICKABLE);

    /* metrics panel: right side */
    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_set_size(panel, 220, 420);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 6, 0);

    s_note_label = lv_label_create(panel);
    lv_label_set_text(s_note_label, "--");
    lv_obj_set_style_text_font(s_note_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_note_label, lv_color_hex(COLOR_NAVY), 0);

    s_freq_label = lv_label_create(panel);
    lv_label_set_text(s_freq_label, "-- Hz");
    lv_obj_set_style_text_font(s_freq_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_freq_label, lv_color_hex(COLOR_CORAL), 0);

    s_vol_label = lv_label_create(panel);
    lv_label_set_text(s_vol_label, "vol --");
    lv_obj_set_style_text_font(s_vol_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_vol_label, lv_color_hex(COLOR_TEAL), 0);

    const char *mode_label = (synth_get_mode() == SYNTH_MODE_CLEAN)
        ? (synth_get_clean_wave() ? "Clean Saw" : "Clean Sine")
        : synth_mode_name(synth_get_mode());
    lv_obj_t *mode_btn = make_button(panel, mode_label, COLOR_CORAL, mode_btn_cb, &s_mode_btn_label);
    lv_obj_add_event_cb(mode_btn, mode_btn_long_cb, LV_EVENT_LONG_PRESSED, NULL);

    make_button(panel, synth_scale_name(synth_get_scale()), COLOR_SKY,
                scale_btn_cb, &s_scale_btn_label);
    make_button(panel, synth_pitch_class_name(synth_get_root()), COLOR_TEAL,
                root_btn_cb, &s_root_btn_label);

    float cur_glide = synth_get_glide();
    s_glide_idx = 1;
    for (int i = 0; i < (int)GLIDE_PRESET_COUNT; i++) {
        if (fabsf(GLIDE_PRESETS[i].seconds - cur_glide) < 0.001f) {
            s_glide_idx = i;
            break;
        }
    }
    make_button(panel, GLIDE_PRESETS[s_glide_idx].label, COLOR_NAVY,
                glide_btn_cb, &s_glide_btn_label);

    bsp_display_unlock();
    ESP_LOGI(TAG, "theremin screen up");
    return ESP_OK;
}
