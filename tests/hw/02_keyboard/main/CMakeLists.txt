idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
                       REQUIRES hhv2_lcd hhv2_keymap esp_driver_i2c esp_driver_gpio esp_driver_ledc)
