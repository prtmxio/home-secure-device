file(GLOB UI_SRCS "ui/*.c")

idf_component_register(
    SRCS
        "main.c"
        "button.c"
        "ble.c"
        "wifi.c"
        "api_client.c"
        "espnow.c"
        "display.c"
        "fingerprint.c"
        "nvs_storage.c"
        ${UI_SRCS}
    INCLUDE_DIRS "." "ui"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        bt
        json
        esp_http_client
        driver
        spi_flash
        log
        esp_lcd
        esp_timer
)
