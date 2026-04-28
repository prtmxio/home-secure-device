/**
 * display.c — ILI9341 TFT driven directly from ESP32-S3 over SPI + LVGL.
 *
 * Replaces the old UART-to-C6 bridge. The public API (display_show,
 * display_hub_location, display_sensor_location, display_sensor_list)
 * is identical — callers in wifi.c, button.c, api_client.c etc. need
 * no changes.
 *
 * Layout (240 × 320, portrait):
 *   [0]  Header      — Glazia logo + "GLAZIA / Hub"       y: 0–67
 *   [1]  Status panel — status text + location             y: 78–147
 *   [2]  Critical Toggle (always ON while powered)         y: 147–182
 *   [3]  Sensor list  — scrollable paired-sensor table     y: 192–311
 */
#include "display.h"
#include "espnow.h"
#include "misc/lv_area.h"
#include "state.h"
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
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "DISPLAY";

extern const lv_img_dsc_t img_glazia_logo;

/* ── Display constants ───────────────────────────────────────────────────── */
#define LCD_H_RES          240
#define LCD_V_RES          320
#define LCD_SPI_HOST       SPI2_HOST
#define LCD_PIXEL_CLK_HZ   (10 * 1000 * 1000)
#define LCD_CMD_BITS       8
#define LCD_PARAM_BITS     8
#define DRAW_BUF_LINES     50

/* ── LVGL object handles (set once in display_init, read-only after) ─────── */
static lv_obj_t *s_status_label   = NULL;
static lv_obj_t *s_location_label = NULL;
static lv_obj_t *s_sensor_label   = NULL;

/* ── Hardware init ───────────────────────────────────────────────────────── */

static void lcd_hw_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = LCD_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = LCD_PIN_DC,
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_endian     = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));

    esp_lcd_panel_reset(panel);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_lcd_panel_init(panel);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_invert_color(panel, false);
    esp_lcd_panel_mirror(panel, true, false);
    esp_lcd_panel_set_gap(panel, 0, 0);
    esp_lcd_panel_disp_on_off(panel, true);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Hand off to LVGL port */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .flags         = { .buff_dma = true },
    };
    lvgl_port_add_disp(&disp_cfg);
}

/* ── UI build (called once LVGL task is ready) ───────────────────────────── */

static void build_ui(lv_disp_t *disp)
{
    lv_theme_t *theme = lv_theme_default_init(disp,
        lv_palette_main(LV_PALETTE_RED),
        lv_palette_main(LV_PALETTE_GREY),
        true, LV_FONT_DEFAULT);
    lv_disp_set_theme(disp, theme);

    /* Root screen — red background */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(scr, lv_color_make(240, 240, 240), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    /* ── Header ──────────────────────────────────────────────────────────── */
    {
        /* Logo on the left — 350×350 source, zoomed to ~55px square */
        lv_obj_t *logo = lv_img_create(scr);
        lv_img_set_src(logo, &img_glazia_logo);
        lv_img_set_zoom(logo, 44);
        lv_img_set_pivot(logo, 0, 0);   /* scale from top-left */
        lv_obj_align(logo, LV_ALIGN_TOP_LEFT, 10, 6);

        /* "GLAZIA / Hub" text on the right — larger size for emphasis */
        lv_obj_t *t = lv_label_create(scr);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_26, LV_PART_MAIN);
        lv_obj_set_style_text_color(t, lv_color_black(), LV_PART_MAIN);
        lv_label_set_text(t, "GLAZIA");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 14, 8);

        lv_obj_t *s = lv_label_create(scr);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(s, lv_color_black(), LV_PART_MAIN);
        lv_label_set_text(s, "Hub");
        lv_obj_align(s, LV_ALIGN_TOP_MID, 14, 42);
    }

    /* ── Status / sensor-data panel ─────────────────────────────────────── */
    {
        lv_obj_t *panel = lv_obj_create(scr);
        lv_obj_set_pos(panel, 8, 68);
        lv_obj_set_size(panel, 224, 70);
        lv_obj_set_style_bg_color(panel, lv_color_make(20, 20, 20), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(panel, lv_color_make(238, 28, 37), LV_PART_MAIN);
        lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(panel, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(panel, 8, LV_PART_MAIN);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

        /* Status text — updated by display_show() */
        s_status_label = lv_label_create(panel);
        lv_label_set_text(s_status_label, "Starting up...");
        lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_status_label, 208);
        lv_obj_set_style_text_color(s_status_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 0, 0);

        /* Location / source — updated by display_hub_location() / display_sensor_location() */
        s_location_label = lv_label_create(panel);
        lv_label_set_text(s_location_label, "Location: --");
        lv_label_set_long_mode(s_location_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_location_label, 208);
        lv_obj_set_style_text_color(s_location_label, lv_color_make(200, 200, 200), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_location_label, &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_align(s_location_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    /* ── Critical toggle (always ON while hub is powered) ────────────────── */
    {
        lv_obj_t *sw = lv_switch_create(scr);
        lv_obj_set_size(sw, 52, 26);
        lv_obj_align(sw, LV_ALIGN_TOP_MID, 0, 144);
        lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(sw, lv_color_make(238, 28, 37),
                                   LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, lv_color_white(),
                                   LV_PART_KNOB | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                                  LV_PART_KNOB | LV_STATE_DEFAULT);

        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "Critical Toggle");
        lv_obj_set_style_text_color(lbl, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 175);
    }

    /* ── Sensor list panel — scrollable ──────────────────────────────────── */
    {
        lv_obj_t *panel = lv_obj_create(scr);
        lv_obj_set_pos(panel, 8, 192);
        lv_obj_set_size(panel, 224, 120);
        lv_obj_set_style_bg_color(panel, lv_color_make(20, 20, 20), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(panel, lv_color_make(238, 28, 37), LV_PART_MAIN);
        lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(panel, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(panel, 8, LV_PART_MAIN);
        lv_obj_set_scroll_dir(panel, LV_DIR_VER);

        s_sensor_label = lv_label_create(panel);
        lv_label_set_text(s_sensor_label,
                          "No sensors paired yet.\nPress hub button to pair.");
        lv_label_set_long_mode(s_sensor_label, LV_LABEL_LONG_WRAP);
        lv_label_set_recolor(s_sensor_label, true);
        lv_obj_set_width(s_sensor_label, 208);
        lv_obj_set_style_text_color(s_sensor_label,
                                    lv_color_make(200, 200, 200), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_sensor_label,
                                   &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_align(s_sensor_label, LV_ALIGN_TOP_LEFT, 0, 4);
    }

    lv_scr_load(scr);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void display_init(void)
{
    lcd_hw_init();

    /* Wait up to 2 s for the LVGL task to be ready before building the UI. */
    if (!lvgl_port_lock(pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "LVGL lock timed out — display not ready");
        return;
    }
    build_ui(lv_disp_get_default());
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Display ready — SPI direct, MOSI=GPIO%d", LCD_PIN_MOSI);
}

/* Thread-safe label update helper */
static void label_set(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    lv_label_set_text(label, text);
    lvgl_port_unlock();
}

void display_show(const char *line1, const char *line2)
{
    char buf[128];
    if (line2 && line2[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s\n%s", line1, line2);
    } else {
        snprintf(buf, sizeof(buf), "%s", line1);
    }
    label_set(s_status_label, buf);
    ESP_LOGI(TAG, "Display: [%s] [%s]", line1, line2 ? line2 : "");
}

void display_hub_location(const char *home_name)
{
    if (!home_name) return;
    char buf[72];
    snprintf(buf, sizeof(buf), "Hub: %s", home_name);
    label_set(s_location_label, buf);
}

void display_sensor_location(const char *mac_str)
{
    if (!mac_str) return;
    char buf[72];
    snprintf(buf, sizeof(buf), "From: %s", mac_str);
    label_set(s_location_label, buf);
}

void display_sensor_list(void)
{
    if (!s_sensor_label) return;

    char sensors[140];
    espnow_get_sensor_list_str(sensors, sizeof(sensors));

    /* Build "Name : #00FF00 Online#" / "#FF0000 Offline#" per entry */
    char out[512];
    int  pos = 0;
    char buf[150];
    strncpy(buf, sensors, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *entry = buf;
    int   count = 0;

    while (entry && *entry && pos < (int)sizeof(out) - 1) {
        char *next   = strchr(entry, ';');
        if (next) *next++ = '\0';

        char *name   = entry;
        char *loc    = strchr(name, '|');
        char *status = NULL;
        if (loc) {
            *loc++ = '\0';
            status = strchr(loc, '|');
            if (status) *status++ = '\0';
        }

        bool online = (status && strcmp(status, "ON") == 0);
        pos += snprintf(out + pos, sizeof(out) - pos,
                        "%s : %s\n",
                        name ? name : "?",
                        online ? "#00FF00 Online#" : "#FF0000 Offline#");
        count++;
        entry = next;
    }

    if (count == 0) {
        strncpy(out, "No sensors paired.\nPress hub button to pair.",
                sizeof(out) - 1);
    } else {
        int len = (int)strlen(out);
        if (len > 0 && out[len - 1] == '\n') out[len - 1] = '\0';
    }

    label_set(s_sensor_label, out);
    ESP_LOGI(TAG, "Display sensor list: %s", sensors[0] ? sensors : "(empty)");
}
