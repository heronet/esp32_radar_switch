# Radar Sensor Component CMakeLists.txt

idf_component_register(
    SRCS "radar_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        driver
        esp_common
        log
)