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

#include "muse_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "muse_text.h"

#define HAPPY_SECS 1.6f

#define AWAKE_BIT BIT0   /* while !s_asleep */
#define NUDGE_BIT BIT1

static volatile muse_mode_t s_mode = MUSE_MODE_BOOT;
static volatile int64_t s_mode_since_us;
static volatile float s_level;
static volatile float s_progress;
static volatile int64_t s_last_poke_us;
static volatile int64_t s_happy_until_us;
static volatile bool s_asleep;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_caption[MUSE_CAPTION_MAX];
static uint32_t s_caption_version;
static float s_caption_progress;
static SemaphoreHandle_t s_format_lock;
static EventGroupHandle_t s_wake;
static volatile int s_page_cols = 16, s_page_lines = 2;
static volatile int s_cjk_cols, s_cjk_lines;
static muse_power_t s_power = { .battery_pct = -1 };
static volatile bool s_as_if_battery;

static float secs_since(int64_t us)
{
    return (float)(esp_timer_get_time() - us) / 1e6f;
}

void muse_state_init(void)
{
    static StaticSemaphore_t lock;
    s_format_lock = xSemaphoreCreateMutexStatic(&lock);
    static StaticEventGroup_t wake;
    s_wake = xEventGroupCreateStatic(&wake);
    xEventGroupSetBits(s_wake, AWAKE_BIT);
    int64_t now = esp_timer_get_time();
    s_mode_since_us = now;
    s_last_poke_us = now;
}

void muse_state_set_mode(muse_mode_t mode)
{
    if (mode == s_mode) {
        return;
    }
    /* Shutting down wins over the voice pipeline finishing a turn. */
    if (s_mode == MUSE_MODE_OFF && mode != MUSE_MODE_IDLE) {
        return;
    }
    s_mode_since_us = esp_timer_get_time();
    s_mode = mode;
}

muse_mode_t muse_state_mode(float *secs_in_mode)
{
    if (secs_in_mode) {
        *secs_in_mode = secs_since(s_mode_since_us);
    }
    return s_mode;
}

void muse_state_set_level(float level)
{
    s_level = level < 0 ? 0 : (level > 1 ? 1 : level);
}

float muse_state_level(void)
{
    return s_level;
}

void muse_state_set_progress(float progress)
{
    s_progress = progress < 0 ? 0 : (progress > 1 ? 1 : progress);
}

float muse_state_progress(void)
{
    return s_progress;
}

void muse_state_set_caption(const char *fmt, ...)
{
    static char buf[sizeof(s_caption)];   /* too big for some callers' stacks */
    xSemaphoreTake(s_format_lock, portMAX_DELAY);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    muse_text_to_ascii(buf, sizeof(buf));   /* replies have curly quotes and dashes */

    portENTER_CRITICAL(&s_lock);
    if (strcmp(buf, s_caption) != 0) {
        memcpy(s_caption, buf, sizeof(s_caption));
        s_caption_version++;
    }
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_format_lock);
}

bool muse_state_caption(char *out, size_t out_len, uint32_t *version)
{
    bool changed = false;
    portENTER_CRITICAL(&s_lock);
    if (*version != s_caption_version) {
        strlcpy(out, s_caption, out_len);
        *version = s_caption_version;
        changed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return changed;
}

void muse_state_set_caption_progress(float progress)
{
    s_caption_progress = progress;
}

float muse_state_caption_progress(void)
{
    return s_caption_progress;
}

void muse_state_set_page(int cols, int lines)
{
    s_page_cols = cols;
    s_page_lines = lines;
}

void muse_state_set_cjk_page(int cols, int lines)
{
    s_cjk_cols = cols;
    s_cjk_lines = lines;
}

void muse_state_page(bool cjk, int *cols, int *lines)
{
    bool own = cjk && s_cjk_cols > 0;
    *cols = own ? s_cjk_cols : s_page_cols;
    *lines = own ? s_cjk_lines : s_page_lines;
}

/* ---- Conversation log (chat view) ---- */

typedef struct {
    muse_chat_role_t role;
    char text[MUSE_CHAT_TEXT_MAX];
} chat_msg_t;

static chat_msg_t *s_chat;      /* PSRAM ring, MUSE_CHAT_MAX entries */
static uint32_t s_chat_seq;     /* messages ever posted */

void muse_state_chat_post(muse_chat_role_t role, const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    if (!s_chat) {
        s_chat = heap_caps_calloc(MUSE_CHAT_MAX, sizeof(chat_msg_t), MALLOC_CAP_SPIRAM);
        if (!s_chat) {
            portEXIT_CRITICAL(&s_lock);
            return;   /* no PSRAM: the log stays empty */
        }
    }
    chat_msg_t *m = &s_chat[s_chat_seq % MUSE_CHAT_MAX];
    m->role = role;
    /* Cut at the cap on a UTF-8 edge, so a bubble never ends mid-character. */
    size_t n = strlen(text);
    if (n >= sizeof(m->text)) {
        n = sizeof(m->text) - 1;
        while (n && (text[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(m->text, text, n);
    m->text[n] = '\0';
    s_chat_seq++;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t muse_state_chat_count(void)
{
    return s_chat_seq;
}

bool muse_state_chat_get(uint32_t seq, muse_chat_role_t *role, char *out, size_t cap)
{
    bool ok = false;
    portENTER_CRITICAL(&s_lock);
    if (s_chat && seq < s_chat_seq && s_chat_seq - seq <= MUSE_CHAT_MAX) {
        const chat_msg_t *m = &s_chat[seq % MUSE_CHAT_MAX];
        if (role) {
            *role = m->role;
        }
        if (out && cap) {
            strlcpy(out, m->text, cap);
        }
        ok = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

void muse_state_set_power(const muse_power_t *power)
{
    bool was = muse_state_on_battery();
    portENTER_CRITICAL(&s_lock);
    s_power = *power;
    portEXIT_CRITICAL(&s_lock);
    if (muse_state_on_battery() != was) {
        muse_state_nudge();
    }
}

muse_power_t muse_state_power(void)
{
    portENTER_CRITICAL(&s_lock);
    muse_power_t p = s_power;
    portEXIT_CRITICAL(&s_lock);
    return p;
}

bool muse_state_on_battery(void)
{
    muse_power_t p = muse_state_power();
    return s_as_if_battery || (!p.usb && p.battery_pct >= 0);
}

void muse_state_set_as_if_battery(bool on)
{
    if (on != s_as_if_battery) {
        s_as_if_battery = on;
        muse_state_nudge();
    }
}

void muse_state_poke(void)
{
    s_last_poke_us = esp_timer_get_time();
}

float muse_state_idle_secs(void)
{
    return secs_since(s_last_poke_us);
}

void muse_state_set_asleep(bool asleep)
{
    if (!asleep) {
        muse_state_poke();
    }
    s_asleep = asleep;
    if (asleep) {
        xEventGroupClearBits(s_wake, AWAKE_BIT);
    } else {
        xEventGroupSetBits(s_wake, AWAKE_BIT);
    }
}

bool muse_state_asleep(void)
{
    return s_asleep;
}

void muse_state_wait_awake(uint32_t timeout_ms)
{
    xEventGroupWaitBits(s_wake, AWAKE_BIT | NUDGE_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    xEventGroupClearBits(s_wake, NUDGE_BIT);
}

void muse_state_nudge(void)
{
    xEventGroupSetBits(s_wake, NUDGE_BIT);
}

void muse_state_make_happy(void)
{
    s_happy_until_us = esp_timer_get_time() + (int64_t)(HAPPY_SECS * 1e6f);
    muse_state_poke();
}

float muse_state_happiness(void)
{
    float left = (float)(s_happy_until_us - esp_timer_get_time()) / 1e6f;
    if (left <= 0) {
        return 0;
    }
    /* Ease out over the last 0.4 s. */
    return left > 0.4f ? 1.0f : left / 0.4f;
}
