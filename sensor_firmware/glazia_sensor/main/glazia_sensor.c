/**
 * glazia_sensor.c — Glazia Sensor Node
 * ──────────────────────────────────────
 * Target: ESP32-C3 (change idf.py set-target if using a different board)
 *
 * Flow:
 *   1. Boot → init ESP-NOW in WiFi-AP mode (no router needed)
 *   2. Wait for HELLO packet from hub
 *   3. Extract hub MAC from packet, add as ESP-NOW peer
 *   4. Send ACK back to hub
 *   5. Periodically send fake PULSE events to hub
 *
 * Packet format is shared with hub's espnow.c:
 *   type = 0x01 (HELLO) | 0x02 (ACK) | 0x03 (EVENT)
 *   payload = 128-byte string (JSON for EVENT)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "SENSOR";

/* ── Packet types (mirror hub's espnow.c) ──────────────────────────────── */
#define PKT_HELLO 0x01
#define PKT_ACK   0x02
#define PKT_EVENT 0x03

typedef struct {
    uint8_t type;
    char    payload[128];
} __attribute__((packed)) espnow_packet_t;

/* ── Sensor state ───────────────────────────────────────────────────────── */
static uint8_t s_hub_mac[6]    = {0};
static bool    s_hub_paired    = false;
static char    s_own_mac_str[18] = {0};

/* ── Forward declaration ────────────────────────────────────────────────── */
static void start_event_loop(void);

/* ── Send callback ──────────────────────────────────────────────────────── */
static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    ESP_LOGI(TAG, "Send → %02X:%02X:%02X:%02X:%02X:%02X : %s",
        mac_addr[0], mac_addr[1], mac_addr[2],
        mac_addr[3], mac_addr[4], mac_addr[5],
        status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

/* ── Receive callback ───────────────────────────────────────────────────── */
static void espnow_recv_cb(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len)
{
    if (len < (int)sizeof(espnow_packet_t)) return;

    const espnow_packet_t *pkt = (const espnow_packet_t *)data;

    if (pkt->type == PKT_HELLO) {
        const uint8_t *src = info->src_addr;
        ESP_LOGI(TAG, "HELLO from hub %02X:%02X:%02X:%02X:%02X:%02X — pairing!",
            src[0], src[1], src[2], src[3], src[4], src[5]);

        /* Store hub MAC */
        memcpy(s_hub_mac, src, 6);

        /* Parse hub channel from payload ("HELLO_FROM_HUB:ch=N") and lock to it */
        int hub_ch = 0;
        if (sscanf(pkt->payload, "HELLO_FROM_HUB:ch=%d", &hub_ch) == 1 && hub_ch > 0) {
            esp_wifi_set_channel(hub_ch, WIFI_SECOND_CHAN_NONE);
            ESP_LOGI(TAG, "Locked to hub channel %d", hub_ch);
        }

        /* Add hub as ESP-NOW peer */
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, s_hub_mac, 6);
        peer.channel = 0;   /* 0 = follow current channel */
        peer.encrypt = false;

        if (!esp_now_is_peer_exist(s_hub_mac)) {
            esp_now_add_peer(&peer);
        }

        /* Send ACK */
        espnow_packet_t ack = { .type = PKT_ACK };
        snprintf(ack.payload, sizeof(ack.payload), "ACK_FROM_%s", s_own_mac_str);
        esp_now_send(s_hub_mac, (uint8_t *)&ack, sizeof(ack));

        ESP_LOGI(TAG, "ACK sent — ESP-NOW link established!");
        s_hub_paired = true;

        /* Start sending events */
        start_event_loop();
    }
}

/* ── Fake event loop ────────────────────────────────────────────────────── */
static void event_task(void *arg)
{
    /*
     * Fake "pulse" sensor — simulates a door/motion trigger.
     * Alternates between INFO and WARNING every other event.
     * Sends one event every 10 seconds.
     */
    int count = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));

        if (!s_hub_paired) continue;

        espnow_packet_t pkt = { .type = PKT_EVENT };
        snprintf(pkt.payload, sizeof(pkt.payload), "test_data %d", count + 1);

        esp_err_t err = esp_now_send(s_hub_mac, (uint8_t *)&pkt, sizeof(pkt));
        if (err != ESP_OK) {
            /* Radio busy — wait briefly and retry once */
            vTaskDelay(pdMS_TO_TICKS(500));
            err = esp_now_send(s_hub_mac, (uint8_t *)&pkt, sizeof(pkt));
        }

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "→ %s", pkt.payload);
        } else {
            ESP_LOGE(TAG, "Send failed (gave up): %s", esp_err_to_name(err));
        }
        count++;
    }
}

static void start_event_loop(void)
{
    static bool started = false;
    if (!started) {
        started = true;
        xTaskCreate(event_task, "events", 3072, NULL, 5, NULL);
    }
}

/* ── WiFi init (needed for ESP-NOW) ────────────────────────────────────── */
static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Long-range mode for better ESP-NOW range */
    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);

    ESP_LOGI(TAG, "WiFi STA started (channel scanning active)");
}

/*
 * Channel scan task — cycles through all 13 WiFi channels every 400ms.
 * Runs until hub's HELLO is received (s_hub_paired becomes true).
 * This way the sensor doesn't need to know the hub's channel in advance.
 */
static void channel_scan_task(void *arg)
{
    uint8_t ch = 1;
    ESP_LOGI(TAG, "Channel scanning started — will lock when HELLO received");
    while (!s_hub_paired) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        ch = (ch % 13) + 1;   /* 1 → 2 → ... → 13 → 1 */
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    /* Hub found — stay on current channel */
    uint8_t locked; wifi_second_chan_t sc;
    esp_wifi_get_channel(&locked, &sc);
    ESP_LOGI(TAG, "Channel locked to %d after HELLO received", locked);
    vTaskDelete(NULL);
}

/* ── Entry point ────────────────────────────────────────────────────────── */
void app_main(void)
{
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Read own MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_own_mac_str, sizeof(s_own_mac_str),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, " Glazia Sensor Node");
    ESP_LOGI(TAG, " MAC: %s", s_own_mac_str);
    ESP_LOGI(TAG, " (use this MAC to generate the QR)");
    ESP_LOGI(TAG, "====================================");

    wifi_init();

    /* Scan channels until hub HELLO is received */
    xTaskCreate(channel_scan_task, "ch_scan", 2048, NULL, 4, NULL);

    /* Init ESP-NOW */
    ESP_ERROR_CHECK(esp_now_init());
    esp_now_register_send_cb(espnow_send_cb);
    esp_now_register_recv_cb(espnow_recv_cb);

    ESP_LOGI(TAG, "Waiting for HELLO from hub...");
    ESP_LOGI(TAG, "(pair hub first: press hub button once after WiFi connects)");
    ESP_LOGI(TAG, "(then press hub button again to enter sensor pairing mode)");
    ESP_LOGI(TAG, "(then scan this sensor's QR on the app)");

    /* app_main can return — event_task keeps running */
}
