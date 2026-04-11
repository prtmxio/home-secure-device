#include "wifi.h"
#include "state.h"
#include "api_client.h"
#include "websocket.h"
#include "display.h"
#include "espnow.h"

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI";

/* Persistent — never deleted so the handler stays valid after initial connect */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int  s_retry_num      = 0;
static bool s_initial_connect = true;   /* true until first successful connection */
#define WIFI_MAX_RETRIES 10

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry_num++;
        ESP_LOGW(TAG, "WiFi dropped — retry %d", s_retry_num);

        if (s_initial_connect && s_retry_num >= WIFI_MAX_RETRIES) {
            /* First-time connect failed too many times — give up */
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        } else {
            /* Always retry after initial connect — never give up mid-operation */
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_wifi_connect();
            if (!s_initial_connect) {
                display_show("WiFi dropped", "Reconnecting...");
            }
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        /* If WiFi recovered mid-operation during sensor pairing — reopen window */
        if (!s_initial_connect && g_mode == MODE_SENSOR_PAIRING) {
            ESP_LOGI(TAG, "WiFi back — reopening sensor pairing window");
            display_show("WiFi back", "Resuming pairing");
            api_enable_sensor_pairing();
            websocket_start();
        }
    }
}

void wifi_connect(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Preparing WiFi connection...");
    display_show("WiFi", "Connecting...");

    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);

    if (strlen(password) > 0) {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

    // RESTING DELAY: Allows voltage regulator to recover from BLE shutdown
    vTaskDelay(pdMS_TO_TICKS(1500));

    esp_wifi_start();

    // TX POWER LIMIT: Constrains peak current to prevent USB disconnects
    esp_wifi_set_max_tx_power(56);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP successfully!");
        s_initial_connect = false;   /* from here on, always retry on drop */

        espnow_init();

        if (strlen(g_hub_secret) > 0) {
            // Already registered (loaded from NVS on reboot) — skip re-registration
            g_mode = MODE_OPERATIONAL;
            if (strlen(g_user_name) > 0) {
                display_show(g_home_name, g_user_name);
            } else {
                display_show("Hub Ready!", g_home_name);
            }
            ESP_LOGI(TAG, "Already registered, going operational");

            // Restore ESP-NOW links to any previously paired sensors
            espnow_reconnect_saved_sensors();
        } else {
            // First time — register with server
            display_show("WiFi Connected", "Registering...");
            api_register_hub();
        }

    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", ssid);
        display_show("WiFi Error", "Check credentials");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    /* Do NOT delete s_wifi_event_group — handler uses it on future disconnects */
}
