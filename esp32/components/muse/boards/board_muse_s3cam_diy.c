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
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

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

static spi_device_handle_t s_raw_dev;
static uint16_t *s_stage;   /* LCD_W x 32 staging for solid fills */

/* The panel is driven with raw SPI polling transfers, not the esp_lcd
 * stack: on this board esp_lcd paints the boot bands and then silently
 * stops reaching GRAM (every call keeps returning ESP_OK), while a
 * torn-down, freshly rebuilt bus + raw polling device paints fine --
 * proven by the v8 probe (2026-10-22): full-screen MAGENTA then CYAN,
 * every return code ESP_OK, chunk fails 0. So the board owns the SPI
 * bus and the ST7789 init sequence itself, and LVGL flushes through
 * the raw path below. */
static SemaphoreHandle_t s_lv_lock;
static volatile bool s_lv_paused;

static esp_err_t raw_tx(const void *data, size_t len_bytes, int dc)
{
    if (!data || len_bytes == 0) {
        return ESP_OK;
    }
    gpio_set_level(LCD_DC, dc);
    spi_transaction_t t = {
        .length = len_bytes * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_raw_dev, &t);
}

static esp_err_t raw_cmd(uint8_t cmd, const uint8_t *params, size_t nparams)
{
    esp_err_t rc = raw_tx(&cmd, 1, 0);
    if (rc == ESP_OK && params && nparams) {
        rc = raw_tx(params, nparams, 1);
    }
    return rc;
}

static esp_err_t raw_window(int x1, int y1, int x2, int y2)
{
    const uint8_t caset[] = { (uint8_t)(x1 >> 8), (uint8_t)x1,
                              (uint8_t)(x2 >> 8), (uint8_t)x2 };
    const uint8_t raset[] = { (uint8_t)(y1 >> 8), (uint8_t)y1,
                              (uint8_t)(y2 >> 8), (uint8_t)y2 };
    esp_err_t rc = raw_cmd(0x2A, caset, sizeof(caset));   /* CASET */
    if (rc == ESP_OK) {
        rc = raw_cmd(0x2B, raset, sizeof(raset));         /* RASET */
    }
    if (rc == ESP_OK) {
        rc = raw_cmd(0x2C, NULL, 0);                      /* RAMWR */
    }
    return rc;
}

static int raw_fill_rows(uint16_t v, int y1, int y2)
{
    if (!s_stage) {
        return -1;
    }
    const uint16_t be = (uint16_t)((v >> 8) | (v << 8));
    for (int i = 0; i < LCD_W * 32; i++) {
        s_stage[i] = be;
    }
    int fails = raw_window(0, y1, LCD_W - 1, y2) != ESP_OK;
    const int rows = y2 - y1 + 1;
    int done = 0;
    while (done < rows) {
        const int slice = rows - done > 32 ? 32 : rows - done;
        fails += raw_tx(s_stage, (size_t)LCD_W * slice * sizeof(uint16_t), 1) != ESP_OK;
        done += slice;
    }
    return fails;
}

static esp_err_t panel_raw_init(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_W * 32 * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");
    const spi_device_interface_config_t raw_cfg = {
        .mode = 0,
        .clock_speed_hz = 10 * 1000 * 1000,
        .spics_io_num = LCD_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_HOST, &raw_cfg, &s_raw_dev), TAG, "spi dev");
    gpio_set_direction(LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(raw_cmd(0x01, NULL, 0), TAG, "swreset");
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_RETURN_ON_ERROR(raw_cmd(0x11, NULL, 0), TAG, "slpout");
    vTaskDelay(pdMS_TO_TICKS(120));
    static const uint8_t colmod[] = { 0x55 };   /* RGB565 */
    raw_cmd(0x3A, colmod, sizeof(colmod));
    static const uint8_t madctl[] = { 0x00 };
    raw_cmd(0x36, madctl, sizeof(madctl));
    raw_cmd(0x21, NULL, 0);   /* INVON: this panel wants inversion on */
    raw_cmd(0x13, NULL, 0);   /* NORON */
    raw_cmd(0x29, NULL, 0);   /* DISPON */
    s_stage = heap_caps_malloc(LCD_W * 32 * sizeof(uint16_t),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_stage) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "panel up (raw SPI polling, 10 MHz)");
    return ESP_OK;
}

static volatile uint32_t s_flush_count;
static volatile uint32_t s_flush_rc_fail;

static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const uint16_t *px = (const uint16_t *)px_map;
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    uint16_t mn = 0xFFFF, mx = 0;
    /* LVGL's draw buffers live in PSRAM (internal RAM is reserved for the
     * I2S DMA: with internal draw buffers the speaker's DMA descriptors
     * failed to allocate and the voice pipeline died). Each slice is
     * byte-swapped into the internal staging buffer, then transmitted. */
    int done_rows = 0;
    while (done_rows < h) {
        const int slice = h - done_rows > 32 ? 32 : h - done_rows;
        const uint16_t *srcp = px + done_rows * w;
        for (int i = 0; i < w * slice; i++) {
            const uint16_t v = srcp[i];
            if (v < mn) {
                mn = v;
            }
            if (v > mx) {
                mx = v;
            }
            s_stage[i] = (uint16_t)((v >> 8) | (v << 8));
        }
        if (raw_window(area->x1, area->y1 + done_rows, area->x2,
                       area->y1 + done_rows + slice - 1) == ESP_OK) {
            if (raw_tx(s_stage, (size_t)w * slice * sizeof(uint16_t), 1) != ESP_OK) {
                s_flush_rc_fail++;
            }
        } else {
            s_flush_rc_fail++;
        }
        done_rows += slice;
    }
    s_flush_count++;
    if (s_flush_count <= 10 || s_flush_count % 100 == 0) {
        ESP_LOGI(TAG, "flush #%lu area %d,%d-%d,%d px min=%04x max=%04x rc_fail=%lu",
                 (unsigned long)s_flush_count, area->x1, area->y1, area->x2,
                 area->y2, mn, mx, (unsigned long)s_flush_rc_fail);
    }
    lv_display_flush_ready(disp);
}

static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(10);
}

static void lv_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t wait_ms = 50;
        if (!s_lv_paused && xSemaphoreTakeRecursive(s_lv_lock, portMAX_DELAY) == pdTRUE) {
            wait_ms = lv_timer_handler();
            xSemaphoreGiveRecursive(s_lv_lock);
        }
        if (wait_ms < 5) {
            wait_ms = 5;
        } else if (wait_ms > 50) {
            wait_ms = 50;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

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

static bool display_lock(int timeout_ms);
static void display_unlock(void);
static void sentinel_task(void *arg);

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
    /* Backlight fully on from the start (the channel config leaves duty 0);
     * muse_ui manages brightness from here via set_brightness(). */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    ESP_LOGE(TAG, "console probe: SECONDARY USB Serial/JTAG is ON (steals LCD pins GPIO19/20)");
#endif
#if CONFIG_ESP_CONSOLE_SECONDARY_NONE
    ESP_LOGI(TAG, "console probe: secondary console is NONE (LCD pins safe)");
#endif
    if (panel_raw_init() != ESP_OK) {
        return NULL;
    }

    /* Boot bands on the raw path (kept as the power-on tell: if these
     * show, the panel path is alive; LVGL paints over them in ~1 s). */
    {
        static const uint16_t band_rgb565[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
        int fails = 0;
        for (int b = 0; b < 4; b++) {
            fails += raw_fill_rows(band_rgb565[b], b * 80, (b + 1) * 80 - 1) != 0;
        }
        ESP_LOGI(TAG, "boot bands drawn (fails=%d), holding 1.5 s", fails);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    /* Hand-rolled LVGL bring-up (see the note above lv_flush_cb). */
    s_lv_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lv_lock) {
        return NULL;
    }
    lv_init();
    const esp_timer_create_args_t tick_args = {
        .callback = lv_tick_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick_timer;
    if (esp_timer_create(&tick_args, &tick_timer) != ESP_OK
        || esp_timer_start_periodic(tick_timer, 10 * 1000) != ESP_OK) {
        return NULL;
    }
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    if (!disp) {
        return NULL;
    }
    lv_display_set_default(disp);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    const size_t buf_bytes = LCD_W * DRAW_BUF_LINES * sizeof(uint16_t);
    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) {
        return NULL;
    }
    lv_display_set_buffers(disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, lv_flush_cb);
    if (xTaskCreatePinnedToCore(lv_task, "lvgl", 12288, NULL, MUSE_UI_PRIORITY,
                                NULL, MUSE_UI_CORE) != pdPASS) {
        return NULL;
    }
    ESP_LOGI(TAG, "LVGL up (hand-rolled port): %dx%d, 2 x %d-line PSRAM buffers",
             LCD_W, LCD_H, DRAW_BUF_LINES);
    xTaskCreate(sentinel_task, "sentinel", 4096, NULL, 5, NULL);
    return disp;
}

static bool display_lock(int timeout_ms)
{
    if (!s_lv_lock) {
        return false;
    }
    const TickType_t ticks = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lv_lock, ticks) == pdTRUE;
}

static void display_unlock(void)
{
    if (s_lv_lock) {
        xSemaphoreGiveRecursive(s_lv_lock);
    }
}

/* One-shot diagnostic (remove with the flush counters once the UI is
 * confirmed visible): 6 s after the display starts, paint the whole
 * screen MAGENTA through the same raw path the bands used, hold 2 s,
 * then invalidate the LVGL screen so the UI repaints over it. Magenta
 * never appearing => the raw path is dead outside display_start.
 * Magenta appearing but never repainted => LVGL flush is not running.
 * Magenta replaced by grey again => flush runs and paints that grey:
 * the content itself is the problem. */
static void sentinel_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(6000));
    if (display_lock(-1)) {
        const int fails = raw_fill_rows(0xF81F, 0, LCD_H - 1);
        ESP_LOGI(TAG, "sentinel: magenta painted, fails=%d, flush_count=%lu rc_fail=%lu",
                 fails, (unsigned long)s_flush_count, (unsigned long)s_flush_rc_fail);
        display_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (display_lock(-1)) {
        lv_obj_invalidate(lv_screen_active());
        ESP_LOGI(TAG, "sentinel: screen invalidated; UI should repaint over magenta");
        display_unlock();
    }
    vTaskDelete(NULL);
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    if (!s_raw_dev) {
        return;
    }
    if (sleep) {
        raw_cmd(0x28, NULL, 0);   /* DISPOFF */
        raw_cmd(0x10, NULL, 0);   /* SLPIN; GRAM is kept */
    } else {
        raw_cmd(0x11, NULL, 0);   /* SLPOUT */
        vTaskDelay(pdMS_TO_TICKS(120));
        raw_cmd(0x29, NULL, 0);   /* DISPON */
    }
}

static void display_pause(bool pause)
{
    s_lv_paused = pause;
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
    .display_unlock = display_unlock,
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
