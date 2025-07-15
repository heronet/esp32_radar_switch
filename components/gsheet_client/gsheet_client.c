#include "gsheet_client.h"
#include <stdio.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char* TAG = "GSHEET_CLIENT";

/* WiFi event group */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;
static const int WIFI_MAXIMUM_RETRY = 3;  // Reduced retry count
static bool s_wifi_initialized = false;

static void wifi_event_handler(void* arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < WIFI_MAXIMUM_RETRY) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "Retry to connect to the AP (%d/%d)", s_retry_num,
               WIFI_MAXIMUM_RETRY);
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    ESP_LOGI(TAG, "Connect to the AP failed");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
    ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

esp_err_t gsheet_client_init(gsheet_client_t* client,
                             const gsheet_config_t* config) {
  if (!client || !config) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(client, 0, sizeof(gsheet_client_t));

  // Copy configuration
  client->config.apps_script_url = strdup(config->apps_script_url);
  client->config.wifi_ssid = strdup(config->wifi_ssid);
  client->config.wifi_password = strdup(config->wifi_password);
  client->config.timeout_ms =
      config->timeout_ms > 0 ? config->timeout_ms : 10000;

  if (!client->config.apps_script_url || !client->config.wifi_ssid ||
      !client->config.wifi_password) {
    ESP_LOGE(TAG, "Failed to allocate memory for configuration");
    gsheet_client_deinit(client);
    return ESP_ERR_NO_MEM;
  }

  // Initialize NVS (only once)
  if (!s_wifi_initialized) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_initialized = true;
  }

  ESP_LOGI(TAG, "Google Sheets client initialized");
  return ESP_OK;
}

esp_err_t gsheet_client_wifi_connect(gsheet_client_t* client) {
  if (!client) {
    return ESP_ERR_INVALID_ARG;
  }

  // Reset retry counter
  s_retry_num = 0;

  // Create or recreate event group
  if (s_wifi_event_group) {
    vEventGroupDelete(s_wifi_event_group);
  }
  s_wifi_event_group = xEventGroupCreate();

  // Register event handlers
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

  wifi_config_t wifi_config = {
      .sta =
          {
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
              .pmf_cfg = {.capable = true, .required = false},
          },
  };

  strcpy((char*)wifi_config.sta.ssid, client->config.wifi_ssid);
  strcpy((char*)wifi_config.sta.password, client->config.wifi_password);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "WiFi init finished. Connecting to %s...",
           client->config.wifi_ssid);

  /* Wait until either the connection is established (WIFI_CONNECTED_BIT) or
   * connection failed */
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
      pdMS_TO_TICKS(15000));  // Reduced timeout to 15 seconds

  esp_err_t result = ESP_FAIL;

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to AP SSID:%s", client->config.wifi_ssid);
    client->wifi_connected = true;
    result = ESP_OK;
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(TAG, "Failed to connect to SSID:%s", client->config.wifi_ssid);
    client->wifi_connected = false;
    result = ESP_FAIL;
  } else {
    ESP_LOGE(TAG, "WiFi connection timeout");
    client->wifi_connected = false;
    result = ESP_ERR_TIMEOUT;
  }

  // Unregister event handlers
  esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler);
  esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler);

  return result;
}

// Add this improved function to your gsheet_client.c file (replace the existing
// one)
esp_err_t gsheet_client_send_status(gsheet_client_t* client,
                                    gsheet_status_t status) {
  if (!client) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!client->wifi_connected) {
    ESP_LOGE(TAG, "WiFi not connected");
    return ESP_ERR_INVALID_STATE;
  }

  // Create HTTP client configuration
  esp_http_client_config_t config = {
      .url = client->config.apps_script_url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = client->config.timeout_ms,
      .skip_cert_common_name_check = true,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 4096,
      .buffer_size_tx = 4096,
  };

  client->http_client = esp_http_client_init(&config);
  if (!client->http_client) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    return ESP_FAIL;
  }

  // Set headers
  esp_http_client_set_header(client->http_client, "Content-Type",
                             "application/x-www-form-urlencoded");
  esp_http_client_set_header(client->http_client, "User-Agent",
                             "ESP32-RadarWatch/1.0");
  esp_http_client_set_header(client->http_client, "Accept", "*/*");
  esp_http_client_set_header(client->http_client, "Cache-Control", "no-cache");

  // Create POST data
  char post_data[64];
  const char* status_str = (status == GSHEET_STATUS_ON) ? "ON" : "OFF";
  snprintf(post_data, sizeof(post_data), "status=%s", status_str);

  // Set POST data
  esp_http_client_set_post_field(client->http_client, post_data,
                                 strlen(post_data));

  ESP_LOGI(TAG, "Sending HTTP POST to: %s", client->config.apps_script_url);
  ESP_LOGI(TAG, "POST data: %s", post_data);

  // Perform HTTP request
  esp_err_t err = esp_http_client_perform(client->http_client);

  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client->http_client);
    int content_length =
        esp_http_client_get_content_length(client->http_client);

    ESP_LOGI(TAG, "HTTP POST Status = %d, Content-Length = %d", status_code,
             content_length);

    if (status_code == 200) {
      // Read response body for debugging
      char response_buffer[512];
      int data_read = esp_http_client_read_response(
          client->http_client, response_buffer, sizeof(response_buffer) - 1);
      if (data_read > 0) {
        response_buffer[data_read] = '\0';
        ESP_LOGI(TAG, "Response: %s", response_buffer);
      }

      ESP_LOGI(TAG, "Status '%s' sent successfully", status_str);
    } else if (status_code == 302) {
      // Handle redirect (common with Google Apps Script)
      ESP_LOGI(
          TAG,
          "Received redirect (302) - this is normal for Google Apps Script");

      // Read response for any error details
      char response_buffer[512];
      int data_read = esp_http_client_read_response(
          client->http_client, response_buffer, sizeof(response_buffer) - 1);
      if (data_read > 0) {
        response_buffer[data_read] = '\0';
        ESP_LOGI(TAG, "Redirect response: %s", response_buffer);
      }

      // For Google Apps Script, 302 is often success
      ESP_LOGI(TAG, "Status '%s' likely sent successfully (302 redirect)",
               status_str);
    } else {
      ESP_LOGW(TAG, "HTTP request completed with status code: %d", status_code);

      // Read response body for error details
      char response_buffer[512];
      int data_read = esp_http_client_read_response(
          client->http_client, response_buffer, sizeof(response_buffer) - 1);
      if (data_read > 0) {
        response_buffer[data_read] = '\0';
        ESP_LOGW(TAG, "Error response: %s", response_buffer);
      }

      err = ESP_FAIL;
    }
  } else {
    ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));

    // For connection-related errors, return specific error codes
    if (err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_TIMEOUT) {
      ESP_LOGW(TAG, "Connection error detected");
      client->wifi_connected = false;  // Mark as disconnected
    }
  }

  esp_http_client_cleanup(client->http_client);
  client->http_client = NULL;

  return err;
}

// Also add this improved WiFi connection check function
bool gsheet_client_check_wifi_connection(gsheet_client_t* client) {
  if (!client) {
    return false;
  }

  // Check if we can get IP address (indicates WiFi is connected)
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif) {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      bool has_ip = (ip_info.ip.addr != 0);

      // Update internal state to match actual WiFi status
      if (has_ip != client->wifi_connected) {
        ESP_LOGI(TAG, "WiFi status updated: %s",
                 has_ip ? "Connected" : "Disconnected");
        client->wifi_connected = has_ip;
      }

      return has_ip;
    }
  }

  // If we can't get network info, assume disconnected
  client->wifi_connected = false;
  return false;
}

bool gsheet_client_is_wifi_connected(gsheet_client_t* client) {
  if (!client) {
    return false;
  }
  return client->wifi_connected;
}

void gsheet_client_deinit(gsheet_client_t* client) {
  if (!client) {
    return;
  }

  // Stop WiFi if connected
  if (client->wifi_connected) {
    esp_wifi_stop();
    client->wifi_connected = false;
  }

  // Clean up HTTP client if exists
  if (client->http_client) {
    esp_http_client_cleanup(client->http_client);
    client->http_client = NULL;
  }

  // Free configuration strings
  if (client->config.apps_script_url) {
    free(client->config.apps_script_url);
    client->config.apps_script_url = NULL;
  }
  if (client->config.wifi_ssid) {
    free(client->config.wifi_ssid);
    client->config.wifi_ssid = NULL;
  }
  if (client->config.wifi_password) {
    free(client->config.wifi_password);
    client->config.wifi_password = NULL;
  }

  // Clean up event group
  if (s_wifi_event_group) {
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;
  }

  memset(client, 0, sizeof(gsheet_client_t));
  ESP_LOGI(TAG, "Google Sheets client deinitialized");
}