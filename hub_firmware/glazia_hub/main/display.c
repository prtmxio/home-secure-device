#include "display.h"
#include "state.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG   = "DISPLAY";
#define DISP_UART        UART_NUM_1
#define DISP_MSG_LEN     160
#define DISP_QUEUE_DEPTH 8

static QueueHandle_t disp_queue = NULL;

// ── Sender task — runs independently of any BLE/WiFi context ─────────────

static void display_task(void *arg)
{
    char msg[DISP_MSG_LEN];
    while (1) {
        if (xQueueReceive(disp_queue, msg, portMAX_DELAY)) {
            uart_write_bytes(DISP_UART, msg, strlen(msg));
            uart_write_bytes(DISP_UART, "\n", 1);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────

void display_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(DISP_UART, 512, 512, 0, NULL, 0);
    uart_param_config(DISP_UART, &cfg);
    uart_set_pin(DISP_UART, DISP_UART_TX, DISP_UART_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    disp_queue = xQueueCreate(DISP_QUEUE_DEPTH, DISP_MSG_LEN);
    xTaskCreate(display_task, "disp_task", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "Display ready (TX=GPIO%d → C6)", DISP_UART_TX);
}

void display_show(const char *line1, const char *line2)
{
    if (!disp_queue) return;

    char msg[DISP_MSG_LEN];
    if (line2 && line2[0] != '\0') {
        snprintf(msg, sizeof(msg), "STATUS:%s|%s", line1, line2);
    } else {
        snprintf(msg, sizeof(msg), "STATUS:%s", line1);
    }

    // Non-blocking — if queue is full, newest message wins (drop oldest)
    if (xQueueSend(disp_queue, msg, 0) == errQUEUE_FULL) {
        char dropped[DISP_MSG_LEN];
        xQueueReceive(disp_queue, dropped, 0);
        xQueueSend(disp_queue, msg, 0);
    }

    ESP_LOGI(TAG, "Display: [%s] [%s]", line1, line2 ? line2 : "");
}
