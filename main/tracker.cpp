#include "tracker.h"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hand_detect.hpp"
#include "dl_image_define.hpp"
#include "mbedtls/base64.h"

extern "C" {
#include "bsp/led.h"
#include "synth.h"
#include "ui.h"
}

static const char *TAG = "tracker";

#define SRC_W        1280
#define SRC_H        720
#define DET_W        224
#define DET_H        224
#define JPEG_IN_MAX  (512 * 1024)

/* Defaults proven during the 2026-08-08/09 field-tuning session: score_thr
 * lowered from the model's 0.2 default because real (non-reference-photo)
 * frames scored 0.2-0.4; miss_limit widened to ~1.2 s so brief confidence
 * dips don't stutter the note; active_margin excludes the unreliable outer
 * band of the narrow FOV (hands there — especially near the top, reached
 * raising for volume — routinely dropped out) and remaps the reliable
 * center to the full control range. Runtime-tunable via tracker_set_tuning()
 * instead of a reflash per adjustment. */
static portMUX_TYPE s_tuning_lock = portMUX_INITIALIZER_UNLOCKED;
static tracker_tuning_t s_tuning = {
    .score_thr = 0.15f,
    .area_closed = 0.03f,
    .area_open = 0.20f,
    .active_margin = 0.22f,
    .miss_limit = 10,
    .ema_alpha = 0.5f,
};

tracker_tuning_t tracker_get_tuning(void)
{
    taskENTER_CRITICAL(&s_tuning_lock);
    tracker_tuning_t t = s_tuning;
    taskEXIT_CRITICAL(&s_tuning_lock);
    return t;
}

void tracker_set_tuning(const tracker_tuning_t *tuning)
{
    taskENTER_CRITICAL(&s_tuning_lock);
    s_tuning = *tuning;
    taskEXIT_CRITICAL(&s_tuning_lock);
}

/* latest-frame mailbox: camera task writes, tracker task reads */
static uint8_t *s_jpeg_in;
static size_t s_jpeg_in_cap;
static volatile uint32_t s_jpeg_len;
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_frame_ready;

static jpeg_decoder_handle_t s_dec;
static uint8_t *s_decode_buf;       /* 1280x720 RGB888 */
static size_t s_decode_cap;
static uint8_t *s_det_buf;          /* 224x224 RGB888 for the detector */

void tracker_on_frame(const camera_frame_view_t *frame, void *ctx)
{
    if (!frame->is_jpeg) {
        return;
    }
    if (frame->width != SRC_W || frame->height != SRC_H) {
        /* downscale_to_det()'s stride math and the JPEG decode size check
         * both assume exactly SRC_W x SRC_H — if the sensor ever negotiates
         * something else (mode table change, BSP update), drop rather than
         * silently misalign the downscale. */
        static uint32_t drop_count = 0;
        if (drop_count % 100 == 0) {
            ESP_LOGW(TAG, "dropping frame: %ux%u != expected %dx%d (drop #%u)",
                     (unsigned)frame->width, (unsigned)frame->height, SRC_W, SRC_H,
                     (unsigned)(drop_count + 1));
        }
        drop_count++;
        return;
    }
    if (frame->size > s_jpeg_in_cap) {
        static uint32_t drop_count = 0;
        if (drop_count % 100 == 0) {
            ESP_LOGW(TAG, "dropping oversized JPEG frame: %u bytes > %u cap (drop #%u)",
                     (unsigned)frame->size, (unsigned)s_jpeg_in_cap, (unsigned)(drop_count + 1));
        }
        drop_count++;
        return;
    }
    /* never block the camera task; drop the frame if the tracker holds the box */
    if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        memcpy(s_jpeg_in, frame->data, frame->size);
        s_jpeg_len = frame->size;
        xSemaphoreGive(s_mutex);
        xSemaphoreGive(s_frame_ready);
    }
}

/* nearest-neighbor downscale of the RGB888 decode.
 * Rows are read bottom-up: the camera mounts upside down on this board and
 * the sensor flip controls are stubbed (same finding as the wedding
 * recorder's PREVIEW_SW_VFLIP), so the flip happens here. */
static void downscale_to_det(void)
{
    uint8_t *dst = s_det_buf;
    for (int y = 0; y < DET_H; y++) {
        int sy = SRC_H - 1 - (y * SRC_H / DET_H);
        const uint8_t *row = s_decode_buf + sy * SRC_W * 3;
        for (int x = 0; x < DET_W; x++) {
            const uint8_t *p = row + (x * SRC_W / DET_W) * 3;
            *dst++ = p[0];
            *dst++ = p[1];
            *dst++ = p[2];
        }
    }
}

#ifdef CONFIG_THEREMIN_DUMP_FIRST_FRAME
/* Dump the detector input over serial as base64 so the exact image can be
 * inspected off-board — see CONFIG_THEREMIN_DUMP_FIRST_FRAME. */
static void dump_det_frame(void)
{
    printf("\nFRAMEDUMP BEGIN %d %d\n", DET_W, DET_H);
    const size_t chunk = 3000;
    unsigned char b64[4096];
    for (size_t off = 0; off < DET_W * DET_H * 3; off += chunk) {
        size_t n = DET_W * DET_H * 3 - off;
        if (n > chunk) {
            n = chunk;
        }
        size_t olen = 0;
        if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &olen, s_det_buf + off, n) == 0) {
            b64[olen] = '\0';
            printf("%s\n", b64);
        }
        vTaskDelay(pdMS_TO_TICKS(2));   /* let the UART drain */
    }
    printf("FRAMEDUMP END\n");
}
#endif

/* expand the reliable center band [margin, 1-margin] to full [0,1] */
static inline float remap_active(float v, float margin)
{
    float out = (v - margin) / (1.0f - 2.0f * margin);
    return out < 0 ? 0 : (out > 1 ? 1 : out);
}

static inline float dist2(float ax, float ay, float bx, float by)
{
    float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

/* Up to two tracked hands, one per camera voice (0/1 — see
 * SYNTH_VOICE_TOUCH in synth.h for why touch gets its own voice instead). */
#define MAX_HANDS 2
typedef struct {
    float ema_x, ema_y, ema_open;
    bool present;
    int misses;
} hand_track_t;

struct DetCenter { float cx, cy, area, score; };

/* Assign up to 2 detections to up to 2 hand slots by nearest previous EMA
 * position, so identity mostly survives frame-to-frame even as hands move —
 * important so a duet doesn't randomly swap which voice each hand drives.
 * With only one hand ever in frame this always resolves to slot 0, so
 * single-hand play is unchanged from before this feature existed. */
static void assign_detections(const DetCenter *dets, int det_count,
                              const hand_track_t hands[MAX_HANDS], int assign[MAX_HANDS])
{
    assign[0] = assign[1] = -1;
    if (det_count == 1) {
        int slot = 0;
        if (hands[1].present && !hands[0].present) {
            slot = 1;
        } else if (hands[0].present && hands[1].present) {
            float d0 = dist2(dets[0].cx, dets[0].cy, hands[0].ema_x, hands[0].ema_y);
            float d1 = dist2(dets[0].cx, dets[0].cy, hands[1].ema_x, hands[1].ema_y);
            slot = (d1 < d0) ? 1 : 0;
        }
        assign[0] = slot;
    } else if (det_count == 2) {
        if (hands[0].present && hands[1].present) {
            float straight = dist2(dets[0].cx, dets[0].cy, hands[0].ema_x, hands[0].ema_y)
                            + dist2(dets[1].cx, dets[1].cy, hands[1].ema_x, hands[1].ema_y);
            float crossed  = dist2(dets[0].cx, dets[0].cy, hands[1].ema_x, hands[1].ema_y)
                            + dist2(dets[1].cx, dets[1].cy, hands[0].ema_x, hands[0].ema_y);
            if (straight <= crossed) { assign[0] = 0; assign[1] = 1; }
            else                     { assign[0] = 1; assign[1] = 0; }
        } else if (hands[0].present || hands[1].present) {
            int present = hands[0].present ? 0 : 1;
            int other = 1 - present;
            float d0 = dist2(dets[0].cx, dets[0].cy, hands[present].ema_x, hands[present].ema_y);
            float d1 = dist2(dets[1].cx, dets[1].cy, hands[present].ema_x, hands[present].ema_y);
            if (d0 <= d1) { assign[0] = present; assign[1] = other; }
            else          { assign[1] = present; assign[0] = other; }
        } else {
            /* neither slot has an identity yet: seed by score, highest first */
            assign[0] = 0;
            assign[1] = 1;
        }
    }
}

static void tracker_task(void *arg)
{
    HandDetect detector;
    detector.set_score_thr(tracker_get_tuning().score_thr);
    hand_track_t hands[MAX_HANDS] = {};
    uint32_t infer_count = 0;
    int64_t infer_us_sum = 0;
    float best_seen = 0.0f;
    int64_t window_start = esp_timer_get_time();

    jpeg_decode_cfg_t dcfg = {};
    dcfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
    dcfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
    dcfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;

    ESP_LOGI(TAG, "task up, first inference loads the model (flash rodata)");

    /* Model sanity check: run the detector on an embedded known-good photo of
     * two open hands. Proves the model + pipeline work on this chip
     * independent of camera image quality. */
    {
        extern const uint8_t testhand_start[] asm("_binary_testhand_224_rgb_start");
        dl::image::img_t timg = {};
        timg.data = (void *)testhand_start;
        timg.width = DET_W;
        timg.height = DET_H;
        timg.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
        std::list<dl::detect::result_t> &tres = detector.run(timg);
        float tbest = 0;
        for (const auto &r : tres) {
            if (r.score > tbest) {
                tbest = r.score;
            }
        }
        ESP_LOGI(TAG, "MODEL SELFTEST: %d hands in known-good image, best score %.2f %s",
                 (int)tres.size(), tbest, tres.size() ? "-- MODEL OK" : "-- MODEL BROKEN ON S31");
    }

    while (true) {
        if (xSemaphoreTake(s_frame_ready, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint32_t out_len = 0;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        esp_err_t de = jpeg_decoder_process(s_dec, &dcfg, s_jpeg_in, s_jpeg_len,
                                            s_decode_buf, s_decode_cap, &out_len);
        xSemaphoreGive(s_mutex);
        if (de != ESP_OK || out_len < SRC_W * SRC_H * 3) {
            continue;
        }

        downscale_to_det();

        static int frame_n = 0;
        if ((++frame_n & 1) == 0) {
            ui_preview_update(s_det_buf, DET_W, DET_H);
        }
#ifdef CONFIG_THEREMIN_DUMP_FIRST_FRAME
        if (frame_n == 30) {   /* let the camera settle a moment first */
            dump_det_frame();
        }
#endif

        tracker_tuning_t tune = tracker_get_tuning();
        detector.set_score_thr(tune.score_thr);   /* cheap; simpler than a change check */

        dl::image::img_t img = {};
        img.data = s_det_buf;
        img.width = DET_W;
        img.height = DET_H;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
        int64_t t0 = esp_timer_get_time();
        std::list<dl::detect::result_t> &results = detector.run(img);
        int64_t t1 = esp_timer_get_time();
        infer_count++;
        infer_us_sum += (t1 - t0);

        /* top 2 detections by score — the model can return more than 2
         * candidates (false positives, retried NMS ties); we only ever
         * drive 2 voices, so anything past the top 2 is discarded. */
        DetCenter dets[MAX_HANDS];
        int det_count = 0;
        {
            const dl::detect::result_t *top1 = nullptr, *top2 = nullptr;
            for (const auto &r : results) {
                if (!top1 || r.score > top1->score) { top2 = top1; top1 = &r; }
                else if (!top2 || r.score > top2->score) { top2 = &r; }
            }
            const dl::detect::result_t *picked[MAX_HANDS] = { top1, top2 };
            for (int i = 0; i < MAX_HANDS; i++) {
                if (!picked[i]) {
                    continue;
                }
                dets[det_count].cx = (picked[i]->box[0] + picked[i]->box[2]) * 0.5f / DET_W;
                dets[det_count].cy = (picked[i]->box[1] + picked[i]->box[3]) * 0.5f / DET_H;
                dets[det_count].area = (float)picked[i]->box_area() / (DET_W * DET_H);
                dets[det_count].score = picked[i]->score;
                det_count++;
            }
        }
        if (det_count > 0 && dets[0].score > best_seen) {
            best_seen = dets[0].score;
        }

#ifdef CONFIG_THEREMIN_TRACE_FRAMES
        /* per-frame trace: how many camera frames waited (queue depth proxy),
         * inference ms, detection count + best score, hand center */
        if ((frame_n % 3) == 0) {
            ESP_LOGI(TAG, "f#%d dec+infer=%lldms dets=%d score=%.2f pos=(%.2f,%.2f)",
                     frame_n, (t1 - t0) / 1000, det_count,
                     det_count > 0 ? dets[0].score : 0.0f,
                     det_count > 0 ? dets[0].cx : 0.0f,
                     det_count > 0 ? dets[0].cy : 0.0f);
        }
#endif

        int assign[MAX_HANDS];
        assign_detections(dets, det_count, hands, assign);

        bool slot_updated[MAX_HANDS] = { false, false };
        for (int i = 0; i < det_count; i++) {
            int slot = assign[i];
            if (slot < 0) {
                continue;
            }
            hand_track_t &h = hands[slot];
            float open = (dets[i].area - tune.area_closed) / (tune.area_open - tune.area_closed);
            open = open < 0 ? 0 : (open > 1 ? 1 : open);

            if (!h.present) {
                h.ema_x = dets[i].cx; h.ema_y = dets[i].cy; h.ema_open = open;
                h.present = true;
            } else {
                h.ema_x += tune.ema_alpha * (dets[i].cx - h.ema_x);
                h.ema_y += tune.ema_alpha * (dets[i].cy - h.ema_y);
                h.ema_open += tune.ema_alpha * (open - h.ema_open);
            }
            h.misses = 0;
            slot_updated[slot] = true;

            /* mirror X so moving your hand right raises pitch on screen;
             * remap the reliable center band to the full control range */
            float freq_norm = remap_active(1.0f - h.ema_x, tune.active_margin);
            float vol = remap_active(1.0f - h.ema_y, tune.active_margin);
            float freq = synth_update_voice(slot, freq_norm, vol, h.ema_open);
            if (slot == 0) {
                /* on-screen note/freq/vol/marker track hand 0 only; hand 1
                 * still sounds (voice 1), just without its own display —
                 * a two-marker UI is a natural follow-up, not done here */
                ui_hand_update(true, freq_norm, 1.0f - vol, freq, vol);
                bsp_led_set_rgb(BSP_LED_STATUS,
                                (uint8_t)(60 * (1.0f - freq_norm)), 20,
                                (uint8_t)(60 * freq_norm));
            }
        }

        for (int slot = 0; slot < MAX_HANDS; slot++) {
            if (slot_updated[slot] || !hands[slot].present) {
                continue;
            }
            if (++hands[slot].misses >= tune.miss_limit) {
                hands[slot].present = false;
                synth_stop_voice(slot);
                if (slot == 0) {
                    ui_hand_update(false, 0, 0, 0, 0);
                    bsp_led_set_rgb(BSP_LED_STATUS, 0, 60, 0);
                }
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - window_start >= 5000000 && infer_count > 0) {
            ESP_LOGI(TAG, "inference: %.1f fps, %.0f ms avg, hands=%d+%d, best score %.2f",
                     infer_count * 1e6f / (now - window_start),
                     infer_us_sum / 1000.0f / infer_count,
                     (int)hands[0].present, (int)hands[1].present, best_seen);
            infer_count = 0;
            infer_us_sum = 0;
            best_seen = 0.0f;
            window_start = now;
        }
    }
}

esp_err_t tracker_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_frame_ready = xSemaphoreCreateBinary();

    jpeg_decode_engine_cfg_t eng = {};
    eng.intr_priority = 0;
    eng.timeout_ms = 100;
    if (jpeg_new_decoder_engine(&eng, &s_dec) != ESP_OK) {
        ESP_LOGE(TAG, "jpeg decoder engine failed");
        vSemaphoreDelete(s_mutex);
        vSemaphoreDelete(s_frame_ready);
        return ESP_FAIL;
    }
    jpeg_decode_memory_alloc_cfg_t rx = {};
    rx.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    jpeg_decode_memory_alloc_cfg_t tx = {};
    tx.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
    s_decode_buf = (uint8_t *)jpeg_alloc_decoder_mem(SRC_W * SRC_H * 3, &rx, &s_decode_cap);
    s_jpeg_in = (uint8_t *)jpeg_alloc_decoder_mem(JPEG_IN_MAX, &tx, &s_jpeg_in_cap);
    s_det_buf = (uint8_t *)heap_caps_malloc(DET_W * DET_H * 3, MALLOC_CAP_SPIRAM);
    if (!s_decode_buf || !s_jpeg_in || !s_det_buf) {
        ESP_LOGE(TAG, "buffer alloc failed");
        jpeg_del_decoder_engine(s_dec);
        heap_caps_free(s_decode_buf);
        heap_caps_free(s_jpeg_in);
        heap_caps_free(s_det_buf);
        vSemaphoreDelete(s_mutex);
        vSemaphoreDelete(s_frame_ready);
        return ESP_ERR_NO_MEM;
    }

    /* Heavy inference lives on core 1 below the synth task, which preempts
     * at will (it spends most time blocked on the I2S DMA anyway). */
    BaseType_t ok = xTaskCreatePinnedToCore(tracker_task, "tracker", 12288, NULL, 5, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
