#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gsheet_client.h"
#include "radar_sensor.h"

static const char* TAG = "RADAR_WATCH";

#define RELAY_CH_1 GPIO_NUM_9
#define RELAY_CH_2 GPIO_NUM_10
#define RADAR_TX GPIO_NUM_2
#define RADAR_RX GPIO_NUM_3

// Configuration - Update these with your credentials
#define WIFI_SSID "RS connection"
#define WIFI_PASSWORD "301285211285"
#define APPS_SCRIPT_URL                                                        \
  "https://script.google.com/macros/s/"                                        \
  "AKfycbwd8KMu5JVEsqry8rbqsiSqWbO00Sv6HHCZ6Zlpt5JRg5z4vsRBpr2WbvyK6jmqO4szfw" \
  "/exec"

static gsheet_client_t gsheet_client;
static gsheet_status_t last_status = GSHEET_STATUS_OFF;

void app_main(void) {
  radar_sensor_t radar_sensor;

  // Initialize radar sensor
  esp_err_t ret =
      radar_sensor_init(&radar_sensor, UART_NUM_1, RADAR_TX, RADAR_RX);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize radar sensor: %s",
             esp_err_to_name(ret));
    return;
  }

  ret = radar_sensor_begin(&radar_sensor, 256000);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to begin radar sensor communication: %s",
             esp_err_to_name(ret));
    radar_sensor_deinit(&radar_sensor);
    return;
  }

  ESP_LOGI(TAG, "Radar sensor initialized successfully");

  // Initialize GPIO for relays
  gpio_set_direction(RELAY_CH_1, GPIO_MODE_OUTPUT);
  gpio_set_direction(RELAY_CH_2, GPIO_MODE_OUTPUT);
  gpio_set_level(RELAY_CH_1, 1);  // Initially OFF
  gpio_set_level(RELAY_CH_2, 1);  // Initially OFF

  // Initialize Google Sheets client
  gsheet_config_t gsheet_config = {.apps_script_url = APPS_SCRIPT_URL,
                                   .wifi_ssid = WIFI_SSID,
                                   .wifi_password = WIFI_PASSWORD,
                                   .timeout_ms = 10000};

  ret = gsheet_client_init(&gsheet_client, &gsheet_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize Google Sheets client: %s",
             esp_err_to_name(ret));
  } else {
    // Connect to WiFi
    ret = gsheet_client_wifi_connect(&gsheet_client);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to connect to WiFi");
    } else {
      ESP_LOGI(TAG, "WiFi connected successfully");
    }
  }

  while (1) {
    gsheet_status_t current_status = GSHEET_STATUS_OFF;

    // Update radar sensor
    if (radar_sensor_update(&radar_sensor)) {
      radar_target_t target = radar_sensor_get_target(&radar_sensor);

      if (target.detected) {
        ESP_LOGI(TAG,
                 "Target detected - X: %.2f mm, Y: %.2f mm, Speed: %.2f cm/s, "
                 "Distance: %.2f mm, Angle: %.2f°",
                 target.x, target.y, target.speed, target.distance,
                 target.angle);

        // Turn relays ON (active low)
        gpio_set_level(RELAY_CH_1, 0);
        gpio_set_level(RELAY_CH_2, 0);
        current_status = GSHEET_STATUS_ON;
      } else {
        ESP_LOGI(TAG, "No target detected");

        // Turn relays OFF (active low)
        gpio_set_level(RELAY_CH_1, 1);
        gpio_set_level(RELAY_CH_2, 1);
        current_status = GSHEET_STATUS_OFF;
      }
    }

    // Send status to Google Sheets only if status changed
    if (current_status != last_status &&
        gsheet_client_is_wifi_connected(&gsheet_client)) {
      ret = gsheet_client_send_status(&gsheet_client, current_status);
      if (ret == ESP_OK) {
        last_status = current_status;
        ESP_LOGI(TAG, "Status updated in Google Sheets: %s",
                 (current_status == GSHEET_STATUS_ON) ? "ON" : "OFF");
      } else {
        ESP_LOGW(TAG, "Failed to send status to Google Sheets: %s",
                 esp_err_to_name(ret));
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // Cleanup (won't be reached in this example)
  radar_sensor_deinit(&radar_sensor);
  gsheet_client_deinit(&gsheet_client);
}