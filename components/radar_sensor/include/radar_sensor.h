#ifndef RADAR_SENSOR_H
#define RADAR_SENSOR_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RADAR_BUFFER_SIZE 30
#define RADAR_FRAME_SIZE 24
#define RADAR_FULL_FRAME_SIZE 26

// Default retention times in milliseconds
#define RADAR_DEFAULT_DETECTION_RETENTION_MS 3000  // 3 seconds
#define RADAR_DEFAULT_ABSENCE_RETENTION_MS 1000    // 1 second

typedef struct {
  bool detected;
  float x;
  float y;
  float speed;
  float distance;
  float angle;
} radar_target_t;

typedef enum {
  WAIT_AA,
  WAIT_FF,
  WAIT_03,
  WAIT_00,
  RECEIVE_FRAME
} radar_parser_state_t;

typedef struct {
  uint32_t detection_retention_ms;  // How long to keep "detected" after losing
                                    // target
  uint32_t absence_retention_ms;    // How long to wait before confirming "not
                                    // detected"
  TickType_t last_detection_time;   // Last time target was actually seen
  TickType_t last_absence_time;     // Last time no target was seen
  bool raw_detected;                // Current raw sensor reading
  bool filtered_detected;           // Filtered output state
  bool retention_enabled;           // Enable/disable retention filtering
} radar_retention_t;

typedef struct {
  uart_port_t uart_port;
  gpio_num_t rx_pin;
  gpio_num_t tx_pin;
  radar_target_t target;
  radar_target_t raw_target;  // Raw unfiltered target data
  uint8_t buffer[RADAR_BUFFER_SIZE];
  size_t buffer_index;
  radar_parser_state_t parser_state;
  radar_retention_t retention;  // Target retention management
} radar_sensor_t;

// Function prototypes
esp_err_t radar_sensor_init(radar_sensor_t* sensor,
                            uart_port_t uart_port,
                            gpio_num_t rx_pin,
                            gpio_num_t tx_pin);
esp_err_t radar_sensor_begin(radar_sensor_t* sensor, uint32_t baud_rate);
bool radar_sensor_update(radar_sensor_t* sensor);
bool radar_sensor_parse_data(radar_sensor_t* sensor,
                             const uint8_t* buf,
                             size_t len);
radar_target_t radar_sensor_get_target(radar_sensor_t* sensor);
radar_target_t radar_sensor_get_raw_target(radar_sensor_t* sensor);
void radar_sensor_deinit(radar_sensor_t* sensor);

// Target retention configuration functions
void radar_sensor_set_retention_times(radar_sensor_t* sensor,
                                      uint32_t detection_retention_ms,
                                      uint32_t absence_retention_ms);
void radar_sensor_enable_retention(radar_sensor_t* sensor, bool enable);
void radar_sensor_reset_retention(radar_sensor_t* sensor);

// Diagnostic functions
bool radar_sensor_is_retention_active(radar_sensor_t* sensor);
uint32_t radar_sensor_get_time_since_last_detection(radar_sensor_t* sensor);
uint32_t radar_sensor_get_time_since_last_absence(radar_sensor_t* sensor);

#endif  // RADAR_SENSOR_H