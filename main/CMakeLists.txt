idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        driver
        esp_common
        freertos
        radar_sensor
        gsheet_client
)