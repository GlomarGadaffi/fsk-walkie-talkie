idf_component_register(
    SRCS
        "app_main.cpp"
        "audio_io.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_spi
        esp_driver_i2s
        esp_timer
        nvs_flash
        fsk_link
        codec2
)
