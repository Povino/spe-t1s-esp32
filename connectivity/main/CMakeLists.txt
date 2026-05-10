idf_component_register(
    SRCS "t1s_lan8651_connectivity.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_eth
        esp_netif
        esp_event
        esp_driver_gpio
        esp_driver_spi
        lwip
        espressif__lan865x
)