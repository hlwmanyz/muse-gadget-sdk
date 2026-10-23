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

#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * An OV2640 on its parallel (DVP) bus, through Espressif's esp32-camera
 * driver: the DIY S3CAM board's camera. The board supplies the lines.
 * PWDN and RESET are -1 where they aren't wired (this board: both), so the
 * sensor can't be powered down between captures; it draws its idle current
 * and only clocks frames out for a capture or the preview stream.
 *
 * still_size and stream_size are framesize_t values (FRAMESIZE_* from
 * esp_camera.h); 0 takes the defaults: VGA stills, 240x240 streamed frames
 * (the board's screen is 240x240). Plain ints keep this header free of the
 * esp32-camera headers, which only the backend itself needs.
 */

typedef struct {
    const char *name;               /* camera_driver_t.name */
    int xclk, pclk, vsync, href;    /* clock and sync lines */
    int sccb_sda, sccb_scl;         /* the SCCB control bus */
    int d0, d1, d2, d3, d4, d5, d6, d7;
    int pwdn, reset;                /* -1 when not wired */
    int still_size;                 /* framesize_t, or 0 for VGA */
    int stream_size;                /* framesize_t, or 0 for 240x240 */
} camera_ov2640_config_t;

/* The backend for `config` (copied), to pass to camera_register(). One per device. */
const camera_driver_t *camera_ov2640(const camera_ov2640_config_t *config);

#ifdef __cplusplus
}
#endif
