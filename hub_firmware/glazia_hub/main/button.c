#include "button.h"
#include "state.h"
#include "api_client.h"
#include "ble.h"
#include "display.h"
#include "websocket.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "BUTTON";

/* ── Sensor pairing: 2-minute auto-stop ──────────────────────────────────── */
#define SENSOR_PAIR_TIMEOUT_MS (2 * 60 * 1000)   /* 2 minutes */

static TimerHandle_t s_pair_timer = NULL;

static void pairing_timeout_cb(TimerHandle_t xTimer)
{
    if (g_mode == MODE_SENSOR_PAIRING) {
        ESP_LOGI(TAG, "Sensor pairing window expired (2 min)");
        g_mode = MODE_OPERATIONAL;
        display_show("Pairing ended", "Timeout 2 min");
        websocket_stop();
    }
}

/* ── Button task ─────────────────────────────────────────────────────────── */
void button_task(void *arg) {
    ESP_LOGI(TAG, "Button task running on GPIO %d", BUTTON_GPIO);
    while (1) {
        if (gpio_get_level(BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));   /* debounce */
            if (gpio_get_level(BUTTON_GPIO) == 0) {
                ESP_LOGI(TAG, "Button pressed! Current Mode: %d", g_mode);

                if (g_mode == MODE_IDLE) {
                    /* 1st press: start hub BLE pairing */
                    g_mode = MODE_HUB_PAIRING;
                    display_show("HUB PAIRING", "Connect via BLE");
                    ble_start();

                } else if (g_mode == MODE_OPERATIONAL) {
                    /* 2nd press: start sensor pairing (2-min window) */
                    g_mode = MODE_SENSOR_PAIRING;
                    display_show("SENSOR PAIRING", "Scan sensor QR");
                    api_enable_sensor_pairing();
                    websocket_start();

                    /* Start / restart the 2-minute timeout timer */
                    if (s_pair_timer) {
                        xTimerReset(s_pair_timer, pdMS_TO_TICKS(100));
                    }
                    ESP_LOGI(TAG, "Sensor pairing window opened (2 min)");
                }

                /* Wait for button release */
                while (gpio_get_level(BUTTON_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void button_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1,
    };
    gpio_config(&io_conf);

    /* One-shot 2-minute timer for sensor pairing auto-stop */
    s_pair_timer = xTimerCreate(
        "pair_timer",
        pdMS_TO_TICKS(SENSOR_PAIR_TIMEOUT_MS),
        pdFALSE,                  /* one-shot */
        NULL,
        pairing_timeout_cb
    );

    xTaskCreate(button_task, "button_task", 5120, NULL, 10, NULL);
}
