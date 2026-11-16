idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
                       REQUIRES hhv2_lcd esp_lcd esp_driver_spi esp_driver_gpio esp_timer esp_mm esp_psram)
