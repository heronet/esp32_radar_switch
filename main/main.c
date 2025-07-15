#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gsheet_client.h"
#include "radar_sensor.h"

static const char* TAG = "RADAR_WATCH";

#define RELAY_CH_1 GPIO_NUM_21
#define RELAY_CH_2 GPIO_NUM_22
#define RADAR_TX GPIO_NUM_16
#define RADAR_RX GPIO_NUM_17

// Configuration - Update these with your credentials
#define WIFI_SSID "CAMPHIGH"
#define WIFI_PASSWORD "samcam69"
#define APPS_SCRIPT_URL                                                        \
  "https://script.google.com/macros/s/"                                        \
  "AKfycbwd8KMu5JVEsqry8rbqsiSqWbO00Sv6HHCZ6Zlpt5JRg5z4vsRBpr2WbvyK6jmqO4szfw" \
  "/exec"

#define WIFI_RECONNECT_INTERVAL_MS 30000  // 30 seconds
#define STATUS_QUEUE_SIZE 10

// Global variables
static gsheet_client_t gsheet_client;
static QueueHandle_t status_queue;
static SemaphoreHandle_t wifi_status_mutex;
static bool wifi_connected = false;

// Status message structure for queue
typedef struct {
  gsheet_status_t status;
  TickType_t timestamp;
} status_message_t;

// System monitoring task function (runs on Core 0)
void system_monitor_task(void* pvParameters) {
  ESP_LOGI(TAG, "System monitor task started on Core %d", xPortGetCoreID());

  while (1) {
    // Get system information
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();

    // Check queue status
    UBaseType_t queue_messages = uxQueueMessagesWaiting(status_queue);
    UBaseType_t queue_spaces = uxQueueSpacesAvailable(status_queue);

    // Check WiFi status
    bool current_wifi_status;
    xSemaphoreTake(wifi_status_mutex, portMAX_DELAY);
    current_wifi_status = wifi_connected;
    xSemaphoreGive(wifi_status_mutex);

    ESP_LOGI(TAG,
             "System Status - Free Heap: %d bytes, Min Free: %d bytes, Queue: "
             "%d/%d, WiFi: %s",
             free_heap, min_free_heap, queue_messages,
             queue_messages + queue_spaces,
             current_wifi_status ? "Connected" : "Disconnected");

    // Monitor every 30 seconds
    vTaskDelay(pdMS_TO_TICKS(30000));
  }
}

// WiFi task function (runs on Core 0)
void wifi_task(void* pvParameters) {
  ESP_LOGI(TAG, "WiFi task started on Core %d", xPortGetCoreID());

  // Initialize Google Sheets client
  gsheet_config_t gsheet_config = {.apps_script_url = APPS_SCRIPT_URL,
                                   .wifi_ssid = WIFI_SSID,
                                   .wifi_password = WIFI_PASSWORD,
                                   .timeout_ms = 10000};

  esp_err_t ret = gsheet_client_init(&gsheet_client, &gsheet_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize Google Sheets client: %s",
             esp_err_to_name(ret));
    vTaskDelete(NULL);
    return;
  }

  // Initial WiFi connection attempt
  ESP_LOGI(TAG, "Attempting initial WiFi connection...");
  ret = gsheet_client_wifi_connect(&gsheet_client);
  if (ret == ESP_OK) {
    xSemaphoreTake(wifi_status_mutex, portMAX_DELAY);
    wifi_connected = true;
    xSemaphoreGive(wifi_status_mutex);
    ESP_LOGI(TAG, "WiFi connected successfully");
  } else {
    ESP_LOGE(TAG,
             "Initial WiFi connection failed, will retry every 30 seconds");
    xSemaphoreTake(wifi_status_mutex, portMAX_DELAY);
    wifi_connected = false;
    xSemaphoreGive(wifi_status_mutex);
  }

  status_message_t status_msg;
  // FIXED: Initialize to invalid state so first message always gets sent
  gsheet_status_t last_sent_status = (gsheet_status_t)-1;  // Invalid state
  TickType_t last_wifi_attempt = xTaskGetTickCount();
  bool wifi_init_done = (ret == ESP_OK);

  while (1) {
    bool current_wifi_status;
    TickType_t current_time = xTaskGetTickCount();

    // Get current WiFi status
    xSemaphoreTake(wifi_status_mutex, portMAX_DELAY);
    current_wifi_status = wifi_connected;
    xSemaphoreGive(wifi_status_mutex);

    // If WiFi is not connected, try to reconnect only every 30 seconds
    if (!current_wifi_status) {
      if (current_time - last_wifi_attempt >=
          pdMS_TO_TICKS(WIFI_RECONNECT_INTERVAL_MS)) {
        ESP_LOGI(TAG, "Attempting WiFi reconnection...");
        ret = gsheet_client_wifi_connect(&gsheet_client);

        xSemaphoreTake(wifi_status_mutex, portMAX_DELAY);
        wifi_connected = (ret == ESP_OK);
        xSemaphoreGive(wifi_status_mutex);

        if (ret == ESP_OK) {
          ESP_LOGI(TAG, "WiFi reconnected successfully");
          wifi_init_done = true;
        } else {
          ESP_LOGW(TAG, "WiFi reconnection failed, will retry in 30 seconds");
        }

        last_wifi_attempt = current_time;
      }

      // Clear the queue if WiFi is not connected to prevent buildup
      status_message_t dummy_msg;
      int dropped_count = 0;
      while (xQueueReceive(status_queue, &dummy_msg, 0) == pdTRUE) {
        dropped_count++;
      }
      if (dropped_count > 0) {
        ESP_LOGI(TAG, "Dropped %d queued messages while WiFi disconnected",
                 dropped_count);
      }

      // Short delay and continue loop
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    // WiFi is connected - process status messages from queue
    if (wifi_init_done && current_wifi_status) {
      // DIAGNOSTIC: Check queue status before processing
      UBaseType_t queue_count = uxQueueMessagesWaiting(status_queue);
      if (queue_count > 0) {
        ESP_LOGI(TAG, "DIAGNOSTIC: Queue has %d messages waiting", queue_count);
      }

      // Process all messages in queue and keep the latest one
      status_message_t latest_msg;
      bool got_message = false;
      int processed_count = 0;

      // Get all messages from queue and keep the latest
      while (xQueueReceive(status_queue, &latest_msg, 0) == pdTRUE) {
        status_msg = latest_msg;  // Keep overwriting with latest
        got_message = true;
        processed_count++;
        ESP_LOGI(TAG,
                 "DIAGNOSTIC: Processed queue message %d: %s (timestamp: %lu)",
                 processed_count,
                 (latest_msg.status == GSHEET_STATUS_ON) ? "ON" : "OFF",
                 latest_msg.timestamp);
      }

      if (got_message) {
        ESP_LOGI(TAG, "DIAGNOSTIC: Will send status %s (last_sent was %s)",
                 (status_msg.status == GSHEET_STATUS_ON) ? "ON" : "OFF",
                 (last_sent_status == GSHEET_STATUS_ON)    ? "ON"
                 : (last_sent_status == GSHEET_STATUS_OFF) ? "OFF"
                                                           : "NONE");

        // FIXED: Send if status has changed OR if this is the first message
        if (status_msg.status != last_sent_status ||
            last_sent_status == (gsheet_status_t)-1) {
          ESP_LOGI(TAG, "Sending status to Google Sheets: %s",
                   (status_msg.status == GSHEET_STATUS_ON) ? "ON" : "OFF");

          TickType_t send_start = xTaskGetTickCount();
          ret = gsheet_client_send_status(&gsheet_client, status_msg.status);
          TickType_t send_end = xTaskGetTickCount();

          ESP_LOGI(TAG, "DIAGNOSTIC: HTTP request took %lu ms",
                   (send_end - send_start) * portTICK_PERIOD_MS);

          if (ret == ESP_OK) {
            last_sent_status = status_msg.status;
            ESP_LOGI(TAG, "Status updated in Google Sheets successfully");
          } else {
            ESP_LOGW(TAG, "Failed to send status to Google Sheets: %s",
                     esp_err_to_name(ret));

            // Check if this is a connection issue
            if (ret == ESP_ERR_HTTP_CONNECT || ret == ESP_ERR_TIMEOUT) {
              ESP_LOGW(
                  TAG,
                  "Connection issue detected, marking WiFi as disconnected");
              xSemaphoreTake(wifi_status_mutex, portMAX_DELAY);
              wifi_connected = false;
              xSemaphoreGive(wifi_status_mutex);
              last_wifi_attempt = xTaskGetTickCount() -
                                  pdMS_TO_TICKS(WIFI_RECONNECT_INTERVAL_MS);
            }
          }
        } else {
          ESP_LOGI(TAG, "DIAGNOSTIC: Status unchanged (%s), skipping send",
                   (status_msg.status == GSHEET_STATUS_ON) ? "ON" : "OFF");
        }
      }
    }

    // Check for queued messages more frequently
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Sensor task function (runs on Core 1)
void sensor_task(void* pvParameters) {
  ESP_LOGI(TAG, "Sensor task started on Core %d", xPortGetCoreID());

  radar_sensor_t radar_sensor;
  gsheet_status_t last_status = GSHEET_STATUS_OFF;

  // Initialize radar sensor
  esp_err_t ret =
      radar_sensor_init(&radar_sensor, UART_NUM_1, RADAR_TX, RADAR_RX);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize radar sensor: %s",
             esp_err_to_name(ret));
    vTaskDelete(NULL);
    return;
  }

  ret = radar_sensor_begin(&radar_sensor, 256000);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to begin radar sensor communication: %s",
             esp_err_to_name(ret));
    radar_sensor_deinit(&radar_sensor);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "Radar sensor initialized successfully");

  // Initialize GPIO for relays
  gpio_set_direction(RELAY_CH_1, GPIO_MODE_OUTPUT);
  gpio_set_direction(RELAY_CH_2, GPIO_MODE_OUTPUT);
  gpio_set_level(RELAY_CH_1, 1);  // Initially OFF (active low)
  gpio_set_level(RELAY_CH_2, 1);  // Initially OFF (active low)

  ESP_LOGI(TAG,
           "Sensor task ready - relays will switch regardless of WiFi status");

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

        // Turn relays ON (active low) - THIS HAPPENS REGARDLESS OF WiFi STATUS
        gpio_set_level(RELAY_CH_1, 0);
        gpio_set_level(RELAY_CH_2, 0);
        current_status = GSHEET_STATUS_ON;
      } else {
        ESP_LOGI(TAG, "No target detected");

        // Turn relays OFF (active low) - THIS HAPPENS REGARDLESS OF WiFi STATUS
        gpio_set_level(RELAY_CH_1, 1);
        gpio_set_level(RELAY_CH_2, 1);
        current_status = GSHEET_STATUS_OFF;
      }
    }

    // Queue status for Google Sheets only if status changed
    if (current_status != last_status) {
      status_message_t status_msg = {.status = current_status,
                                     .timestamp = xTaskGetTickCount()};

      // Try to send to queue (non-blocking)
      if (xQueueSend(status_queue, &status_msg, 0) == pdTRUE) {
        ESP_LOGI(TAG, "Status queued for upload: %s (relays already switched)",
                 (current_status == GSHEET_STATUS_ON) ? "ON" : "OFF");
      } else {
        ESP_LOGW(TAG,
                 "Status queue full, dropping message (relays still switched)");
      }

      last_status = current_status;
    }

    // Run sensor task at 1Hz
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // Cleanup (won't be reached in this example)
  radar_sensor_deinit(&radar_sensor);
}

void app_main(void) {
  ESP_LOGI(TAG, "Application starting...");
  ESP_LOGI(TAG, "System will work as follows:");
  ESP_LOGI(TAG,
           "1. Sensor task (Core 1) - Always switches relays based on radar "
           "detection");
  ESP_LOGI(TAG,
           "2. WiFi task (Core 0) - Connects to WiFi and sends data to Google "
           "Sheets");
  ESP_LOGI(TAG,
           "3. Relays work immediately, Google Sheets updates only when WiFi "
           "is connected");
  ESP_LOGI(TAG,
           "4. WiFi reconnection attempts every 30 seconds if disconnected");

  // Create queue for status messages
  status_queue = xQueueCreate(STATUS_QUEUE_SIZE, sizeof(status_message_t));
  if (status_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create status queue");
    return;
  }

  // Create mutex for WiFi status
  wifi_status_mutex = xSemaphoreCreateMutex();
  if (wifi_status_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create WiFi status mutex");
    vQueueDelete(status_queue);
    return;
  }

  // Create system monitor task on Core 0 (monitors system health)
  BaseType_t monitor_task_created =
      xTaskCreatePinnedToCore(system_monitor_task, "system_monitor", 2048, NULL,
                              1,  // Low priority for monitoring
                              NULL,
                              0  // Pin to Core 0
      );

  if (monitor_task_created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create system monitor task");
    vQueueDelete(status_queue);
    vSemaphoreDelete(wifi_status_mutex);
    return;
  }

  // Create WiFi task on Core 0 (handles network operations)
  BaseType_t wifi_task_created =
      xTaskCreatePinnedToCore(wifi_task, "wifi_task", 4096, NULL,
                              5,  // Higher priority for WiFi task
                              NULL,
                              0  // Pin to Core 0
      );

  if (wifi_task_created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create WiFi task");
    vQueueDelete(status_queue);
    vSemaphoreDelete(wifi_status_mutex);
    return;
  }

  // Create sensor task on Core 1 (handles real-time sensor operations)
  BaseType_t sensor_task_created =
      xTaskCreatePinnedToCore(sensor_task, "sensor_task", 4096, NULL,
                              4,  // High priority for sensor task (real-time)
                              NULL,
                              1  // Pin to Core 1
      );

  if (sensor_task_created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create sensor task");
    vQueueDelete(status_queue);
    vSemaphoreDelete(wifi_status_mutex);
    return;
  }

  ESP_LOGI(TAG, "All tasks created successfully");
  ESP_LOGI(TAG, "Core 0: WiFi task + System monitor task");
  ESP_LOGI(TAG, "Core 1: Sensor task (real-time relay control)");

  // app_main runs on Core 0, so we can add a simple alive indicator
  while (1) {
    ESP_LOGI(TAG, "Main task alive on Core %d", xPortGetCoreID());
    vTaskDelay(pdMS_TO_TICKS(60000));  // Log every minute
  }
}