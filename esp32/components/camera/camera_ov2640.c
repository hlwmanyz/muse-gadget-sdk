/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "camera_ov2640.h"

#include <stdbool.h>

#include "esp_camera.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "camera_ov2640";

#define XCLK_HZ (20 * 1000 * 1000)
/* The board's backlight PWM owns LEDC timer 0 / channel 0 (LOW_SPEED);
 * the sensor's XCLK takes timer 1 / channel 1. */
#define CAM_LEDC_TIMER LEDC_TIMER_1
#define CAM_LEDC_CHANNEL LEDC_CHANNEL_1
#define JPEG_QUALITY 12
#define STREAM_STACK 8192
#define STREAM_STOP_MS 5000

static camera_ov2640_config_t s_cfg;
static bool s_inited;
static framesize_t s_size;          /* the sensor's current frame size */
static bool s_size_set;

static framesize_t still_size(void)
{
    return s_cfg.still_size ? (framesize_t)s_cfg.still_size : FRAMESIZE_VGA;
}

static framesize_t stream_size(void)
{
    return s_cfg.stream_size ? (framesize_t)s_cfg.stream_size : FRAMESIZE_240X240;
}

static esp_err_t ov2640_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    const camera_config_t cfg = {
        .pin_pwdn = s_cfg.pwdn,
        .pin_reset = s_cfg.reset,
        .pin_xclk = s_cfg.xclk,
        .pin_sccb_sda = s_cfg.sccb_sda,
        .pin_sccb_scl = s_cfg.sccb_scl,
        .pin_d7 = s_cfg.d7,
        .pin_d6 = s_cfg.d6,
        .pin_d5 = s_cfg.d5,
        .pin_d4 = s_cfg.d4,
        .pin_d3 = s_cfg.d3,
        .pin_d2 = s_cfg.d2,
        .pin_d1 = s_cfg.d1,
        .pin_d0 = s_cfg.d0,
        .pin_vsync = s_cfg.vsync,
        .pin_href = s_cfg.href,
        .pin_pclk = s_cfg.pclk,
        .xclk_freq_hz = XCLK_HZ,
        .ledc_timer = CAM_LEDC_TIMER,
        .ledc_channel = CAM_LEDC_CHANNEL,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = still_size(),
        .jpeg_quality = JPEG_QUALITY,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = esp_camera_init(&cfg);
    if (err == ESP_OK) {
        s_inited = true;
        s_size = still_size();
        s_size_set = true;
        ESP_LOGI(TAG, "OV2640 up: XCLK %d Hz on LEDC timer %d", XCLK_HZ, (int)CAM_LEDC_TIMER);
    } else {
        ESP_LOGW(TAG, "OV2640 didn't start: %s", esp_err_to_name(err));
    }
    return err;
}

/* Switches the sensor's frame size (capture and stream sizes differ), and
 * drops the first frame afterwards: it was exposed for the old size and the
 * auto-exposure needs a frame to settle. Only called with the camera held
 * (camera.c's one-holder rule), so no frame is out meanwhile. */
static esp_err_t set_size(framesize_t size)
{
    if (s_size_set && s_size == size) {
        return ESP_OK;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    ESP_RETURN_ON_FALSE(sensor, ESP_ERR_INVALID_STATE, TAG, "no sensor");
    ESP_RETURN_ON_ERROR(sensor->set_framesize(sensor, size), TAG, "set framesize");
    s_size = size;
    s_size_set = true;
    camera_fb_t *stale = esp_camera_fb_get();
    if (stale) {
        esp_camera_fb_return(stale);
    }
    return ESP_OK;
}

static esp_err_t ov2640_capture(camera_frame_t *out)
{
    int64_t t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(set_size(still_size()), TAG, "still size");
    camera_fb_t *fb = esp_camera_fb_get();
    ESP_RETURN_ON_FALSE(fb, ESP_ERR_TIMEOUT, TAG, "no frame");
    out->jpeg = fb->buf;
    out->len = fb->len;
    out->width = fb->width;
    out->height = fb->height;
    out->priv = fb;
    ESP_LOGI(TAG, "%dx%d JPEG, %u bytes in %lld ms", out->width, out->height, (unsigned)out->len,
             (esp_timer_get_time() - t0) / 1000);
    return ESP_OK;
}

static void ov2640_release(camera_frame_t *frame)
{
    if (frame->priv) {
        esp_camera_fb_return((camera_fb_t *)frame->priv);
    }
}

/* ---- Stream ---- */

static camera_frame_cb_t s_on_frame;
static void *s_frame_ctx;
static volatile bool s_stop;
static SemaphoreHandle_t s_stopped;

/* Each frame the sensor clocks out goes to s_on_frame, until s_stop. With a
 * single frame buffer (fb_count 1) the next frame only starts once this one
 * is handed back, which paces the stream to what the callback can draw. */
static void stream_task(void *arg)
{
    (void)arg;
    while (!s_stop) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const camera_frame_t frame = {
            .jpeg = fb->buf,
            .len = fb->len,
            .width = fb->width,
            .height = fb->height,
            .priv = fb,
        };
        s_on_frame(&frame, s_frame_ctx);
        esp_camera_fb_return(fb);
    }
    ESP_LOGI(TAG, "stream stopped");
    xSemaphoreGive(s_stopped);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t ov2640_stream_start(camera_frame_cb_t on_frame, void *ctx)
{
    if (!s_stopped) {
        s_stopped = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_stopped, ESP_ERR_NO_MEM, TAG, "stream semaphore");
    }
    ESP_RETURN_ON_ERROR(set_size(stream_size()), TAG, "stream size");
    s_on_frame = on_frame;
    s_frame_ctx = ctx;
    s_stop = false;
    /* Its stack can be in PSRAM: the frame buffers it handles are PSRAM too. */
    if (xTaskCreateWithCaps(stream_task, "camera_stream", STREAM_STACK, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void ov2640_stream_stop(void)
{
    s_stop = true;
    /* Waits for the task however long it takes: it owns the frame buffer
     * until it comes back, and fb_get is answered by the running sensor. */
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(STREAM_STOP_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "stream slow to stop, still waiting");
        xSemaphoreTake(s_stopped, portMAX_DELAY);
    }
}

static camera_driver_t s_driver;

const camera_driver_t *camera_ov2640(const camera_ov2640_config_t *config)
{
    s_cfg = *config;
    s_driver = (camera_driver_t){
        .name = s_cfg.name,
        .max_width = 640,
        .max_height = 480,
        .init = ov2640_init,
        .capture = ov2640_capture,
        .release = ov2640_release,
        .stream_start = ov2640_stream_start,
        .stream_stop = ov2640_stream_stop,
    };
    return &s_driver;
}
