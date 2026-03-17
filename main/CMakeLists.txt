set(requires esp_driver_gpio nvs_flash spiffs esp_netif esp_event esp_eth esp_wifi esp_http_server esp_timer)

idf_component_register(SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${requires})

    spiffs_create_partition_image(storage ../data FLASH_IN_PROJECT)