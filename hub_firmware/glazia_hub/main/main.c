#include "nvs_flash.h"
#include "nvs_storage.h"
#include "wifi.h"
#include "button.h"
#include "display.h"
#include "fingerprint.h"
#include "state.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

// Instantiate globals
hub_mode_t g_mode = MODE_IDLE;
char g_wifi_ssid[64] = {0}, g_wifi_password[64] = {0}, g_provisioning_token[64] = {0};
char g_hub_mac[18] = {0}, g_hub_secret[128] = {0}, g_home_id[64] = {0}, g_home_name[64] = {0}, g_user_name[64] = {0};
char g_pending_sensor_mac[18] = {0};
char g_pending_provision_key[33] = {0};

void app_main(void) {
    ESP_LOGI(TAG, "APP CONSOLE: UART0 active");
    ESP_LOGI(TAG, "Boot: app_main starting");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init needs erase: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_hub_mac, sizeof(g_hub_mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "HUB MAC: %s", g_hub_mac);

    ESP_LOGI(TAG, "display_init starting");
    display_init();
    ESP_LOGI(TAG, "display_init queued/done");

    ESP_LOGI(TAG, "fp_init starting");
    esp_err_t fp_err = fp_init();
    if (fp_err != ESP_OK) {
        ESP_LOGW(TAG, "Fingerprint init failed: %s", esp_err_to_name(fp_err));
    } else {
        ESP_LOGI(TAG, "Fingerprint driver ready");
    }

    ESP_LOGI(TAG, "button_init starting");
    button_init();
    ESP_LOGI(TAG, "Button input ready");

    // AUTO-CONNECT LOGIC
    ESP_LOGI(TAG, "Credential check starting");
    if (nvs_load_credentials()) {
        ESP_LOGI(TAG, "Saved credentials found! Reconnecting...");
        g_mode = MODE_WIFI_CONNECTING;
        ESP_LOGI(TAG, "Mode transition: WIFI_CONNECTING");
        wifi_connect(g_wifi_ssid, g_wifi_password);
    } else {
        g_mode = MODE_IDLE;
        ESP_LOGI(TAG, "No saved credentials. Mode transition: IDLE, waiting for button BLE setup");
        display_show_setup_prompt();
    }

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
