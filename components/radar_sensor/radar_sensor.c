#include "radar_sensor.h"
#include "esp_log.h"

static const char *TAG = "RADAR_SENSOR";

esp_err_t radar_sensor_init(radar_sensor_t *sensor, uart_port_t uart_port,
                            gpio_num_t rx_pin, gpio_num_t tx_pin)
{
    if (!sensor)
    {
        return ESP_ERR_INVALID_ARG;
    }

    sensor->uart_port = uart_port;
    sensor->rx_pin = rx_pin;
    sensor->tx_pin = tx_pin;
    sensor->buffer_index = 0;
    sensor->parser_state = WAIT_AA;

    // Initialize target structure
    sensor->target.detected = false;
    sensor->target.x = 0.0f;
    sensor->target.y = 0.0f;
    sensor->target.speed = 0.0f;
    sensor->target.distance = 0.0f;
    sensor->target.angle = 0.0f;

    return ESP_OK;
}

esp_err_t radar_sensor_begin(radar_sensor_t *sensor, uint32_t baud_rate)
{
    if (!sensor)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    esp_err_t ret = uart_param_config(sensor->uart_port, &uart_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure UART parameters");
        return ret;
    }

    ret = uart_set_pin(sensor->uart_port, sensor->tx_pin, sensor->rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set UART pins");
        return ret;
    }

    ret = uart_driver_install(sensor->uart_port, 1024, 1024, 0, NULL, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install UART driver");
        return ret;
    }

    return ESP_OK;
}

bool radar_sensor_update(radar_sensor_t *sensor)
{
    if (!sensor)
    {
        return false;
    }

    bool data_updated = false;
    uint8_t byte_in;

    while (uart_read_bytes(sensor->uart_port, &byte_in, 1, 0) > 0)
    {
        switch (sensor->parser_state)
        {
        case WAIT_AA:
            if (byte_in == 0xAA)
            {
                sensor->parser_state = WAIT_FF;
            }
            break;

        case WAIT_FF:
            if (byte_in == 0xFF)
            {
                sensor->parser_state = WAIT_03;
            }
            else
            {
                sensor->parser_state = WAIT_AA;
            }
            break;

        case WAIT_03:
            if (byte_in == 0x03)
            {
                sensor->parser_state = WAIT_00;
            }
            else
            {
                sensor->parser_state = WAIT_AA;
            }
            break;

        case WAIT_00:
            if (byte_in == 0x00)
            {
                sensor->buffer_index = 0;
                sensor->parser_state = RECEIVE_FRAME;
            }
            else
            {
                sensor->parser_state = WAIT_AA;
            }
            break;

        case RECEIVE_FRAME:
            sensor->buffer[sensor->buffer_index++] = byte_in;
            if (sensor->buffer_index >= RADAR_FULL_FRAME_SIZE)
            {
                // Check tail bytes
                if (sensor->buffer[24] == 0x55 && sensor->buffer[25] == 0xCC)
                {
                    data_updated = radar_sensor_parse_data(sensor, sensor->buffer, RADAR_FRAME_SIZE);
                }
                sensor->parser_state = WAIT_AA;
                sensor->buffer_index = 0;
            }
            break;
        }
    }

    return data_updated;
}

bool radar_sensor_parse_data(radar_sensor_t *sensor, const uint8_t *buf, size_t len)
{
    if (!sensor || !buf || len != RADAR_FRAME_SIZE)
    {
        return false;
    }

    // Parse first 8 bytes for the first target
    int16_t raw_x = buf[0] | (buf[1] << 8);
    int16_t raw_y = buf[2] | (buf[3] << 8);
    int16_t raw_speed = buf[4] | (buf[5] << 8);
    uint16_t raw_pixel_dist = buf[6] | (buf[7] << 8);

    sensor->target.detected = !(raw_x == 0 && raw_y == 0 && raw_speed == 0 && raw_pixel_dist == 0);

    // Parse signed values (fix the sign bit logic from original)
    sensor->target.x = (raw_x & 0x8000) ? -(raw_x & 0x7FFF) : (raw_x & 0x7FFF);
    sensor->target.y = (raw_y & 0x8000) ? -(raw_y & 0x7FFF) : (raw_y & 0x7FFF);
    sensor->target.speed = (raw_speed & 0x8000) ? -(raw_speed & 0x7FFF) : (raw_speed & 0x7FFF);

    if (sensor->target.detected)
    {
        sensor->target.distance = sqrtf(sensor->target.x * sensor->target.x +
                                        sensor->target.y * sensor->target.y);

        // Angle calculation (convert radians to degrees, then flip)
        float angle_rad = atan2f(sensor->target.y, sensor->target.x) - (M_PI / 2.0f);
        float angle_deg = angle_rad * (180.0f / M_PI);
        sensor->target.angle = -angle_deg; // align angle with x measurement positive/negative sign
    }
    else
    {
        sensor->target.distance = 0.0f;
        sensor->target.angle = 0.0f;
    }

    return true;
}

radar_target_t radar_sensor_get_target(radar_sensor_t *sensor)
{
    if (!sensor)
    {
        radar_target_t empty_target = {0};
        return empty_target;
    }

    return sensor->target;
}

void radar_sensor_deinit(radar_sensor_t *sensor)
{
    if (sensor)
    {
        uart_driver_delete(sensor->uart_port);
    }
}