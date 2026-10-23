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
 * The wake-word detector (Espressif WakeNet; CONFIG_MUSE_WAKEWORD). The
 * voice task feeds it the idle audio it already captures, one chunk at a
 * time, from that task only. feed() returns true exactly once per
 * detection, on the chunk that completes the word; afterwards it stays
 * quiet until reset(), so a word and the speech after it can't retrigger
 * while the turn it started is still being handled. If the model can't
 * load (no model partition flashed, say), feed() returns false forever
 * and the talk button is unaffected.
 */
bool muse_wakenet_feed(const int16_t *pcm, size_t samples);
void muse_wakenet_reset(void);
bool muse_wakenet_ready(void);

#ifdef __cplusplus
}
#endif
