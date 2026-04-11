#include "espnow.h"
#include "state.h"
#include "display.h"
#include "api_client.h"
#include "websocket.h"
#include "nvs_storage.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ESPNOW";

// ── Packet types ──────────────────────────────────────────────────────────
#define PKT_HELLO   0x01
#define PKT_ACK     0x02
#define PKT_EVENT   0x03

typedef struct {
    uint8_t type;
    char    payload[128];
} espnow_packet_t;

// ── Event forwarding queue ────────────────────────────────────────────────
/*
 * recv_cb runs inside the WiFi task — blocking HTTP there starves the radio
 * and causes ESP-NOW MAC-layer NACKs on the next incoming frame.
 * Solution: recv_cb just enqueues; a dedicated task does the HTTP POST.
 */
#define EVENT_QUEUE_DEPTH 20

typedef struct {
    char mac_str[18];
    char payload[128];
} event_item_t;

static QueueHandle_t s_event_queue = NULL;

static void event_forward_task(void *arg)
{
    event_item_t item;
    while (1) {
        if (xQueueReceive(s_event_queue, &item, portMAX_DELAY) == pdTRUE) {
            api_send_event(item.mac_str, item.payload, "info");
        }
    }
}

// ── Multi-sensor table ────────────────────────────────────────────────────
#define MAX_SENSORS 10

typedef struct {
    uint8_t mac[6];
    bool    paired;
} sensor_entry_t;

static sensor_entry_t s_sensors[MAX_SENSORS];
static int            s_sensor_count = 0;

// ── MAC string → binary ───────────────────────────────────────────────────
static void mac_str_to_bytes(const char *mac_str, uint8_t *mac_bytes)
{
    sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
        &mac_bytes[0], &mac_bytes[1], &mac_bytes[2],
        &mac_bytes[3], &mac_bytes[4], &mac_bytes[5]);
}

static void mac_bytes_to_str(const uint8_t *mac, char *out)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ── Receive callback ──────────────────────────────────────────────────────
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data, int data_len)
{
    if (data_len < (int)sizeof(espnow_packet_t)) return;

    espnow_packet_t *pkt = (espnow_packet_t *)data;
    const uint8_t   *src  = recv_info->src_addr;

    char mac_str[18];
    mac_bytes_to_str(src, mac_str);

    // Find which registered sensor sent this
    sensor_entry_t *entry = NULL;
    for (int i = 0; i < s_sensor_count; i++) {
        if (memcmp(s_sensors[i].mac, src, 6) == 0) {
            entry = &s_sensors[i];
            break;
        }
    }

    if (pkt->type == PKT_ACK) {
        if (entry == NULL) {
            ESP_LOGW(TAG, "ACK from unknown sensor %s — ignoring", mac_str);
            return;
        }

        ESP_LOGI(TAG, "Got ACK from %s — ESP-NOW link established!", mac_str);
        entry->paired = true;

        display_show("Sensor Paired!", mac_str);

        /* Persist the full sensor MAC list so it survives hub reboots */
        char saved_macs[MAX_SENSORS][18];
        int  saved_count = 0;
        for (int i = 0; i < s_sensor_count; i++) {
            mac_bytes_to_str(s_sensors[i].mac, saved_macs[saved_count++]);
        }
        nvs_save_sensors(saved_macs, saved_count);

        /* Pairing window stays open (2-min timer in button.c closes it).
           This lets the user scan and pair more sensors without re-pressing. */

    } else if (pkt->type == PKT_EVENT) {
        if (entry == NULL) {
            ESP_LOGW(TAG, "Event from unknown sensor %s — ignoring", mac_str);
            return;
        }

        ESP_LOGI(TAG, "Event from %s: %s", mac_str, pkt->payload);
        display_show("SENSOR DATA", pkt->payload);

        /* Enqueue for HTTP forwarding — do NOT block here (we're in the WiFi task) */
        event_item_t item;
        strncpy(item.mac_str, mac_str, sizeof(item.mac_str) - 1);
        strncpy(item.payload, pkt->payload, sizeof(item.payload) - 1);
        item.mac_str[sizeof(item.mac_str) - 1] = '\0';
        item.payload[sizeof(item.payload) - 1]  = '\0';

        if (xQueueSend(s_event_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Event queue full — dropping event from %s", mac_str);
        }
    }
}

// ── Send callback (debug) ─────────────────────────────────────────────────
static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    ESP_LOGI(TAG, "Send to %02X:%02X:%02X:%02X:%02X:%02X: %s",
        mac_addr[0], mac_addr[1], mac_addr[2],
        mac_addr[3], mac_addr[4], mac_addr[5],
        status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
}

// ── HELLO retry task ──────────────────────────────────────────────────────
/*
 * arg = pointer to sensor_entry_t inside s_sensors[] (stable static memory).
 * Embeds the hub's current WiFi channel in the payload so the sensor can
 * lock to exactly the right channel without scanning.
 */
static void hello_retry_task(void *arg)
{
    sensor_entry_t *entry = (sensor_entry_t *)arg;

    uint8_t primary; wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);

    espnow_packet_t pkt = { .type = PKT_HELLO };
    snprintf(pkt.payload, sizeof(pkt.payload), "HELLO_FROM_HUB:ch=%d", primary);

    char mac_str[18];
    mac_bytes_to_str(entry->mac, mac_str);

    for (int i = 0; i < 10; i++) {
        if (entry->paired) break;

        esp_err_t err = esp_now_send(entry->mac, (uint8_t *)&pkt, sizeof(pkt));
        ESP_LOGI(TAG, "HELLO → %s  attempt %d/10: %s (ch%d)",
            mac_str, i + 1, err == ESP_OK ? "sent" : esp_err_to_name(err), primary);

        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    if (!entry->paired) {
        ESP_LOGE(TAG, "No ACK from %s after 10 attempts — pairing failed", mac_str);
        display_show("Pair Failed", "No response");
    }
    vTaskDelete(NULL);
}

// ── Public API ────────────────────────────────────────────────────────────

void espnow_init(void)
{
    s_event_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(event_item_t));
    xTaskCreate(event_forward_task, "evt_fwd", 4096, NULL, 4, NULL);

    ESP_ERROR_CHECK(esp_now_init());
    esp_now_register_recv_cb(espnow_recv_cb);
    esp_now_register_send_cb(espnow_send_cb);

    uint8_t primary; wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);
    ESP_LOGI(TAG, "ESP-NOW ready — hub channel: %d (embedded in HELLO)", primary);
}

void espnow_pair_sensor(const char *sensor_mac_str)
{
    if (s_sensor_count >= MAX_SENSORS) {
        ESP_LOGE(TAG, "MAX_SENSORS (%d) reached — cannot pair more", MAX_SENSORS);
        display_show("Pair Failed", "Max sensors!");
        return;
    }

    ESP_LOGI(TAG, "Pairing sensor #%d: %s", s_sensor_count + 1, sensor_mac_str);
    display_show("Sensor Found!", sensor_mac_str);

    sensor_entry_t *entry = &s_sensors[s_sensor_count];
    mac_str_to_bytes(sensor_mac_str, entry->mac);
    entry->paired = false;
    s_sensor_count++;

    // Add as ESP-NOW peer (guard against duplicate adds)
    if (!esp_now_is_peer_exist(entry->mac)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, entry->mac, 6);
        peer.channel = 0;   /* 0 = follow current channel */
        peer.encrypt = false;

        if (esp_now_add_peer(&peer) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add ESP-NOW peer %s", sensor_mac_str);
            s_sensor_count--;
            return;
        }
    }

    // Fire retry task — passes pointer to this sensor's entry
    xTaskCreate(hello_retry_task, "hello_retry", 3072, entry, 5, NULL);
}

void espnow_reconnect_saved_sensors(void)
{
    char macs[MAX_SENSORS][18];
    int count = nvs_load_sensors(macs, MAX_SENSORS);

    if (count == 0) {
        ESP_LOGI(TAG, "No saved sensors in NVS — nothing to reconnect");
        return;
    }

    ESP_LOGI(TAG, "Reconnecting %d saved sensor(s) from NVS...", count);
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "  → %s", macs[i]);
        espnow_pair_sensor(macs[i]);
    }
}
