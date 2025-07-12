# CMakeLists.txt for gsheet_client component
idf_component_register(
    SRCS "gsheet_client.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_client esp-tls mbedtls
)