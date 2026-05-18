idf_component_register(
    SRCS "t1s_lan8651_udp_measurement.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_eth
        esp_netif
        esp_event
        esp_timer
        esp_driver_gpio
        esp_driver_spi
        esp_rom
        lwip
        espressif__lan865x
)
