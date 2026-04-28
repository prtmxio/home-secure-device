idf_component_register(
    SRCS
        "tft_test.c"
        "ui/screens.c"
        "ui/styles.c"
        "ui/images.c"
        "ui/ui.c"
        "ui/ui_image_glazia_logo.c"
        "ui/eez-flow.cpp"
    INCLUDE_DIRS "." "ui"
    REQUIRES esp_lcd driver esp_timer esp_lcd_touch_xpt2046
)
