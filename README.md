# Radar Sensor Auto Fan/Light Control

An ESP32-based presence detection system that automatically turns on/off fans and lights using a radar sensor and relay module. The system detects human presence and movement, providing hands-free automation for your home or office.

## Features

- **Presence Detection**: Uses radar sensor for accurate human presence detection
- **Automatic Control**: Turns fan/light on when presence is detected, off when no one is present
- **Real-time Monitoring**: Provides detailed target information including position, speed, and distance
- **UART Communication**: High-speed communication at 256kbps for reliable data transfer
- **GPIO Control**: Simple relay control via ESP32 GPIO pins
- **Non-contact Detection**: Works through walls, clothing, and in various lighting conditions

## Hardware Requirements

### Components

- ESP32 development board
- Radar sensor module (compatible with the protocol used in this code)
- Relay module (5V or 3.3V compatible)
- Fan and/or light fixtures
- Jumper wires
- Power supply (12V for fan, appropriate voltage for lights)
- Breadboard or PCB for connections

### Pin Connections

| Component     | ESP32 Pin | Description             |
| ------------- | --------- | ----------------------- |
| Radar RX      | GPIO 6    | UART receive from radar |
| Radar TX      | GPIO 5    | UART transmit to radar  |
| Relay Control | GPIO 1    | Digital output to relay |
| Radar VCC     | 3.3V/5V   | Power supply            |
| Radar GND     | GND       | Ground connection       |
| Relay VCC     | 5V        | Relay power supply      |
| Relay GND     | GND       | Ground connection       |

## Software Requirements

- ESP-IDF framework
- FreeRTOS (included with ESP-IDF)
- Standard ESP32 libraries

## Installation & Setup

### 1. Clone or Download the Project

```bash
git clone <repository-url>
cd radar-sensor-control
```

### 2. Configure ESP-IDF Environment

```bash
# Set up ESP-IDF environment
. $HOME/esp/esp-idf/export.sh
```

### 3. Build and Flash

```bash
# Configure the project
idf.py menuconfig

# Build the project
idf.py build

# Flash to ESP32
idf.py -p /dev/ttyUSB0 flash monitor
```

## Code Structure

### Files

- `main.c` - Main application logic and initialization
- `radar_sensor.h` - Header file with radar sensor definitions
- `radar_sensor.c` - Radar sensor driver implementation (provided in paste.txt)

### Key Functions

#### Radar Sensor Driver (`radar_sensor.c`)

- `radar_sensor_init()` - Initialize radar sensor structure and pins
- `radar_sensor_begin()` - Configure UART communication
- `radar_sensor_update()` - Parse incoming radar data
- `radar_sensor_parse_data()` - Extract target information from raw data
- `radar_sensor_get_target()` - Get current target data
- `radar_sensor_deinit()` - Cleanup resources

#### Main Application (`main.c`)

- Initialize radar sensor on UART1
- Continuously monitor for presence
- Control relay based on detection status
- Log target information for debugging

## Configuration

### UART Settings

- **Baud Rate**: 256,000 bps
- **Data Bits**: 8
- **Parity**: None
- **Stop Bits**: 1
- **Flow Control**: None

### Radar Data Protocol

The radar sensor uses a specific frame format:

- **Header**: `0xAA 0xFF 0x03 0x00`
- **Data Frame**: 24 bytes of target information
- **Tail**: `0x55 0xCC`

### Target Information

- **Position**: X, Y coordinates in millimeters
- **Speed**: Movement speed in cm/s
- **Distance**: Calculated distance from sensor
- **Angle**: Direction of target relative to sensor

## Usage

1. **Power On**: Connect power to ESP32 and radar sensor
2. **Automatic Detection**: The system continuously monitors for presence
3. **Fan/Light Control**:
   - **Presence Detected**: Relay activates (GPIO 1 HIGH), turning on fan/light
   - **No Presence**: Relay deactivates (GPIO 1 LOW), turning off fan/light
4. **Monitor Logs**: Use serial monitor to view detection status and target data

### Serial Output Example

```
I (1234) RADAR_WATCH: Radar sensor initialized successfully
I (1456) RADAR_WATCH: Target detected - X: 850.00 mm, Y: 1200.00 mm, Speed: 15.50 cm/s, Distance: 1478.32 mm, Angle: 35.23°
I (1789) RADAR_WATCH: No target detected
```

## Customization

### Adjust Detection Sensitivity

Modify the detection logic in `radar_sensor_parse_data()` to change sensitivity thresholds.

### Change Update Frequency

Adjust the delay in the main loop:

```c
vTaskDelay(pdMS_TO_TICKS(100)); // 100ms delay
```

### Multiple Relays

Add more GPIO pins for controlling multiple devices:

```c
gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT); // Second relay
gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT); // Third relay
```

## Troubleshooting

### Common Issues

1. **No Radar Data Received**

   - Check UART pin connections (RX/TX may be swapped)
   - Verify baud rate setting (256000)
   - Ensure radar sensor is powered correctly

2. **Relay Not Switching**

   - Check relay module power supply
   - Verify GPIO pin connection
   - Test relay module independently

3. **False Detections**

   - Adjust radar sensor position
   - Modify detection thresholds in code
   - Check for electromagnetic interference

4. **Compilation Errors**
   - Ensure all header files are included
   - Check ESP-IDF version compatibility
   - Verify component configuration

### Debug Tips

- Use `ESP_LOGI()` statements for debugging
- Monitor serial output for radar data
- Test relay independently with simple GPIO toggle
- Verify UART communication with oscilloscope if available

## Safety Considerations

- Ensure proper electrical isolation between low-voltage control and high-voltage loads
- Use appropriate fuses and circuit breakers
- Follow local electrical codes and regulations
- Consider using optically isolated relays for better safety
- Test thoroughly before permanent installation

## Future Enhancements

- Add web interface for remote monitoring
- Implement presence timeout delays
- Add motion-based sensitivity adjustment
- Include multiple detection zones
- Add smartphone app integration
- Implement energy usage monitoring

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Support

For issues and questions:

- Check the troubleshooting section
- Review ESP-IDF documentation
- Create an issue in the project repository
