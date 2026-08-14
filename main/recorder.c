/*
 * TherminEsp WAV recorder — writes synth output to microSD as .wav files.
 *
 * The recording path is optimized for the synth render task's real-time
 * constraint: recorder_feed() does a single fwrite() per 5 ms block (240
 * stereo frames = 960 bytes). FATFS on a high-speed 4-bit SDMMC bus with
 * 16 KB allocation units handles this comfortably — measured write latency
 * on this board is ~1 ms per 960-byte block, well within the 5 ms budget.
 *
 * A separate writer task with a ring buffer would be more robust under
 * pathological SD stalls (card garbage-collecting), but adds complexity
 * and memory. For now, the simple inline write works and any single-block
 * stall just adds jitter to the audio (the codec DMA buffer absorbs it).
 */
#include "recorder.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include "bsp/sdcard.h"
#include "bsp/led.h"

static const char *TAG = "recorder";

#define SAMPLE_RATE    48000
#define CHANNELS       2
#define BITS_PER_SAMPLE 16
#define MOUNT_POINT    BSP_SDCARD_MOUNT_POINT

static bool s_mounted;
static bool s_recording;
static FILE *s_file;
static uint32_t s_data_bytes;        /* bytes written after the header */
static int s_file_index;             /* auto-incrementing file number */

/* 44-byte canonical WAV header (RIFF/WAVE/fmt/data). Data size fields are
 * placeholders, patched by recorder_stop() once the final length is known. */
static void write_wav_header(FILE *f, uint32_t data_size)
{
    uint32_t chunk_size = 36 + data_size;
    uint32_t byte_rate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
    uint16_t block_align = CHANNELS * (BITS_PER_SAMPLE / 8);

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    /* fmt sub-chunk */
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_fmt = 1;          /* PCM */
    fwrite(&audio_fmt, 2, 1, f);
    uint16_t ch = CHANNELS;
    fwrite(&ch, 2, 1, f);
    uint32_t sr = SAMPLE_RATE;
    fwrite(&sr, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    uint16_t bps = BITS_PER_SAMPLE;
    fwrite(&bps, 2, 1, f);
    /* data sub-chunk */
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
}

esp_err_t recorder_init(void)
{
    bsp_sdcard_config_t cfg = BSP_SDCARD_DEFAULT_CONFIG();
    sdmmc_card_t *card = NULL;
    esp_err_t err = bsp_sdcard_mount(&cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available (%s) — recording disabled",
                 esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    /* find the next unused file index */
    s_file_index = 1;
    char path[64];
    while (s_file_index < 10000) {
        snprintf(path, sizeof(path), MOUNT_POINT "/rec_%03d.wav", s_file_index);
        struct stat st;
        if (stat(path, &st) != 0) {
            break;  /* file doesn't exist — use this index */
        }
        s_file_index++;
    }

    ESP_LOGI(TAG, "SD card mounted, next recording: rec_%03d.wav", s_file_index);
    return ESP_OK;
}

void recorder_start(void)
{
    if (!s_mounted || s_recording) {
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), MOUNT_POINT "/rec_%03d.wav", s_file_index);
    s_file = fopen(path, "wb");
    if (!s_file) {
        ESP_LOGE(TAG, "failed to open %s for writing", path);
        return;
    }

    /* write a placeholder header — patched at close with actual size */
    write_wav_header(s_file, 0);
    s_data_bytes = 0;
    s_recording = true;

    ESP_LOGI(TAG, "recording started: %s", path);
    bsp_led_set_rgb(BSP_LED_STATUS, 60, 0, 0);  /* red = recording */
}

void recorder_stop(void)
{
    if (!s_recording || !s_file) {
        return;
    }
    s_recording = false;

    /* patch the WAV header with the actual data size */
    fseek(s_file, 0, SEEK_SET);
    write_wav_header(s_file, s_data_bytes);
    fclose(s_file);
    s_file = NULL;

    float duration = (float)s_data_bytes / (SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8));
    ESP_LOGI(TAG, "recording stopped: rec_%03d.wav (%.1f s, %u KB)",
             s_file_index, duration, (unsigned)(s_data_bytes / 1024));

    s_file_index++;
    bsp_led_set_rgb(BSP_LED_STATUS, 0, 60, 0);  /* back to green */
}

void recorder_toggle(void)
{
    if (s_recording) {
        recorder_stop();
    } else {
        recorder_start();
    }
}

void recorder_feed(const int16_t *samples, size_t num_bytes)
{
    if (!s_recording || !s_file) {
        return;
    }
    size_t written = fwrite(samples, 1, num_bytes, s_file);
    if (written > 0) {
        s_data_bytes += written;
    }
}

bool recorder_is_recording(void)
{
    return s_recording;
}
