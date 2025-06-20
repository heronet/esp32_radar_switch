#ifndef RADAR_SENSOR_H
#define RADAR_SENSOR_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "driver/uart.h"
#include "driver/gpio.h"

#define RADAR_BUFFER_SIZE 30
#define RADAR_FRAME_SIZE 24
#define RADAR_FULL_FRAME_SIZE 26

typedef struct
{
    bool detected;
    float x;
    float y;
    float speed;
    float distance;
    float angle;
} radar_target_t;

typedef enum
{
    WAIT_AA,
    WAIT_FF,
    WAIT_03,
    WAIT_00,
    RECEIVE_FRAME
} radar_parser_state_t;

typedef struct
{
    uart_port_t uart_port;
    gpio_num_t rx_pin;
    gpio_num_t tx_pin;
    radar_target_t target;
    uint8_t buffer[RADAR_BUFFER_SIZE];
    size_t buffer_index;
    radar_parser_state_t parser_state;
} radar_sensor_t;

// Function prototypes
esp_err_t radar_sensor_init(radar_sensor_t *sensor, uart_port_t uart_port,
                            gpio_num_t rx_pin, gpio_num_t tx_pin);
esp_err_t radar_sensor_begin(radar_sensor_t *sensor, uint32_t baud_rate);
bool radar_sensor_update(radar_sensor_t *sensor);
bool radar_sensor_parse_data(radar_sensor_t *sensor, const uint8_t *buf, size_t len);
radar_target_t radar_sensor_get_target(radar_sensor_t *sensor);
void radar_sensor_deinit(radar_sensor_t *sensor);

#endif // RADAR_SENSOR_H