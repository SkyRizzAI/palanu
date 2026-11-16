idf_component_register(SRCS "main.c" "kb.c" "ui.c" INCLUDE_DIRS "."
                       REQUIRES hhv2_keymap esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c
                                esp_driver_ledc esp_timer esp_psram)
