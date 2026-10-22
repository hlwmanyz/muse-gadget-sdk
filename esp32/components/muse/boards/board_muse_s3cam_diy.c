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

/*
 * Bread Compact WiFi S3CAM: ESP32-S3-WROOM-1 (16 MB flash, 8 MB octal
 * PSRAM), 240x320 ST7789 SPI LCD (no touch), I2S MEMS mic, MAX98357A I2S
 * amp driving the speaker, BOOT button on GPIO0, built-in LED on GPIO48
 * (unused: status shows on the display). No codec chip, no battery.
 * Pins follow xiaozhi-esp32's bread-compact-wifi-s3cam board
 * (main/boards/bread-compact-wifi-s3cam/config.h). The OV2640 camera is
 * wired but not used by this port (v1).
 *
 * The display occupies GPIO19/20, the S3's USB D+/D- pins, so the native
 * USB-Serial-JTAG console is unavailable; the console is UART0 behind the
 * board's USB-UART bridge (CONFIG_MUSE_CONSOLE_UART=y in the overlay).
 *
 * Audio: two simplex I2S ports, because the mic and speaker have separate
 * clocks (the speaker on port 0, the mic on port 1, as xiaozhi's
 * NoAudioCodecSimplex does). Both run at 16 kHz, 16-bit, stereo slots,
 * 32 clocks a frame. No codec: the MAX98357A takes I2S as is,
 * esp_codec_dev adds the volume in software, and the mic gain is applied
 * here (same pattern as board_guition_jc3248w535.c, which drives an
 * NS4168 amp without a codec).
 */
#include <math.h>

#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_W 240
#define LCD_H 320
#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_19
#define LCD_MOSI GPIO_NUM_20
#define LCD_CS GPIO_NUM_45
#define LCD_DC GPIO_NUM_47
#define LCD_RST GPIO_NUM_21
#define LCD_BL GPIO_NUM_38
#define DRAW_BUF_LINES 32

#define SPK_BCLK GPIO_NUM_40
#define SPK_WS GPIO_NUM_41   /* LRCK */
#define SPK_DOUT GPIO_NUM_39

#define MIC_SCK GPIO_NUM_2
#define MIC_WS GPIO_NUM_1
#define MIC_DIN GPIO_NUM_42

#define TALK_GPIO GPIO_NUM_0  /* BOOT */

/* An INMP441 is about -26 dBFS at 94 dB SPL; Muse's default 30 dB of gain
 * brings speech up to where the ES8311 boards have it. */
#define MIC_GAIN_OFFSET_DB 0

static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk;
static i2s_chan_handle_t s_tx, s_rx;
static int s_mic_gain_q8 = 256;
static int32_t s_mic_dc[2];            /* each slot's zero level, x256 */

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    /* Held at reset to get here from the bootloader; don't count that as a press. */
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    (void)touch;
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_W * DRAW_BUF_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_err_t rc_reset = esp_lcd_panel_reset(s_panel);
    esp_err_t rc_init = esp_lcd_panel_init(s_panel);
    esp_err_t rc_inv = esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_err_t rc_on = esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "panel bringup: reset=%s init=%s invert=%s on=%s",
             esp_err_to_name(rc_reset), esp_err_to_name(rc_init),
             esp_err_to_name(rc_inv), esp_err_to_name(rc_on));
    /* The LCD sits on GPIO19/20, the S3's native USB D-/D+ pins. If a USB
     * Serial/JTAG console (primary or the IDF-default secondary) is compiled
     * in, it claims those pins and the panel hears nothing. Probe it. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    ESP_LOGW(TAG, "console probe: PRIMARY USB Serial/JTAG console is ON");
#endif
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    ESP_LOGW(TAG, "console probe: SECONDARY USB Serial/JTAG console is ON");
#endif
#if CONFIG_ESP_CONSOLE_UART
    ESP_LOGI(TAG, "console probe: UART console is ON (num %d)", CONFIG_ESP_CONSOLE_UART_NUM);
#endif

    /* One-shot panel bisection v2 (2026-10-22, remove once the screen works):
     * same 4 bands as v1 (which drew nothing visible in run #65), but issued
     * as 20-line sub-draws like xiaozhi's flush / the 2026-10-15 per-line
     * diagnostic that DID light this panel, every return code logged, and
     * held 30 s so the pattern cannot be missed. */
    {
        static const uint16_t band_rgb565[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
        const int sub_h = 20;
        uint16_t *px = heap_caps_malloc(LCD_W * sub_h * sizeof(uint16_t),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (px) {
            int fails = 0;
            for (int b = 0; b < 4; b++) {
                const uint16_t v = band_rgb565[b];
                const uint16_t be = (uint16_t)((v >> 8) | (v << 8));
                for (int i = 0; i < LCD_W * sub_h; i++) {
                    px[i] = be;
                }
                for (int y = b * 80; y < (b + 1) * 80; y += sub_h) {
                    esp_err_t rc = esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W,
                                                             y + sub_h, px);
                    if (rc != ESP_OK) {
                        fails++;
                        ESP_LOGW(TAG, "band draw y=%d rc=%s", y, esp_err_to_name(rc));
                    }
                }
            }
            ESP_LOGI(TAG, "band test v2 drawn (fails=%d), holding 30 s", fails);
            for (int s = 30; s > 0; s -= 5) {
                ESP_LOGI(TAG, "band test holding: %d s left", s);
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            heap_caps_free(px);
        } else {
            ESP_LOGW(TAG, "band test skipped: no DMA buffer");
        }
    }

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_W,
            .ver_res = LCD_H,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

/* No codec: the MAX98357A takes I2S as is, esp_codec_dev adds the volume in
 * software, and the mic gain is applied here. Both channels run all the time;
 * idle, the speaker sends zeros (auto_clear). */
static int data_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    (void)on;
    return ESP_CODEC_DEV_OK;
}

static int spk_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    size_t wrote;
    return i2s_channel_write(s_tx, data, size, &wrote, portMAX_DELAY) == ESP_OK ? ESP_CODEC_DEV_OK
                                                                                : ESP_CODEC_DEV_WRITE_FAIL;
}

/* A MEMS mic's slot has a small DC offset, which the gain would turn into
 * clipping: each slot's zero level is tracked and taken off first. */
static int mic_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    size_t got;
    if (i2s_channel_read(s_rx, data, size, &got, pdMS_TO_TICKS(1000)) != ESP_OK || got != (size_t)size) {
        return ESP_CODEC_DEV_READ_FAIL;
    }
    int16_t *s = (int16_t *)data;
    for (int i = 0; i < size / 2; i++) {
        int32_t *dc = &s_mic_dc[i & 1];
        *dc += (s[i] * 256 - *dc) >> 8;
        int v = (s[i] - (*dc >> 8)) * s_mic_gain_q8 >> 8;
        s[i] = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
    }
    return ESP_CODEC_DEV_OK;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    (void)mic;
    s_mic_gain_q8 = (int)(256.0f * powf(10.0f, (db - MIC_GAIN_OFFSET_DB) / 20.0f));
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* Speaker: its own clocks, TX only, on I2S port 0. */
    i2s_chan_config_t spk_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    spk_chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&spk_chan_cfg, &s_tx, NULL), TAG, "i2s speaker channel");
    i2s_std_config_t spk_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK,
            .ws = SPK_WS,
            .dout = SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    /* 32-bit slots (64 clocks a frame), which an INMP441-style mic needs;
     * the MAX98357A takes them too. Each sample is the slot's top 16 bits. */
    spk_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    spk_cfg.slot_cfg.ws_width = 32;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &spk_cfg), TAG, "i2s speaker");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s speaker on");

    /* Mic: its own clocks, RX only, on I2S port 1. */
    i2s_chan_config_t mic_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    mic_chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_chan_cfg, NULL, &s_rx), TAG, "i2s mic channel");
    i2s_std_config_t mic_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DIN,
        },
    };
    mic_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    mic_cfg.slot_cfg.ws_width = 32;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &mic_cfg), TAG, "i2s mic");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s mic on");

    static const audio_codec_data_if_t spk_if = { .enable = data_enable, .write = spk_write };
    static const audio_codec_data_if_t mic_if = { .enable = data_enable, .read = mic_read };
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &spk_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &mic_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk }, 1, timeout_ms);
}

/* No power switch or latch: the screen goes dark and the chip sleeps until
 * BOOT is pressed. On USB it is still powered. */
static esp_err_t power_off(void)
{
    set_brightness(0);
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "muse-s3cam-diy",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = false,
    .diagonal_in = 2.0f,   /* approximate: the common 240x320 ST7789 module */
    /* BOOT is the only button, so it also wakes the screen and the chip. */
    .talk_button = "boot",
    .aux_button = "boot",
    .talk_hint = { LV_ALIGN_BOTTOM_RIGHT, -16, -8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
