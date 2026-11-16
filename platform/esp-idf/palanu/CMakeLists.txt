# Component "palanu": the aggregator the target depends on, and the platform glue (ESP-IDF port, board hhv2).
# It REQUIRES nema, system and apps, so targets/hhv2/main needs no other REQUIRES. nema, system and apps never REQUIRE
# palanu: the glue implements the nema_port_* and board functions they call, and those calls resolve at link time.
get_filename_component(PAL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

file(GLOB PORT_SRC ${CMAKE_CURRENT_LIST_DIR}/*.c)
file(GLOB BOARD_SRC ${PAL_ROOT}/boards/hhv2/*.c)

idf_component_register(
    SRCS ${PORT_SRC} ${BOARD_SRC}
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR} ${PAL_ROOT}/boards/hhv2
    REQUIRES nema system apps
             esp_timer esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_uart esp_lcd esp_driver_rmt esp_adc app_update
             esp_psram freertos heap nvs_flash esp_wifi esp_netif esp_event lwip bt
             fatfs sdmmc esp_driver_sdspi
    PRIV_REQUIRES esp_hw_support esp_system
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter -fstack-usage) # stack frames checked by forge/tools/ramcheck.ts
