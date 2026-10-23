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

#include "muse_wakenet.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"

#include "muse_mem.h"

static const char *TAG = "muse_wakenet";

/* The model picked in the board's sdkconfig (CONFIG_SR_WN_*): matched
 * against the names in the flashed model data, so changing the word is a
 * config change, not a code change. */
#define WAKE_WORD_KEYWORD "nihaoxiaoxin"

/*
 * The detector, built by muse_wakenet_init(): esp_srmodel_init() reads
 * the "model" partition the flasher wrote srmodels.bin to. A device
 * flashed without it (an app-only update from before the partition
 * existed) finds no WakeNet model, and stays button-only.
 */
static bool s_dead;              /* no model, or it would not start */
static esp_wn_iface_t *s_wn;
static model_iface_data_t *s_data;
static int16_t *s_buf;           /* one detect chunk, in PSRAM */
static int s_chunk;              /* samples per detect() call */
static int s_fill;
static bool s_hit;               /* fired; quiet until muse_wakenet_reset() */

void muse_wakenet_init(void)
{
    if (s_data || s_dead) {
        return;
    }
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGW(TAG, "no model partition: wake word off, the talk button still works");
        s_dead = true;
        return;
    }
    char *name = esp_srmodel_filter(models, ESP_WN_PREFIX, WAKE_WORD_KEYWORD);
    if (!name) {
        ESP_LOGW(TAG, "no %s wake-word model in the model data: wake word off", WAKE_WORD_KEYWORD);
        esp_srmodel_deinit(models);
        s_dead = true;
        return;
    }
    s_wn = (esp_wn_iface_t *)esp_wn_handle_from_name(name);
    if (s_wn) {
        s_data = s_wn->create(name, DET_MODE_95);
    }
    if (!s_wn || !s_data) {
        ESP_LOGW(TAG, "wake-word model %s would not start: wake word off", name);
        esp_srmodel_deinit(models);
        s_dead = true;
        return;
    }
    s_chunk = s_wn->get_samp_chunksize(s_data);
    s_buf = heap_caps_malloc(s_chunk * sizeof(int16_t), MUSE_BIG_CAPS);
    if (!s_buf) {
        ESP_LOGW(TAG, "no memory for the wake-word buffer: wake word off");
        s_wn->destroy(s_data);
        s_data = NULL;
        esp_srmodel_deinit(models);
        s_dead = true;
        return;
    }
    char *words = esp_srmodel_get_wake_words(models, name);
    ESP_LOGI(TAG, "listening for %s (%s, %d samples per pass)",
             words ? words : name, name, s_chunk);
    free(words);
    /* srmodel_list_t itself stays: the model data points into it. */
}

bool muse_wakenet_ready(void)
{
    return s_data != NULL;
}

void muse_wakenet_reset(void)
{
    s_fill = 0;
    s_hit = false;
}

bool muse_wakenet_feed(const int16_t *pcm, size_t samples)
{
    if (s_hit || !s_data) {   /* init failed (s_dead), or never ran */
        return false;
    }
    /* The detector eats fixed-size chunks; idle audio arrives in smaller
     * ones, so accumulate, detecting each time the buffer fills. */
    while (samples) {
        int take = s_chunk - s_fill;
        if (take > (int)samples) {
            take = (int)samples;
        }
        memcpy(s_buf + s_fill, pcm, take * sizeof(int16_t));
        s_fill += take;
        pcm += take;
        samples -= take;
        if (s_fill == s_chunk) {
            s_fill = 0;
            if (s_wn->detect(s_data, s_buf) == WAKENET_DETECTED) {
                ESP_LOGI(TAG, "wake word heard");
                s_hit = true;
                return true;
            }
        }
    }
    return false;
}
