/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "muse_camera.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "muse_board.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "rom/tjpgd.h"

/*
 * The DIY S3CAM's camera.capture and live preview, on the board's camera
 * (components/camera; board_muse_s3cam_diy.c registers the OV2640). Same
 * shape as the Watcher's glue (boards/watcher_camera.c), with one board
 * difference: streamed frames are already the screen's 240x240. The board
 * has no touch and its one button is the talk button, so the preview is
 * worked by the camera.preview command instead; a still can be taken with
 * camera.capture at any time (during the preview it returns the frame on
 * screen). The sensor is wired without PWDN, so it stays powered; frames
 * are only clocked out while a capture or the preview runs.
 */

static const char *TAG = "s3cam.camera";
static SemaphoreHandle_t s_lock;     /* s_last */
static uint8_t *s_last;              /* the preview's newest frame, a JPEG */
static size_t s_last_len;
static atomic_bool s_preview_active;
static atomic_bool s_busy;           /* a capture, or the preview starting or stopping */
static esp_timer_handle_t s_preview_timer;   /* closes a forgotten preview */

/* The preview is opened by command (camera.preview), not by a button on
 * this board, so nothing local closes it: left alone it would hold the
 * sensor (and its DMA buffers) forever. Give it 90 seconds, then close
 * it like a second toggle. */
#define CAMERA_PREVIEW_TIMEOUT_US (90LL * 1000 * 1000)
static void preview_timeout(void *arg)
{
    (void)arg;
    if (s_preview_active) muse_camera_preview_toggle();
}

#define CAMERA_PREVIEW_SIZE 240
#define CAMERA_JPEG_POOL 3100

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t offset;
} jpeg_source_t;

static UINT jpeg_read(JDEC *jd, BYTE *buffer, UINT count)
{
    jpeg_source_t *src = jd->device;
    size_t remain = src->length - src->offset;
    if (count > remain) count = remain;
    if (buffer && count) memcpy(buffer, src->data + src->offset, count);
    src->offset += count;
    return count;
}

static UINT jpeg_draw(JDEC *jd, void *bitmap, JRECT *rect)
{
    int left = rect->left;
    int top = rect->top;
    int right = rect->right > CAMERA_PREVIEW_SIZE - 1 ? CAMERA_PREVIEW_SIZE - 1 : rect->right;
    int bottom = rect->bottom > CAMERA_PREVIEW_SIZE - 1 ? CAMERA_PREVIEW_SIZE - 1 : rect->bottom;
    if (left > right || top > bottom) return 1;

    int source_w = rect->right - rect->left + 1;
    int width = right - left + 1;
    int height = bottom - top + 1;
    uint16_t pixels[16 * 16];
    uint8_t *pixel_bytes = (uint8_t *)pixels;
    const uint8_t *rgb = bitmap;
    for (int y = top; y <= bottom; y++) {
        for (int x = left; x <= right; x++) {
            size_t si = ((size_t)(y - rect->top) * source_w + (x - rect->left)) * 3;
            size_t di = ((size_t)(y - top) * width + (x - left)) * 2;
            uint16_t px = ((rgb[si] & 0xF8) << 8) | ((rgb[si + 1] & 0xFC) << 3) | (rgb[si + 2] >> 3);
            pixel_bytes[di] = px >> 8;
            pixel_bytes[di + 1] = px & 0xFF;
        }
    }
    muse_ui_image_draw(left, top, width, height, pixels);
    return 1;
}

static void preview_draw(const uint8_t *jpeg, size_t len)
{
    void *pool = heap_caps_malloc(CAMERA_JPEG_POOL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pool) return;
    jpeg_source_t src = { .data = jpeg, .length = len };
    JDEC jd;
    if (jd_prepare(&jd, jpeg_read, pool, CAMERA_JPEG_POOL, &src) == JDR_OK) {
        if (jd.width > CAMERA_PREVIEW_SIZE || jd.height > CAMERA_PREVIEW_SIZE) {
            muse_state_set_caption("CAMERA FRAME TOO LARGE");
        } else {
            jd_decomp(&jd, jpeg_draw, 0);
        }
    }
    free(pool);
}

/* Each streamed frame: kept for a capture during the preview, and drawn. */
static void on_frame(const camera_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (!s_preview_active) return;
    uint8_t *copy = heap_caps_malloc(frame->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy) {
        memcpy(copy, frame->jpeg, frame->len);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        free(s_last);
        s_last = copy;
        s_last_len = frame->len;
        xSemaphoreGive(s_lock);
    }
    preview_draw(frame->jpeg, frame->len);
}

esp_err_t muse_camera_prepare(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    if (!s_preview_timer) {
        const esp_timer_create_args_t args = { .callback = preview_timeout, .name = "cam_prev" };
        esp_timer_create(&args, &s_preview_timer);   /* best effort: the preview still works without it */
    }
    return ESP_OK;
}

/* `jpeg` as base64, for camera.capture's result. */
static char *to_base64(const uint8_t *jpeg, size_t len)
{
    size_t cap = (len + 2) / 3 * 4 + 1, out = 0;
    char *b64 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b64 && mbedtls_base64_encode((unsigned char *)b64, cap, &out, jpeg, len) != 0) {
        free(b64);
        return NULL;
    }
    return b64;
}

static bool capture_frame(char **jpeg_base64, const char **error)
{
    if (s_preview_active) {
        /* The preview has the camera: the frame it's showing is the photo. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        *jpeg_base64 = s_last ? to_base64(s_last, s_last_len) : NULL;
        bool had = s_last != NULL;
        xSemaphoreGive(s_lock);
        *error = !had ? "no camera frame yet" : *jpeg_base64 ? NULL : "camera memory allocation failed";
        return *jpeg_base64 != NULL;
    }
    camera_frame_t frame;
    esp_err_t err = camera_capture(&frame);
    if (err != ESP_OK) {
        *error = err == ESP_ERR_TIMEOUT ? "OV2640 camera did not respond"
                 : err == ESP_ERR_NO_MEM ? "camera memory allocation failed"
                 : err == ESP_ERR_NOT_SUPPORTED ? "camera unavailable"
                                                 : "camera capture failed";
        return false;
    }
    *jpeg_base64 = to_base64(frame.jpeg, frame.len);
    camera_release(&frame);
    /* Power the sensor down before the result is queued: while it stays
     * up, its DMA buffers keep DMA memory under the control session's
     * send reserve and the result would never leave. */
    camera_stop();
    *error = *jpeg_base64 ? NULL : "camera memory allocation failed";
    return *jpeg_base64 != NULL;
}

bool muse_camera_capture(char **jpeg_base64, const char **error)
{
    if (!jpeg_base64 || !error) return false;
    *jpeg_base64 = NULL;
    if (muse_camera_prepare() != ESP_OK) {
        *error = "camera memory allocation failed";
        return false;
    }
    if (atomic_exchange(&s_busy, true)) {
        *error = "camera busy";
        return false;
    }
    bool ok = capture_frame(jpeg_base64, error);
    atomic_store(&s_busy, false);
    return ok;
}

static void preview_toggle_task(void *arg)
{
    (void)arg;
    if (!s_preview_active) {
        /* The view first, so the stream's first frames land on it. */
        s_preview_active = true;
        muse_state_set_caption("LIVE VIEW");
        muse_ui_image_hide();
        muse_board->display_lock(-1);
        muse_ui_show_face();
        muse_board->display_unlock();
        esp_err_t err = camera_stream_start(on_frame, NULL);
        if (err != ESP_OK) {
            s_preview_active = false;
            ESP_LOGW(TAG, "preview didn't start: %s", esp_err_to_name(err));
            muse_state_set_caption(err == ESP_ERR_TIMEOUT ? "OV2640 CAMERA NOT READY" : "CAMERA PREVIEW FAILED");
        } else if (s_preview_timer) {
            esp_timer_start_once(s_preview_timer, CAMERA_PREVIEW_TIMEOUT_US);
        }
    } else {
        if (s_preview_timer) esp_timer_stop(s_preview_timer);
        s_preview_active = false;
        camera_stream_stop();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        free(s_last);
        s_last = NULL;
        s_last_len = 0;
        xSemaphoreGive(s_lock);
        muse_ui_image_hide();
        muse_board->display_lock(-1);
        muse_ui_show_face();
        muse_board->display_unlock();
        muse_state_set_caption("PREVIEW OFF");
    }
    atomic_store(&s_busy, false);
    vTaskDeleteWithCaps(NULL);
}

bool muse_camera_preview_active(void)
{
    return s_preview_active;
}

void muse_camera_preview_toggle(void)
{
    if (muse_camera_prepare() != ESP_OK) {
        muse_state_set_caption("CAMERA MEMORY ERROR");
        return;
    }
    if (atomic_exchange(&s_busy, true)) return;
    if (xTaskCreateWithCaps(preview_toggle_task, "camera_view", 8192, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        != pdPASS) {
        ESP_LOGE(TAG, "could not start camera preview task");
        atomic_store(&s_busy, false);
    }
}
