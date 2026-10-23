FILE(GLOB app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc esp_driver_uart esp_driver_i2c esp_driver_rmt esp_adc nvs_flash esp_timer freertos driver esp_wifi esp_netif esp_event esp_http_server bootloader_support)
