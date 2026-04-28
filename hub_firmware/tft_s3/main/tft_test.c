/**
 * tft_s3 — ILI9341 display test running directly on the ESP32-S3 hub board.
 * ─────────────────────────────────────────────────────────────────────────────
 * Replaces the two-board setup (S3 → UART → C6 → SPI → TFT) with a single
 * board: the S3 drives the ILI9341 over SPI directly.
 *
 * Displays:
 *   • Header   "Glazia Hub is on"
 *   • Panel    "pritam is great"
 *
 * SPI pin assignment (change to match your wiring):
 *   MOSI  GPIO11
 *   SCK   GPIO12
 *   CS    GPIO10
 *   DC    GPIO13
 *   RST   GPIO14
 *
 * Avoids: Button (GPIO21), UART1 (GPIO17/18), USB (GPIO19/20),
 *         octal PSRAM/flash (GPIO27–37).
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

/* Logo image — forward-declare directly to avoid images.h's <lvgl/lvgl.h>
 * path (managed component dir is lvgl__lvgl, not lvgl). Definition lives in
 * ui/ui_image_glazia_logo.c which handles its own LVGL include via __has_include. */
extern const lv_img_dsc_t img_glazia_logo;

static const char *TAG = "tft_s3";

/* ── TFT SPI pins (ESP32-S3) ─────────────────────────────────────────────── */
/* GPIO11/12/10/13 are SPI2 IOMUX pins on S3 — IOMUX path may conflict on
 * some boards. Moving MOSI off IOMUX (GPIO11→GPIO4) forces the whole SPI2
 * bus through the GPIO matrix, the same path the C6 used.                   */
#define PIN_MOSI   4
#define PIN_SCK   12
#define PIN_CS    10
#define PIN_DC    13
#define PIN_RST   14

/* ── Display resolution (portrait, ILI9341) ──────────────────────────────── */
#define LCD_H_RES        240
#define LCD_V_RES        320
#define LCD_SPI_HOST     SPI2_HOST
#define LCD_PIXEL_CLK_HZ (2 * 1000 * 1000)   /* slow for debug — raise to 20MHz once working */
#define LCD_CMD_BITS     8
#define LCD_PARAM_BITS   8
#define DRAW_BUF_LINES   50

static esp_lcd_panel_io_handle_t io_handle;
static esp_lcd_panel_handle_t    panel_handle;

/* ── LCD init ────────────────────────────────────────────────────────────── */
static void lcd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_DC,
        .cs_gpio_num       = PIN_CS,
        .pclk_hz           = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_endian     = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel_handle));

    esp_lcd_panel_reset(panel_handle);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_lcd_panel_init(panel_handle);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_invert_color(panel_handle, false);
    esp_lcd_panel_mirror(panel_handle, true, false);
    esp_lcd_panel_set_gap(panel_handle, 0, 0);
    esp_lcd_panel_disp_on_off(panel_handle, true);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ── LVGL init ───────────────────────────────────────────────────────────── */
static lv_disp_t *lvgl_init(void)
{
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io_handle,
        .panel_handle  = panel_handle,
        .buffer_size   = LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .flags         = { .buff_dma = true },
    };
    return lvgl_port_add_disp(&disp_cfg);
}

/* ── Build the UI ────────────────────────────────────────────────────────── */
/*
 * Layout (240 × 320, portrait):
 *
 *  ┌──────────────────────────┐  y: 0–67
 *  │  [logo]  GLAZIA  Hub     │  header, red background
 *  ├──────────────────────────┤
 *  │  Glazia Hub is on        │  y: 78–155  dark panel
 *  ├──────────────────────────┤
 *  │                          │  spacer
 *  ├──────────────────────────┤
 *  │  pritam is great         │  y: 180–280  dark panel
 *  └──────────────────────────┘
 */
static void build_ui(lv_disp_t *disp)
{
    /* Wait up to 2 s for the LVGL task to be ready.
     * lvgl_port_lock(0) with zero timeout returns false immediately if the
     * task hasn't started yet, silently skipping all drawing. */
    if (!lvgl_port_lock(pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "LVGL lock timed out — nothing drawn");
        return;
    }

    /* Default theme */
    lv_theme_t *theme = lv_theme_default_init(disp,
        lv_palette_main(LV_PALETTE_RED),
        lv_palette_main(LV_PALETTE_GREY),
        true, LV_FONT_DEFAULT);
    lv_disp_set_theme(disp, theme);

    /* Root screen */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(scr, lv_color_make(238, 28, 37), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    /* ── Header ─────────────────────────────────────────────────────────── */
    {
        /* Glazia logo (scaled) */
        lv_obj_t *img = lv_img_create(scr);
        lv_obj_set_pos(img, 5, 6);
        lv_img_set_src(img, &img_glazia_logo);
        lv_img_set_zoom(img, 40);   /* 40/256 ≈ 55 px square */
        lv_img_set_pivot(img, 0, 0);

        /* "GLAZIA" */
        lv_obj_t *title = lv_label_create(scr);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_26, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
        lv_label_set_text(title, "GLAZIA");
        lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -8, 8);

        /* "Hub" */
        lv_obj_t *sub = lv_label_create(scr);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(sub, lv_color_make(255, 180, 180), LV_PART_MAIN);
        lv_label_set_text(sub, "Hub");
        lv_obj_align(sub, LV_ALIGN_TOP_RIGHT, -8, 42);
    }

    /* ── "Glazia Hub is on" panel ────────────────────────────────────────── */
    {
        lv_obj_t *panel = lv_obj_create(scr);
        lv_obj_set_pos(panel, 8, 78);
        lv_obj_set_size(panel, 224, 70);
        lv_obj_set_style_bg_color(panel, lv_color_make(20, 20, 20), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(panel, lv_color_make(238, 28, 37), LV_PART_MAIN);
        lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(panel, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(panel);
        lv_label_set_text(lbl, "Glazia Hub is on");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_make(80, 220, 80), LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    }

    /* ── "pritam is great" panel ─────────────────────────────────────────── */
    {
        lv_obj_t *panel = lv_obj_create(scr);
        lv_obj_set_pos(panel, 8, 165);
        lv_obj_set_size(panel, 224, 70);
        lv_obj_set_style_bg_color(panel, lv_color_make(20, 20, 20), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(panel, lv_color_make(238, 28, 37), LV_PART_MAIN);
        lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(panel, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(panel);
        lv_label_set_text(lbl, "pritam is great");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    }

    lv_scr_load(scr);
    lvgl_port_unlock();
}

/* ── Entry point ─────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "tft_s3 starting — S3 drives ILI9341 directly");

    lcd_init();

    /* ── Direct SPI test — flash black then white 5 times ───────────────────
     * 0x0000 = black and 0xFFFF = white in EVERY color mode — no ambiguity.
     * If the screen flickers at all → SPI is working, tune colors/init after.
     * If screen stays solid white throughout → SPI data is not reaching GRAM. */
    {
        static uint16_t buf[LCD_H_RES * DRAW_BUF_LINES];

        for (int round = 0; round < 5; round++) {
            uint16_t color = (round % 2 == 0) ? 0x0000 : 0xFFFF;
            ESP_LOGI(TAG, "filling %s", color == 0x0000 ? "BLACK" : "WHITE");

            for (int i = 0; i < LCD_H_RES * DRAW_BUF_LINES; i++) buf[i] = color;
            for (int y = 0; y < LCD_V_RES; y += DRAW_BUF_LINES) {
                esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle,
                                    0, y, LCD_H_RES, y + DRAW_BUF_LINES, buf);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "draw_bitmap failed at y=%d: %s", y, esp_err_to_name(ret));
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            vTaskDelay(pdMS_TO_TICKS(1500)); /* hold each color for 1.5 s */
        }
    }

    lv_disp_t *disp = lvgl_init();
    build_ui(disp);

    ESP_LOGI(TAG, "UI ready");

    /* Nothing left to do — LVGL timer runs inside esp_lvgl_port */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
