set(UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../ui")

idf_component_register(
    SRCS
        "tft_test.c"
        "${UI_DIR}/ui_image_glazia_logo.c"
    INCLUDE_DIRS "." "${UI_DIR}"
    REQUIRES esp_lcd driver esp_timer
)
