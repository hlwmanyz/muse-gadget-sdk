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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared state between the voice pipeline (writer) and the UI (reader).
 * Scalars are plain word-sized stores; the caption is guarded by a spinlock.
 */

typedef enum {
    MUSE_MODE_BOOT = 0,
    MUSE_MODE_IDLE,
    MUSE_MODE_LISTENING,
    MUSE_MODE_THINKING,
    MUSE_MODE_SPEAKING,
    MUSE_MODE_ERROR,
    MUSE_MODE_OFF,       /* powering down */
    MUSE_MODE_COUNT,
} muse_mode_t;

typedef struct {
    int battery_pct;   /* -1 when no battery is connected */
    int battery_mv;    /* 0 when the board can't measure it */
    bool charging;
    bool usb;
} muse_power_t;

void muse_state_init(void);

void muse_state_set_mode(muse_mode_t mode);
muse_mode_t muse_state_mode(float *secs_in_mode);

/* Live audio level (mic while listening, playback while speaking), 0..1. */
void muse_state_set_level(float level);
float muse_state_level(void);

/* Progress of the current phase (recording length or playback), 0..1. */
void muse_state_set_progress(float progress);
float muse_state_progress(void);

/* Room for a page of reply text; longer captions are cut short. */
#define MUSE_CAPTION_MAX 400

void muse_state_set_caption(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Copies the caption if it changed since *version; returns true on change. */
bool muse_state_caption(char *out, size_t out_len, uint32_t *version);

/* Fraction through the spoken message the caption sits at, for the chat view's
 * scroll walk. A plain live value, not versioned: the walk reads it every frame. */
void muse_state_set_caption_progress(float progress);
float muse_state_caption_progress(void);

/* How much reply text the screen shows at once, set by the UI: lines of up to
 * `cols` characters. The replies are wrapped and paged to fit. A screen that
 * draws CJK larger than its other caption text sets a page for replies with CJK
 * in them too; without one they get the usual page. */
void muse_state_set_page(int cols, int lines);
void muse_state_set_cjk_page(int cols, int lines);
void muse_state_page(bool cjk, int *cols, int *lines);

/*
 * The conversation log behind the chat view (CONFIG_MUSE_CHAT_VIEW): the
 * voice pipeline posts each side of a turn as it lands -- what Muse heard,
 * then the reply -- and the UI reads entries back by sequence number.
 * Only the newest MUSE_CHAT_MAX are kept. Storage is PSRAM, allocated on
 * the first post; without PSRAM the log stays empty and posts are dropped.
 * Texts are kept as posted (the caption path folds to ASCII; this doesn't).
 */
typedef enum {
    MUSE_CHAT_USER = 0,   /* what the person said, shown on the right */
    MUSE_CHAT_ASSISTANT,  /* Muse's reply, shown on the left */
} muse_chat_role_t;

#define MUSE_CHAT_MAX 20         /* messages kept */
#define MUSE_CHAT_TEXT_MAX 1024  /* bytes per message, cut on a UTF-8 edge */

void muse_state_chat_post(muse_chat_role_t role, const char *text);
/* Messages ever posted; also the sequence number of the next one. */
uint32_t muse_state_chat_count(void);
/* Copies message `seq` out; false when it was already evicted (or isn't
 * there yet). */
bool muse_state_chat_get(uint32_t seq, muse_chat_role_t *role, char *out, size_t cap);

void muse_state_set_power(const muse_power_t *power);
muse_power_t muse_state_power(void);
/* A battery and no USB power (charger or computer). Power saving asleep (the
 * voice task resting, the display paused, the Wi-Fi nap) is only for then;
 * a change nudges (muse_state_wait_awake). */
bool muse_state_on_battery(void);
/* Bench tests over USB (">nap"): count as on battery until turned off. */
void muse_state_set_as_if_battery(bool on);

/* Any user interaction: keeps Muse awake. */
void muse_state_poke(void);
float muse_state_idle_secs(void);

/* Screen sleep (display dark, rendering paused). Waking also pokes. */
void muse_state_set_asleep(bool asleep);
bool muse_state_asleep(void);
/* For a task with nothing to do asleep: returns once awake, nudged
 * (muse_state_nudge, which doesn't wake the screen) or timeout_ms passes. */
void muse_state_wait_awake(uint32_t timeout_ms);
void muse_state_nudge(void);

/* Touch/pet reaction. */
void muse_state_make_happy(void);
float muse_state_happiness(void);

#ifdef __cplusplus
}
#endif
