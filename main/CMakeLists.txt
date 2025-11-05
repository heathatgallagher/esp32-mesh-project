idf_component_register(SRCS "hello_world_main.c"
                       PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server driver mdns
                       INCLUDE_DIRS "")
