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
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

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
static SemaphoreHandle_t s_lvgl_mutex = NULL;
static volatile bool s_lvgl_paused = false;
static TaskHandle_t s_lvgl_task = NULL;
static muse_gpio_button_t s_talk;

static esp_err_t init(void)
{
    return muse_gpio_button_init(&s_talk, TALK_GPIO);
}

static void s3cam_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* Byte swap (xiaozhi swap_bytes=1) */
    uint16_t *px = (uint16_t *)px_map;
    int32_t n = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    for (int32_t i = 0; i < n; i++) {
        uint16_t v = px[i];
        px[i] = (uint16_t)((v << 8) | (v >> 8));
    }
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}
static void s3cam_lvgl_task(void *arg)
{
    (void)arg;
    while (1) {
        if (!s_lvgl_paused && xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            lv_tick_inc(10);
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true); /* bread-compact-wifi-s3cam */
    esp_lcd_panel_disp_on_off(s_panel, true);

    lv_init();
    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (!s_lvgl_mutex) return NULL;
    lv_display_t *disp = lv_display_create(LCD_RES, LCD_RES);
    if (!disp) return NULL;
    /* Allocate draw buffer in DMA-capable internal RAM */
    static lv_draw_buf_t s_draw_buf;
static lv_draw_buf_t s_draw_buf2;
    uint32_t stride = LCD_RES * 2;
    uint32_t data_size = stride * 20;
    void *buf_data = heap_caps_malloc(data_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf_data) return NULL;
    lv_draw_buf_init(&s_draw_buf, LCD_RES, 20, LV_COLOR_FORMAT_RGB565, stride, buf_data, data_size);
    lv_draw_buf_init(&s_draw_buf2, LCD_RES, 20, LV_COLOR_FORMAT_RGB565, stride, buf_data2, data_size);
    lv_display_set_draw_buffers(disp, &s_draw_buf, &s_draw_buf2);
    lv_display_set_flush_cb(disp, s3cam_flush_cb);
    if (xTaskCreatePinnedToCore(s3cam_lvgl_task, "lvgl", 4096, NULL, 5, &s_lvgl_task, 1) != pdPASS) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    if (!s_lvgl_mutex) return false;
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS((uint32_t)timeout_ms);
    return xSemaphoreTake(s_lvgl_mutex, t) == pdTRUE;
}
static void display_unlock(void)
{
    if (s_lvgl_mutex) xSemaphoreGive(s_lvgl_mutex);
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
    s_lvgl_paused = pause;
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
    .display_unlock = display_unlock,
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
