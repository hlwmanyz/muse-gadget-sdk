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
 * DIY ESP32-S3 N16R8 CAM board (e.g. GOOUUU ESP32-S3-CAM: 16 MB flash, 8 MB
 * octal PSRAM) with a 1.54" 240x240 ST7789 SPI LCD, an INMP441 I2S microphone
 * and a MAX98357A I2S amplifier. Wiring follows the xiaozhi breadboard
 * tutorial's table (INMP441/MAX98357A/ST7789 modules on a breadboard).
 *
 * The LCD sits on GPIO19/20, the chip's native USB pins, so the console and
 * flashing go through the board's CH340 TTL port (UART0); see
 * CONFIG_MUSE_CONSOLE_UART in devices/sdkconfig.muse-s3cam-diy. The BOOT
 * button (GPIO0) is push-to-talk. The OV2640 camera is not used.
 */
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

/* ST7789 1.54" 240x240, SPI. */
#define LCD_RES 240
#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_19
#define LCD_MOSI GPIO_NUM_20
#define LCD_CS GPIO_NUM_45
#define LCD_DC GPIO_NUM_47
#define LCD_RST GPIO_NUM_21
#define LCD_BL GPIO_NUM_38
#define DRAW_BUF_LINES 40

/* INMP441 I2S microphone (L/R strapped to GND: left slot). */
#define MIC_WS GPIO_NUM_1
#define MIC_SCK GPIO_NUM_2
#define MIC_SD GPIO_NUM_42

/* MAX98357A I2S amplifier (SD strapped high: always on). */
#define SPK_BCLK GPIO_NUM_40
#define SPK_WS GPIO_NUM_41
#define SPK_DOUT GPIO_NUM_39

#define TALK_GPIO GPIO_NUM_0      /* BOOT button */

static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk;

static esp_err_t init(void)
{
    return muse_gpio_button_init(&s_talk, TALK_GPIO);
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
        .max_transfer_sz = LCD_RES * LCD_RES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0, /* bread-compact-wifi-s3cam 8-pin 240x240: mode 0 */
        .pclk_hz = 40 * 1000 * 1000, /* xiaozhi runs 80 MHz on this wiring */
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, /* bread-compact-wifi-s3cam */
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true); /* bread-compact-wifi-s3cam */
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* DIAGNOSTIC: draw red/blue split directly, bypass LVGL.
     * If user sees red/blue: SPI data path works.
     * If user sees gray: SPI data path broken. */
    {
        static uint16_t test_buf[240 * 40];
        for (int i = 0; i < 240 * 40; i++) test_buf[i] = 0xF800; /* red */
        for (int y = 0; y < 120; y += 40) {
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, 240, y + 40, test_buf);
        }
        for (int i = 0; i < 240 * 40; i++) test_buf[i] = 0x001F; /* blue */
        for (int y = 120; y < 240; y += 40) {
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, 240, y + 40, test_buf);
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
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
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

/*
 * INMP441 on I2S0 (RX only) and MAX98357A on I2S1 (TX only). Neither chip has
 * a control interface, so codec_if is NULL and esp_codec_dev applies its
 * software volume to the speaker; the mic gain stays fixed.
 */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t mic_rx, spk_tx;

    i2s_chan_config_t mic_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_chan_cfg, NULL, &mic_rx), TAG, "mic i2s channel");
    const i2s_std_config_t mic_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = MIC_SCK,
            .ws = MIC_WS,
            .dout = GPIO_NUM_NC,
            .din = MIC_SD,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(mic_rx, &mic_cfg), TAG, "mic i2s");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(mic_rx), TAG, "mic i2s on");

    i2s_chan_config_t spk_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    spk_chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&spk_chan_cfg, &spk_tx, NULL), TAG, "spk i2s channel");
    const i2s_std_config_t spk_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = SPK_BCLK,
            .ws = SPK_WS,
            .dout = SPK_DOUT,
            .din = GPIO_NUM_NC,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(spk_tx, &spk_cfg), TAG, "spk i2s");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(spk_tx), TAG, "spk i2s on");

    audio_codec_i2s_cfg_t mic_i2s = { .port = I2S_NUM_0, .rx_handle = mic_rx, .tx_handle = NULL };
    const audio_codec_data_if_t *mic_data = audio_codec_new_i2s_data(&mic_i2s);
    audio_codec_i2s_cfg_t spk_i2s = { .port = I2S_NUM_1, .rx_handle = NULL, .tx_handle = spk_tx };
    const audio_codec_data_if_t *spk_data = audio_codec_new_i2s_data(&spk_i2s);
    ESP_RETURN_ON_FALSE(mic_data && spk_data, ESP_ERR_NO_MEM, TAG, "i2s data if");

    esp_codec_dev_cfg_t mic_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = NULL, .data_if = mic_data,
    };
    esp_codec_dev_cfg_t spk_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = NULL, .data_if = spk_data,
    };
    *mic = esp_codec_dev_new(&mic_dev_cfg);
    *spk = esp_codec_dev_new(&spk_dev_cfg);
    return *mic && *spk ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk }, 1, timeout_ms);
}

/* USB-powered through the TTL port; no battery. */
static esp_err_t read_power(muse_power_t *out)
{
    out->battery_pct = -1;
    out->battery_mv = 0;
    out->charging = false;
    out->usb = true;
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "ESP32-S3 N16R8 CAM DIY (ST7789 + INMP441 + MAX98357A)",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = false,
    .touch = false,
    .talk_button = "boot",
    .aux_button = NULL,
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -8 },
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
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
