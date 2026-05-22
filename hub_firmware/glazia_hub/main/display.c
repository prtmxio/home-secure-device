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
#include "button.h"
#include "espnow.h"
#include "fingerprint.h"
#include "wifi.h"
#include "ui/ui.h"
#include "ui/screens.h"
#include "misc/lv_area.h"
#include "state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "DISPLAY";

/* ── Display constants ───────────────────────────────────────────────────── */
#define LCD_H_RES          240
#define LCD_V_RES          320
#define LCD_SPI_HOST       SPI2_HOST
#define LCD_PIXEL_CLK_HZ   (10 * 1000 * 1000)
#define LCD_CMD_BITS       8
#define LCD_PARAM_BITS     8
#define DRAW_BUF_LINES     20

/* ── LVGL object handles (set once in display_init, read-only after) ─────── */
static lv_obj_t *s_critical_sw    = NULL;
static TaskHandle_t s_auth_task   = NULL;
static bool s_switch_internal     = false;
static bool s_ui_online           = true;
static enum ScreensEnum s_prev_screen = SCREEN_ID_HUB_ONLINE;
static enum ScreensEnum s_current_screen = SCREEN_ID_HUB_ONLINE;
static bool s_screen_configured[_SCREEN_ID_LAST + 1];

typedef enum {
    DISPLAY_NOT_STARTED = 0,
    DISPLAY_STARTING,
    DISPLAY_READY,
    DISPLAY_FAILED,
} display_state_t;

typedef enum {
    CACHE_VIEW_NONE = 0,
    CACHE_VIEW_SETUP,
    CACHE_VIEW_ONLINE,
    CACHE_VIEW_OFFLINE,
    CACHE_VIEW_FINGERPRINT,
} cached_view_t;

typedef struct {
    cached_view_t view;
    char line1[64];
    char line2[96];
    char fp_title[48];
    char fp_phase[64];
    char fp_message[96];
    uint8_t fp_progress;
    bool has_temp_hum;
    float temp;
    float hum;
    char home_name[64];
} display_cache_t;

static volatile display_state_t s_display_state = DISPLAY_NOT_STARTED;
static SemaphoreHandle_t s_display_cache_mutex = NULL;
static display_cache_t s_display_cache = {
    .view = CACHE_VIEW_NONE,
};

void display_fingerprint_status(const char *message);
static void create_sensor_row(lv_obj_t *parent, int index, const char *name, bool enabled);
static void display_init_task(void *arg);
static esp_err_t lcd_hw_init(void);
static void load_screen_locked(enum ScreensEnum screen);
static void configure_screen_locked(enum ScreensEnum screen);
static void refresh_sensor_nodes_locked(void);
static void set_online_dashboard_locked(void);
static void set_offline_dashboard_locked(bool setup);
static const char *fingerprint_phase_for_title(const char *title);
static const char *fingerprint_message_normalize(const char *message);
static bool display_is_ready(void);
static void cache_apply_locked(void);

/* ── XPT2046 touch — raw SPI device (no registry component needed) ───────── */
static spi_device_handle_t s_tp_spi = NULL;
static lv_indev_t         *s_tp_indev = NULL;
static lv_indev_drv_t      s_tp_drv;
/* ── Hardware init ───────────────────────────────────────────────────────── */

/*
 * XPT2046 calibration constants — derived from observed raw ADC min/max.
 *
 * On this panel the axes are physically swapped relative to the ILI9341:
 *   0xD0 (XPT2046 "X") → measures the VERTICAL position (screen Y)
 *   0x90 (XPT2046 "Y") → measures the HORIZONTAL position (screen X)
 *
 * The LCD is initialised with mirror(true, false) — X axis is flipped —
 * so the touch horizontal coordinate must also be mirrored.
 *
 * Adjust these if the touch boundary feels off after calibration:
 */
#define TP_X_RAW_MIN   0    /* 0x90 reading at the left   edge of the panel */
#define TP_X_RAW_MAX   1300    /* 0x90 reading at the right  edge of the panel (corrected for this hardware) */
#define TP_Y_RAW_MIN  1173   /* 0xD0 reading at the top edge of the panel */
#define TP_Y_RAW_MAX  2465    /* 0xD0 reading at the bottom edge of the panel */

/*
 * XPT2046 single-channel read.
 * cmd: 0xD0 = X axis, 0x90 = Y axis  (12-bit differential mode)
 * Protocol: send 1 command byte + 2 zero bytes; result is in bytes 1–2,
 * bits [14:3], so right-shift by 3 to get the 12-bit ADC value (0–4095).
 */
static uint16_t xpt2046_read_raw(uint8_t cmd)
{
    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0x00, 0x00, 0x00};
    spi_transaction_t t = {
        .length    = 24,        /* 3 bytes */
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_transmit(s_tp_spi, &t);
    uint16_t result = (((uint16_t)rx[1] << 8) | rx[2]) >> 3;

    return result;
}

/*
 * Average 8 successive reads of the same channel.
 * XPT2046 is noisy; a single read can jump ~30 px, which LVGL's click
 * detector treats as a drag and never fires LV_EVENT_CLICKED on the switch.
 * Averaging stabilises the reported position to within ~3 px.
 */
static uint16_t xpt2046_read_avg(uint8_t cmd)
{
    uint32_t sum = 0;
    for (int i = 0; i < 6; i++) {
        sum += xpt2046_read_raw(cmd);
    }
    return (uint16_t)(sum / 6);
}

/*
 * LVGL input-device read callback — called from the LVGL task on every tick.
 * T_IRQ is active-low: LOW = panel touched.
 */

static void xpt2046_read_cb(lv_indev_drv_t *drv,
                            lv_indev_data_t *data)
{
    LV_UNUSED(drv);

    static bool s_last_pressed = false;

    bool pressed =
        (gpio_get_level(TOUCH_PIN_IRQ) == 0);


    if (pressed) {

        /*
         * 0xD0 -> vertical
         * 0x90 -> horizontal
         */

        uint16_t raw_phys_y =
            xpt2046_read_avg(0xD0);

        uint16_t raw_phys_x =
            xpt2046_read_avg(0x90);

        if (raw_phys_x > 3500 || raw_phys_y > 3500) { data->state = LV_INDEV_STATE_RELEASED; return; }

        /*
         * Clamp
         */

        if (raw_phys_x < TP_X_RAW_MIN)
            raw_phys_x = TP_X_RAW_MIN;

        if (raw_phys_x > TP_X_RAW_MAX)
            raw_phys_x = TP_X_RAW_MAX;

        if (raw_phys_y < TP_Y_RAW_MIN)
            raw_phys_y = TP_Y_RAW_MIN;

        if (raw_phys_y > TP_Y_RAW_MAX)
            raw_phys_y = TP_Y_RAW_MAX;

        /*
         * Interpolate
         */

        lv_coord_t px =
            (lv_coord_t)(
                ((uint32_t)(raw_phys_x - TP_X_RAW_MIN)
                * (LCD_H_RES - 1))
                / (TP_X_RAW_MAX - TP_X_RAW_MIN)
            );

        lv_coord_t py =
            (lv_coord_t)(
                ((uint32_t)(raw_phys_y - TP_Y_RAW_MIN)
                * (LCD_V_RES - 1))
                / (TP_Y_RAW_MAX - TP_Y_RAW_MIN)
            );

        /*
         * LCD mirrored in X
         */

        px = (LCD_H_RES - 1) - px;


        /*
            * Final clamp
         */

        if (px < 0)
            px = 0;

        if (px >= LCD_H_RES)
            px = LCD_H_RES - 1;

        if (py < 0)
            py = 0;

        if (py >= LCD_V_RES)
            py = LCD_V_RES - 1;

        data->point.x = px;
        data->point.y = py;

        static uint32_t dbg_counter = 0;

        if ((dbg_counter++ % 10) == 0) {
            ESP_LOGI(TAG,
                     "TOUCH raw=(%u,%u) px=(%d,%d)",
                     raw_phys_x,
                     raw_phys_y,
                     px,
                     py);
        }
    }

    if (!pressed && s_last_pressed) {
        ESP_LOGI(TAG, "TOUCH released");
    }

    s_last_pressed = pressed;

    data->state =
        pressed
        ? LV_INDEV_STATE_PRESSED
        : LV_INDEV_STATE_RELEASED;
}

typedef struct {
    lv_obj_t *sw;
    bool turn_on;
    int action;
} auth_task_arg_t;

enum {
    AUTH_ACTION_HUB_TOGGLE = 1,
    AUTH_ACTION_ADD_SENSOR,
    AUTH_ACTION_ADD_FINGERPRINT,
};

static void set_switch_checked(lv_obj_t *sw, bool checked)
{
    if (!sw) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;

    s_switch_internal = true;
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }
    s_switch_internal = false;

    lvgl_port_unlock();
}

static void auth_toggle_task(void *arg)
{
    auth_task_arg_t *a = (auth_task_arg_t *)arg;
    lv_obj_t *sw = a->sw;
    bool turn_on = a->turn_on;
    int action = a->action;
    free(a);

    g_mode = MODE_FINGERPRINT_VERIFY;
    display_show_fingerprint_screen("Verify Fingerprint", "Place your finger on the sensor");
    esp_err_t result = fp_verify();

    if (result == ESP_OK) {
        if (action == AUTH_ACTION_ADD_SENSOR) {
            display_clear_sensor_notifications();
            s_prev_screen = SCREEN_ID_SENSOR_NODES_SETTING;
            if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                load_screen_locked(SCREEN_ID_ADD_ANOTHER__SENSOR);
                lvgl_port_unlock();
            }
            g_mode = MODE_OPERATIONAL;
        } else if (action == AUTH_ACTION_ADD_FINGERPRINT) {
            display_show_fingerprint_screen("Add Fingerprint", "Place your finger on the sensor");
            g_mode = MODE_FINGERPRINT_ENROLL;
            if (fp_enroll() == ESP_OK) {
                display_fingerprint_status("Registration completed");
            } else {
                display_fingerprint_status("Denied");
            }
            g_mode = s_ui_online ? MODE_OPERATIONAL : MODE_OFFLINE;
            vTaskDelay(pdMS_TO_TICKS(1200));
            display_show_dashboard(s_ui_online);
        } else if (turn_on) {
            display_fingerprint_status("Turning hub on");
            if (wifi_resume_from_offline_mode()) {
                set_switch_checked(sw, true);
                display_show_dashboard(true);
            } else {
                set_switch_checked(sw, false);
                g_mode = MODE_OFFLINE;
                display_show_dashboard(false);
            }
        } else {
            display_fingerprint_status("Turning hub off");
            wifi_enter_offline_mode();
            set_switch_checked(sw, false);
            display_show_dashboard(false);
        }
    } else {
        display_fingerprint_status("Denied");
        vTaskDelay(pdMS_TO_TICKS(1200));
        if (action == AUTH_ACTION_HUB_TOGGLE && turn_on) {
            set_switch_checked(sw, false);
            g_mode = MODE_OFFLINE;
            display_show_dashboard(false);
        } else if (action == AUTH_ACTION_HUB_TOGGLE) {
            set_switch_checked(sw, true);
            g_mode = MODE_OPERATIONAL;
            display_show_dashboard(true);
        } else {
            g_mode = MODE_OPERATIONAL;
            display_show_dashboard(s_ui_online);
        }
    }

    s_auth_task = NULL;
    vTaskDelete(NULL);
}

static void start_auth_action(int action, lv_obj_t *sw, bool turn_on)
{
    if (s_auth_task != NULL) {
        display_fingerprint_status("Auth in progress");
        return;
    }

    auth_task_arg_t *arg = malloc(sizeof(*arg));
    if (!arg) {
        display_fingerprint_status("Auth unavailable");
        return;
    }
    arg->sw = sw;
    arg->turn_on = turn_on;
    arg->action = action;

    if (xTaskCreate(auth_toggle_task, "fp_auth", 6144, arg, 5, &s_auth_task) != pdPASS) {
        free(arg);
        s_auth_task = NULL;
        display_fingerprint_status("Auth unavailable");
    }
}

static void critical_toggle_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED || s_switch_internal) return;

    lv_obj_t *sw = lv_event_get_target(e);
    bool is_on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    ESP_LOGI(TAG, "Critical Toggle requested: %s", is_on ? "ON" : "OFF");

    if (s_auth_task != NULL) {
        set_switch_checked(sw, !is_on);
        display_fingerprint_status("Auth in progress");
        return;
    }

    bool can_toggle_on = (g_mode == MODE_OFFLINE && is_on);
    bool can_toggle_off = (g_mode == MODE_OPERATIONAL && !is_on);
    if (!can_toggle_on && !can_toggle_off) {
        set_switch_checked(sw, g_mode != MODE_OFFLINE);
        return;
    }

    start_auth_action(AUTH_ACTION_HUB_TOGGLE, sw, is_on);
}

static esp_err_t lcd_hw_init(void)
{
    ESP_LOGI(TAG, "Display init: SPI bus starting, free internal heap=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    spi_bus_config_t bus = {
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = TOUCH_PIN_MISO,
        .sclk_io_num     = LCD_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Display init: SPI bus ready");

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
    ESP_LOGI(TAG, "Display init: creating LCD panel IO");
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: LCD panel IO create failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_endian     = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_LOGI(TAG, "Display init: creating ILI9341 panel");
    err = esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: ILI9341 panel create failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Display init: panel reset");
    err = esp_lcd_panel_reset(panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: panel reset failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Display init: panel init");
    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: panel init failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "Display init: panel orientation/on");
    err = esp_lcd_panel_invert_color(panel, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: invert color failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_mirror(panel, true, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: panel mirror failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_set_gap(panel, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: panel gap failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: panel on failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    /* T_IRQ: active-low open-drain output from XPT2046 — pull up internally */
    gpio_config_t irq_cfg = {
        .pin_bit_mask = 1ULL << TOUCH_PIN_IRQ,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_LOGI(TAG, "Display init: touch IRQ GPIO config");
    err = gpio_config(&irq_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: touch IRQ GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register XPT2046 as a separate SPI device on SPI2 */
    spi_device_interface_config_t tp_devcfg = {
        .clock_speed_hz = 1000 * 1000,  // 1 MHz
        .mode           = 0,                 // SPI mode 0 (CPOL=0, CPHA=0)
        .spics_io_num   = TOUCH_PIN_CS,      // GPIO5
        .queue_size     = 1,
        .flags          = 0,
    };
    ESP_LOGI(TAG, "Display init: adding touch SPI device");
    err = spi_bus_add_device(LCD_SPI_HOST, &tp_devcfg, &s_tp_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: touch SPI device add failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Hand off to LVGL port */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_LOGI(TAG, "Display init: LVGL port init");
    err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: LVGL port init failed: %s", esp_err_to_name(err));
        return err;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = false,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .flags         = { .buff_dma = true },
    };
    ESP_LOGI(TAG, "Display init: adding LVGL display, free internal heap=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (!lvgl_port_add_disp(&disp_cfg)) {
        ESP_LOGE(TAG, "Display init: lvgl_port_add_disp returned NULL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Display init: LVGL display registered");
    return ESP_OK;
}

/* ── UI build (called once LVGL task is ready) ───────────────────────────── */

static void load_screen_locked(enum ScreensEnum screen)
{
    ui_ensure_screen(screen);
    configure_screen_locked(screen);
    s_current_screen = screen;
    loadScreen(screen);
}

static void set_switch_checked_locked(lv_obj_t *sw, bool checked)
{
    if (!sw) return;
    s_switch_internal = true;
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }
    s_switch_internal = false;
}

static void set_label_locked(lv_obj_t *label, const char *text)
{
    if (label && text) {
        lv_label_set_text(label, text);
    }
}

static void cache_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void cache_lock(void)
{
    if (s_display_cache_mutex) {
        xSemaphoreTake(s_display_cache_mutex, portMAX_DELAY);
    }
}

static void cache_unlock(void)
{
    if (s_display_cache_mutex) {
        xSemaphoreGive(s_display_cache_mutex);
    }
}

static bool display_is_ready(void)
{
    return s_display_state == DISPLAY_READY;
}

static const char *nonnull_text(const char *text, const char *fallback)
{
    return (text && text[0]) ? text : fallback;
}

static void set_online_dashboard_locked(void)
{
    load_screen_locked(SCREEN_ID_HUB_ONLINE);
    set_switch_checked_locked(objects.obj1, true);
    set_label_locked(objects.hub_status, "Online");
    set_label_locked(objects.welcome_home, "Welcome, user");
    set_label_locked(objects.sensor_info,
                     strlen(g_home_name) > 0 ? g_home_name : "  HUB_loc\n(other info)");
}

static void set_offline_dashboard_locked(bool setup)
{
    load_screen_locked(SCREEN_ID_HUB_OFFLINE);
    set_switch_checked_locked(objects.obj22, false);
    set_label_locked(objects.hub_status_1, setup ? "Setup" : "Offline");
    set_label_locked(objects.welcome_home_1, setup ? "Glazia Hub" : "Welcome, user");
    set_label_locked(objects.sensor_info_1,
                     setup ? "Press button\nfor BLE setup" : "  HUB_loc\n(other info)");
}

static const char *fingerprint_phase_for_title(const char *title)
{
    if (title && (strstr(title, "Verify") || strstr(title, "verify"))) {
        return "Verifying your fingerprint";
    }
    return "Registering your fingerprint";
}

static const char *fingerprint_message_normalize(const char *message)
{
    if (!message) return NULL;
    if (strcmp(message, "Place finger on sensor") == 0 ||
        strcmp(message, "Scan your fingerprint") == 0 ||
        strcmp(message, "Try fingerprint again") == 0) {
        return "Place your finger on the sensor";
    }
    if (strcmp(message, "Processing . . .") == 0 ||
        strcmp(message, "Processing") == 0) {
        return "Processing...";
    }
    if (strcmp(message, "Fingerprint registered") == 0) {
        return "Registration completed";
    }
    if (strcmp(message, "Access denied") == 0 ||
        strcmp(message, "Enrollment failed") == 0) {
        return "Denied";
    }
    return message;
}

static void set_dashboard_values_locked(float temp, float hum)
{
    char buf[16];
    if (temp < 0.0f) temp = 0.0f;
    if (temp > 50.0f) temp = 50.0f;
    if (hum < 0.0f) hum = 0.0f;
    if (hum > 100.0f) hum = 100.0f;

    snprintf(buf, sizeof(buf), "%.1f", temp);
    set_label_locked(objects.temp_val, buf);
    set_label_locked(objects.temp_val_1, buf);
    if (objects.temp_arc) lv_arc_set_value(objects.temp_arc, (int)temp);
    if (objects.temp_arc_1) lv_arc_set_value(objects.temp_arc_1, (int)temp);

    snprintf(buf, sizeof(buf), "%.0f", hum);
    set_label_locked(objects.hum_val, buf);
    set_label_locked(objects.hum_val_1, buf);
    if (objects.hum_arc) lv_arc_set_value(objects.hum_arc, (int)hum);
    if (objects.hum_arc_1) lv_arc_set_value(objects.hum_arc_1, (int)hum);
}

static void cache_apply_locked(void)
{
    cache_lock();

    switch (s_display_cache.view) {
    case CACHE_VIEW_SETUP:
        set_offline_dashboard_locked(true);
        break;
    case CACHE_VIEW_ONLINE:
        set_online_dashboard_locked();
        break;
    case CACHE_VIEW_OFFLINE:
        set_offline_dashboard_locked(false);
        break;
    case CACHE_VIEW_FINGERPRINT:
        load_screen_locked(SCREEN_ID_FINGERPRINT_SETTING);
        set_label_locked(objects.obj53, nonnull_text(s_display_cache.fp_title, "Fingerprint"));
        set_label_locked(objects.obj49, nonnull_text(s_display_cache.fp_phase, "Registering your fingerprint"));
        set_label_locked(objects.obj52,
                         fingerprint_message_normalize(nonnull_text(s_display_cache.fp_message,
                                                                    "Place your finger on the sensor")));
        if (objects.obj51) lv_bar_set_value(objects.obj51, s_display_cache.fp_progress, LV_ANIM_OFF);
        break;
    case CACHE_VIEW_NONE:
    default:
        break;
    }

    if (s_display_cache.line1[0] || s_display_cache.line2[0]) {
        if (s_current_screen == SCREEN_ID_HUB_ONLINE) {
            set_label_locked(objects.hub_status, s_display_cache.line1);
            set_label_locked(objects.sensor_info, s_display_cache.line2);
        } else if (s_current_screen == SCREEN_ID_HUB_OFFLINE) {
            set_label_locked(objects.hub_status_1, s_display_cache.line1);
            set_label_locked(objects.sensor_info_1, s_display_cache.line2);
        }
    }

    if (s_display_cache.home_name[0]) {
        if (s_current_screen == SCREEN_ID_HUB_ONLINE) {
            set_label_locked(objects.sensor_info, s_display_cache.home_name);
        } else if (s_current_screen == SCREEN_ID_HUB_OFFLINE) {
            set_label_locked(objects.sensor_info_1, s_display_cache.home_name);
        }
    }

    if (s_display_cache.has_temp_hum) {
        set_dashboard_values_locked(s_display_cache.temp, s_display_cache.hum);
    }

    cache_unlock();
}

static void nav_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    enum ScreensEnum target = SCREEN_ID_HUB_ONLINE;
    if (s_current_screen == SCREEN_ID_ABOUT_GLAZIA ||
        s_current_screen == SCREEN_ID_SENSOR_NODES_SETTING ||
        s_current_screen == SCREEN_ID_FINGERPRINT_SETTING) {
        target = SCREEN_ID_SETTINGS_MENU;
    } else if (s_current_screen == SCREEN_ID_ADD_ANOTHER__SENSOR) {
        display_clear_sensor_notifications();
        target = SCREEN_ID_SENSOR_NODES_SETTING;
    } else {
        target = s_ui_online ? SCREEN_ID_HUB_ONLINE : SCREEN_ID_HUB_OFFLINE;
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        load_screen_locked(target);
        lvgl_port_unlock();
    }
}

static void settings_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_prev_screen = s_ui_online ? SCREEN_ID_HUB_ONLINE : SCREEN_ID_HUB_OFFLINE;
    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        load_screen_locked(SCREEN_ID_SETTINGS_MENU);
        lvgl_port_unlock();
    }
}

static void sensor_nodes_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        load_screen_locked(SCREEN_ID_SENSOR_NODES_SETTING);
        refresh_sensor_nodes_locked();
        lvgl_port_unlock();
    }
}

static void about_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        load_screen_locked(SCREEN_ID_ABOUT_GLAZIA);
        lvgl_port_unlock();
    }
}

static void add_fingerprint_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_auth_action(AUTH_ACTION_ADD_FINGERPRINT, NULL, false);
}

static void add_sensor_auth_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_auth_action(AUTH_ACTION_ADD_SENSOR, NULL, false);
}

static void add_sensor_start_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    sensor_pairing_open_window();
}

static void sensor_switch_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || s_switch_internal) return;
    lv_obj_t *sw = lv_event_get_target(e);
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    espnow_set_sensor_enabled(index, enabled);
}

static void create_sensor_row(lv_obj_t *parent, int index, const char *name, bool enabled)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_pos(row, 9, 9 + index * 54);
    lv_obj_set_size(row, 219, 50);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, 160, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(label, 41, 20);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_pos(sw, 165, 15);
    lv_obj_set_size(sw, 50, 20);
    set_switch_checked_locked(sw, enabled);
    lv_obj_add_event_cb(sw, sensor_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)index);
}

static void configure_screen_locked(enum ScreensEnum screen)
{
    if (screen < _SCREEN_ID_FIRST || screen > _SCREEN_ID_LAST || s_screen_configured[screen]) {
        return;
    }

    switch (screen) {
    case SCREEN_ID_HUB_ONLINE:
        s_critical_sw = objects.obj1;
        if (objects.obj1) lv_obj_add_event_cb(objects.obj1, critical_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
        if (objects.button) lv_obj_add_event_cb(objects.button, settings_cb, LV_EVENT_CLICKED, NULL);
        set_dashboard_values_locked(0.0f, 0.0f);
        break;

    case SCREEN_ID_HUB_OFFLINE:
        if (objects.obj22) lv_obj_add_event_cb(objects.obj22, critical_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
        if (objects.button_1) lv_obj_add_event_cb(objects.button_1, settings_cb, LV_EVENT_CLICKED, NULL);
        break;

    case SCREEN_ID_SETTINGS_MENU:
        if (objects.obj14) lv_obj_add_event_cb(objects.obj14, nav_back_cb, LV_EVENT_CLICKED, NULL);
        if (objects.fingerprint_option) lv_obj_add_event_cb(objects.fingerprint_option, add_fingerprint_cb, LV_EVENT_CLICKED, NULL);
        if (objects.sensor_nodes_option) lv_obj_add_event_cb(objects.sensor_nodes_option, sensor_nodes_cb, LV_EVENT_CLICKED, NULL);
        if (objects.about_section) lv_obj_add_event_cb(objects.about_section, about_cb, LV_EVENT_CLICKED, NULL);
        break;

    case SCREEN_ID_SENSOR_NODES_SETTING:
        if (objects.settings_menu_cont_1) {
            lv_obj_set_scroll_dir(objects.settings_menu_cont_1, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(objects.settings_menu_cont_1, LV_SCROLLBAR_MODE_ACTIVE);
        }
        if (objects.obj44) lv_obj_add_event_cb(objects.obj44, nav_back_cb, LV_EVENT_CLICKED, NULL);
        if (objects.add_sensor_button) lv_obj_add_event_cb(objects.add_sensor_button, add_sensor_auth_cb, LV_EVENT_CLICKED, NULL);
        break;

    case SCREEN_ID_ABOUT_GLAZIA: {
        lv_obj_t *about_parent = objects.obj48 ? lv_obj_get_parent(objects.obj48) : NULL;
        if (about_parent) {
            lv_obj_set_scroll_dir(about_parent, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(about_parent, LV_SCROLLBAR_MODE_ACTIVE);
        }
        if (objects.obj47) lv_obj_add_event_cb(objects.obj47, nav_back_cb, LV_EVENT_CLICKED, NULL);
        break;
    }

    case SCREEN_ID_FINGERPRINT_SETTING:
        if (objects.obj54) lv_obj_add_event_cb(objects.obj54, nav_back_cb, LV_EVENT_CLICKED, NULL);
        if (objects.obj51) lv_bar_set_value(objects.obj51, 0, LV_ANIM_OFF);
        break;

    case SCREEN_ID_ADD_ANOTHER__SENSOR:
        if (objects.added_sensor_data) {
            lv_obj_set_scroll_dir(objects.added_sensor_data, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(objects.added_sensor_data, LV_SCROLLBAR_MODE_ACTIVE);
            lv_obj_clean(objects.added_sensor_data);
        }
        if (objects.obj55) lv_obj_add_event_cb(objects.obj55, nav_back_cb, LV_EVENT_CLICKED, NULL);
        if (objects.obj59) lv_obj_add_event_cb(objects.obj59, add_sensor_start_cb, LV_EVENT_CLICKED, NULL);
        break;

    default:
        break;
    }

    s_screen_configured[screen] = true;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

static void display_init_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Display init task started");
    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_err_t err = lcd_hw_init();
    if (err != ESP_OK) {
        s_display_state = DISPLAY_FAILED;
        ESP_LOGE(TAG, "Display init failed during hardware/LVGL setup: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* Wait up to 2 s for the LVGL task to be ready before building the UI. */
    ESP_LOGI(TAG, "Display init: waiting for LVGL lock");
    if (!lvgl_port_lock(pdMS_TO_TICKS(2000))) {
        s_display_state = DISPLAY_FAILED;
        ESP_LOGE(TAG, "LVGL lock timed out — display not ready");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Display init: ui_init starting");
    ui_init();
    ESP_LOGI(TAG, "Display init: ui_init done");
    configure_screen_locked(SCREEN_ID_HUB_ONLINE);

    /* Register XPT2046 as LVGL pointer input device inside the lock */
    ESP_LOGI(TAG, "Display init: registering touch input");
    lv_indev_drv_init(&s_tp_drv);
    s_tp_drv.type         = LV_INDEV_TYPE_POINTER;
    s_tp_drv.read_cb      = xpt2046_read_cb;
    s_tp_drv.scroll_limit = 50;
    s_tp_indev = lv_indev_drv_register(&s_tp_drv);
    if (!s_tp_indev) {
        ESP_LOGW(TAG, "Display init: touch input registration returned NULL");
    }

    s_display_state = DISPLAY_READY;
    cache_apply_locked();

    lvgl_port_unlock();

    fp_set_display_cb(display_fingerprint_status);

    ESP_LOGI(TAG, "Display ready — SPI direct, MOSI=GPIO%d, touch enabled", LCD_PIN_MOSI);
    vTaskDelete(NULL);
}

void display_init(void)
{
    ESP_LOGI(TAG, "Display init requested");
    if (!s_display_cache_mutex) {
        s_display_cache_mutex = xSemaphoreCreateMutex();
        if (!s_display_cache_mutex) {
            ESP_LOGE(TAG, "Display init: failed to create cache mutex");
            s_display_state = DISPLAY_FAILED;
            return;
        }
    }

    fp_set_display_cb(display_fingerprint_status);

    if (s_display_state == DISPLAY_READY || s_display_state == DISPLAY_STARTING) {
        ESP_LOGI(TAG, "Display init skipped, state=%d", (int)s_display_state);
        return;
    }

    s_display_state = DISPLAY_STARTING;
    if (xTaskCreate(display_init_task, "display_init", 8192, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        s_display_state = DISPLAY_FAILED;
        ESP_LOGE(TAG, "Display init: failed to create display init task");
        return;
    }
    ESP_LOGI(TAG, "Display init queued");
}

static void refresh_sensor_nodes_locked(void)
{
    ui_ensure_screen(SCREEN_ID_SENSOR_NODES_SETTING);
    configure_screen_locked(SCREEN_ID_SENSOR_NODES_SETTING);
    if (!objects.settings_menu_cont_1) return;

    lv_obj_clean(objects.settings_menu_cont_1);
    int count = espnow_get_sensor_count();
    if (count == 0) {
        lv_obj_t *label = lv_label_create(objects.settings_menu_cont_1);
        lv_label_set_text(label, "No sensor added yet");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        char name[24];
        bool enabled = true;
        bool paired = false;
        if (espnow_get_sensor_info(i, name, sizeof(name), &enabled, &paired)) {
            create_sensor_row(objects.settings_menu_cont_1, i, name, enabled);
        }
    }
}

void display_show(const char *line1, const char *line2)
{
    cache_lock();
    cache_copy(s_display_cache.line1, sizeof(s_display_cache.line1), line1);
    cache_copy(s_display_cache.line2, sizeof(s_display_cache.line2), line2);
    cache_unlock();

    ESP_LOGI(TAG, "Display: [%s] [%s]", line1 ? line1 : "", line2 ? line2 : "");
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached message only (state=%d)", (int)s_display_state);
        return;
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        if (s_current_screen == SCREEN_ID_HUB_ONLINE) {
            set_label_locked(objects.hub_status, line1);
            set_label_locked(objects.sensor_info, line2 ? line2 : "");
        } else if (s_current_screen == SCREEN_ID_HUB_OFFLINE) {
            set_label_locked(objects.hub_status_1, line1);
            set_label_locked(objects.sensor_info_1, line2 ? line2 : "");
        }
        lvgl_port_unlock();
    }
}

void display_show_setup_prompt(void)
{
    s_ui_online = false;
    cache_lock();
    s_display_cache.view = CACHE_VIEW_SETUP;
    s_display_cache.line1[0] = '\0';
    s_display_cache.line2[0] = '\0';
    cache_unlock();

    ESP_LOGI(TAG, "Display setup prompt");
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached setup prompt only (state=%d)", (int)s_display_state);
        return;
    }

    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    set_offline_dashboard_locked(true);
    lvgl_port_unlock();
}

void display_fingerprint_status(const char *message)
{
    if (!message) return;
    cache_lock();
    cache_copy(s_display_cache.fp_message, sizeof(s_display_cache.fp_message),
               fingerprint_message_normalize(message));
    s_display_cache.view = CACHE_VIEW_FINGERPRINT;
    cache_unlock();

    ESP_LOGI(TAG, "Fingerprint panel: %s", message);
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached fingerprint status only (state=%d)", (int)s_display_state);
        return;
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        set_label_locked(objects.obj52, fingerprint_message_normalize(message));
        lvgl_port_unlock();
    }
}

void display_hub_location(const char *home_name)
{
    if (!home_name) return;
    cache_lock();
    cache_copy(s_display_cache.home_name, sizeof(s_display_cache.home_name), home_name);
    cache_unlock();

    ESP_LOGI(TAG, "Display hub location: %s", home_name);
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached hub location only (state=%d)", (int)s_display_state);
        return;
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        if (s_current_screen == SCREEN_ID_HUB_ONLINE) {
            set_label_locked(objects.sensor_info, home_name);
        } else if (s_current_screen == SCREEN_ID_HUB_OFFLINE) {
            set_label_locked(objects.sensor_info_1, home_name);
        }
        lvgl_port_unlock();
    }
}

void display_sensor_location(const char *mac_str)
{
    (void)mac_str;
}

void display_sensor_list(void)
{
    display_refresh_sensor_nodes();
}

void display_show_dashboard(bool online)
{
    s_ui_online = online;
    g_mode = online ? MODE_OPERATIONAL : MODE_OFFLINE;
    cache_lock();
    s_display_cache.view = online ? CACHE_VIEW_ONLINE : CACHE_VIEW_OFFLINE;
    s_display_cache.line1[0] = '\0';
    s_display_cache.line2[0] = '\0';
    cache_unlock();

    ESP_LOGI(TAG, "Display dashboard: %s", online ? "online" : "offline");
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached dashboard only (state=%d)", (int)s_display_state);
        return;
    }

    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (online) {
        set_online_dashboard_locked();
    } else {
        set_offline_dashboard_locked(false);
    }
    lvgl_port_unlock();
}

void display_show_fingerprint_screen(const char *title, const char *prompt)
{
    const char *phase = fingerprint_phase_for_title(title);
    const char *message = fingerprint_message_normalize(nonnull_text(prompt, "Place your finger on the sensor"));

    cache_lock();
    s_display_cache.view = CACHE_VIEW_FINGERPRINT;
    cache_copy(s_display_cache.fp_title, sizeof(s_display_cache.fp_title), title ? title : "Fingerprint");
    cache_copy(s_display_cache.fp_phase, sizeof(s_display_cache.fp_phase), phase);
    cache_copy(s_display_cache.fp_message, sizeof(s_display_cache.fp_message), message);
    s_display_cache.fp_progress = 0;
    cache_unlock();

    ESP_LOGI(TAG, "Fingerprint screen: title='%s' phase='%s' message='%s'",
             title ? title : "Fingerprint", phase, message);
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached fingerprint screen only (state=%d)", (int)s_display_state);
        return;
    }

    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    s_prev_screen = s_current_screen;
    load_screen_locked(SCREEN_ID_FINGERPRINT_SETTING);
    set_label_locked(objects.obj53, title ? title : "Fingerprint");
    set_label_locked(objects.obj49, phase);
    set_label_locked(objects.obj52, message);
    if (objects.obj51) lv_bar_set_value(objects.obj51, 0, LV_ANIM_OFF);
    lvgl_port_unlock();
}

void display_fingerprint_phase(const char *phase, const char *message)
{
    const char *normalized = fingerprint_message_normalize(message);

    cache_lock();
    s_display_cache.view = CACHE_VIEW_FINGERPRINT;
    cache_copy(s_display_cache.fp_phase, sizeof(s_display_cache.fp_phase), phase);
    cache_copy(s_display_cache.fp_message, sizeof(s_display_cache.fp_message), normalized);
    cache_unlock();

    ESP_LOGI(TAG, "Fingerprint panel: phase='%s' message='%s'",
             phase ? phase : "", message ? message : "");
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached fingerprint phase only (state=%d)", (int)s_display_state);
        return;
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        set_label_locked(objects.obj49, phase);
        set_label_locked(objects.obj52, normalized);
        lvgl_port_unlock();
    }
}

void display_fingerprint_progress(uint8_t percent)
{
    if (percent > 100) percent = 100;
    cache_lock();
    s_display_cache.fp_progress = percent;
    cache_unlock();

    if (!display_is_ready()) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    if (objects.obj51) lv_bar_set_value(objects.obj51, percent, LV_ANIM_OFF);
    lvgl_port_unlock();
}

void display_update_temp_hum(float temp, float hum)
{
    cache_lock();
    s_display_cache.has_temp_hum = true;
    s_display_cache.temp = temp;
    s_display_cache.hum = hum;
    cache_unlock();

    if (!display_is_ready()) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    set_dashboard_values_locked(temp, hum);
    lvgl_port_unlock();
}

void display_refresh_sensor_nodes(void)
{
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, skipped sensor-node refresh (state=%d)", (int)s_display_state);
        return;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    refresh_sensor_nodes_locked();
    lvgl_port_unlock();
}

void display_sensor_added_notification(const char *name)
{
    if (!name) return;
    ESP_LOGI(TAG, "Display sensor added notification: %s", name);
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, skipped sensor-added notification (state=%d)", (int)s_display_state);
        return;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(300))) return;
    ui_ensure_screen(SCREEN_ID_ADD_ANOTHER__SENSOR);
    configure_screen_locked(SCREEN_ID_ADD_ANOTHER__SENSOR);
    if (!objects.added_sensor_data) {
        lvgl_port_unlock();
        return;
    }

    int index = lv_obj_get_child_cnt(objects.added_sensor_data);
    lv_obj_t *row = lv_obj_create(objects.added_sensor_data);
    lv_obj_set_pos(row, 7, 8 + index * 30);
    lv_obj_set_size(row, 180, 26);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, 120, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s added", name);
    lv_label_set_text(label, buf);
    lv_obj_set_pos(label, 38, 6);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lvgl_port_unlock();
}

void display_clear_sensor_notifications(void)
{
    if (!display_is_ready()) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(300))) return;
    if (objects.added_sensor_data) lv_obj_clean(objects.added_sensor_data);
    lvgl_port_unlock();
}
