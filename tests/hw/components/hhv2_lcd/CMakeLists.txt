idf_component_register(SRCS "hhv2_lcd.c" "font5x7.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd esp_driver_spi esp_driver_gpio)
