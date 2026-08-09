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
#define MISS_LIMIT   10             /* ~1.2 s of grace before the note drops */
#define SCORE_THR    0.15f          /* espdet-pico confidence floor */
/* The detector is reliable only in the center of the narrow FOV — hands near
 * the edges (especially the top, reached when raising for volume) drop out.
 * Map this center band to the full [0,1] control range so small, centered
 * movements span the whole instrument and the hand stays where it's seen. */
#define ACTIVE_MARGIN 0.22f
#define EMA_ALPHA    0.5f
/* box area fraction of frame -> openness (fist small, open hand big) */
#define AREA_CLOSED  0.03f
#define AREA_OPEN    0.20f

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
    if (!frame->is_jpeg || frame->size > s_jpeg_in_cap) {
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

/* Debug helper (currently unwired): dump the detector input over serial as
 * base64 so the exact image can be inspected off-board. */
static void __attribute__((unused)) dump_det_frame(void)
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

/* expand the reliable center band [MARGIN, 1-MARGIN] to full [0,1] */
static inline float remap_active(float v)
{
    float out = (v - ACTIVE_MARGIN) / (1.0f - 2.0f * ACTIVE_MARGIN);
    return out < 0 ? 0 : (out > 1 ? 1 : out);
}

static void tracker_task(void *arg)
{
    HandDetect detector;
    detector.set_score_thr(SCORE_THR);
    float ema_x = 0.5f, ema_y = 0.5f, ema_open = 1.0f;
    bool hand_present = false;
    int misses = 0;
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

        const dl::detect::result_t *best = nullptr;
        for (const auto &r : results) {
            if (!best || r.score > best->score) {
                best = &r;
            }
        }
        if (best && best->score > best_seen) {
            best_seen = best->score;
        }

#ifdef CONFIG_THEREMIN_TRACE_FRAMES
        /* per-frame trace: how many camera frames waited (queue depth proxy),
         * inference ms, detection count + best score, hand center */
        if ((frame_n % 3) == 0) {
            ESP_LOGI(TAG, "f#%d dec+infer=%lldms dets=%d score=%.2f pos=(%.2f,%.2f)",
                     frame_n, (t1 - t0) / 1000, (int)results.size(),
                     best ? best->score : 0.0f,
                     best ? (best->box[0] + best->box[2]) * 0.5f / DET_W : 0.0f,
                     best ? (best->box[1] + best->box[3]) * 0.5f / DET_H : 0.0f);
        }
#endif

        if (best) {
            misses = 0;
            float cx = (best->box[0] + best->box[2]) * 0.5f / DET_W;
            float cy = (best->box[1] + best->box[3]) * 0.5f / DET_H;
            float area = (float)best->box_area() / (DET_W * DET_H);
            float open = (area - AREA_CLOSED) / (AREA_OPEN - AREA_CLOSED);
            open = open < 0 ? 0 : (open > 1 ? 1 : open);

            if (!hand_present) {
                ema_x = cx; ema_y = cy; ema_open = open;
                hand_present = true;
            } else {
                ema_x += EMA_ALPHA * (cx - ema_x);
                ema_y += EMA_ALPHA * (cy - ema_y);
                ema_open += EMA_ALPHA * (open - ema_open);
            }

            /* mirror X so moving your hand right raises pitch on screen;
             * remap the reliable center band to the full control range */
            float freq_norm = remap_active(1.0f - ema_x);
            float vol = remap_active(1.0f - ema_y);
            float freq = synth_update_voice(0, freq_norm, vol, ema_open);
            ui_hand_update(true, freq_norm, ema_y, freq, vol);
            bsp_led_set_rgb(BSP_LED_STATUS,
                            (uint8_t)(60 * (1.0f - freq_norm)), 20,
                            (uint8_t)(60 * freq_norm));
        } else if (hand_present && ++misses >= MISS_LIMIT) {
            hand_present = false;
            synth_stop_voice(0);
            ui_hand_update(false, 0, 0, 0, 0);
            bsp_led_set_rgb(BSP_LED_STATUS, 0, 60, 0);
        }

        int64_t now = esp_timer_get_time();
        if (now - window_start >= 5000000 && infer_count > 0) {
            ESP_LOGI(TAG, "inference: %.1f fps, %.0f ms avg, hand=%d, best score %.2f",
                     infer_count * 1e6f / (now - window_start),
                     infer_us_sum / 1000.0f / infer_count, (int)hand_present, best_seen);
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
        return ESP_ERR_NO_MEM;
    }

    /* Heavy inference lives on core 1 below the synth task, which preempts
     * at will (it spends most time blocked on the I2S DMA anyway). */
    BaseType_t ok = xTaskCreatePinnedToCore(tracker_task, "tracker", 12288, NULL, 5, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
