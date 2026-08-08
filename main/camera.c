#include "camera.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/camera.h"

static const char *TAG = "camera";

static bsp_camera_t *s_cam;
static bsp_camera_format_t s_fmt;
static bool s_is_jpeg;
static camera_frame_cb_t s_cb;
static void *s_cb_ctx;

static void camera_task(void *arg)
{
    uint32_t frames = 0;
    int64_t window_start = esp_timer_get_time();

    while (true) {
        bsp_camera_frame_t frame;
        if (bsp_camera_get_frame(s_cam, &frame) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        camera_frame_view_t view = {
            .data = frame.data,
            .size = frame.size,
            .width = s_fmt.width,
            .height = s_fmt.height,
            .is_jpeg = s_is_jpeg,
        };
        if (s_cb) {
            s_cb(&view, s_cb_ctx);
        }
        bsp_camera_return_frame(s_cam, &frame);

        frames++;
        int64_t now = esp_timer_get_time();
        if (now - window_start >= 5000000) {
            ESP_LOGI(TAG, "capture: %.1f fps (%ux%u %s)",
                     frames * 1e6f / (now - window_start),
                     (unsigned)s_fmt.width, (unsigned)s_fmt.height,
                     s_is_jpeg ? "JPEG" : "RGB565");
            frames = 0;
            window_start = now;
        }
    }
}

static esp_err_t try_open(uint32_t w, uint32_t h, bsp_camera_pixel_format_t pf)
{
    bsp_camera_config_t cfg = {
        .width = w,
        .height = h,
        .pixel_format = pf,
        .xclk_freq_hz = BSP_CAMERA_DEFAULT_XCLK_FREQ_HZ,
    };
    esp_err_t e = bsp_camera_open(&cfg, &s_cam);
    if (e != ESP_OK) {
        s_cam = NULL;
        return e;
    }
    e = bsp_camera_start(s_cam);
    if (e != ESP_OK) {
        bsp_camera_close(s_cam);
        s_cam = NULL;
        return e;
    }
    bsp_camera_get_format(s_cam, &s_fmt);
    return ESP_OK;
}

esp_err_t camera_start(camera_frame_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;

    /* Preferred: small RGB565 frames the detector can eat directly. */
    esp_err_t e = try_open(240, 240, BSP_CAMERA_PIXEL_FORMAT_RGB565_BE);
    if (e == ESP_OK) {
        s_is_jpeg = false;
        ESP_LOGI(TAG, "opened 240x240 RGB565 (negotiated %ux%u, %u bytes/frame)",
                 (unsigned)s_fmt.width, (unsigned)s_fmt.height,
                 (unsigned)s_fmt.sizeimage);
    } else {
        /* Fallback proven by the wedding recorder: 720p JPEG. */
        ESP_LOGW(TAG, "RGB565 240x240 open failed (%s), falling back to 720p JPEG",
                 esp_err_to_name(e));
        e = try_open(1280, 720, BSP_CAMERA_PIXEL_FORMAT_JPEG);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "camera open failed entirely: %s", esp_err_to_name(e));
            return e;
        }
        s_is_jpeg = true;
        ESP_LOGI(TAG, "opened 1280x720 JPEG (negotiated %ux%u)",
                 (unsigned)s_fmt.width, (unsigned)s_fmt.height);
    }

    BaseType_t ok = xTaskCreatePinnedToCore(camera_task, "camera", 4096, NULL, 5, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
