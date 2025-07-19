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
static const int WIFI_MAXIMUM_RETRY = 5;  // Increased retry count
static bool s_wifi_initialized = false;
static esp_event_handler_instance_t wifi_handler_instance = NULL;
static esp_event_handler_instance_t ip_handler_instance = NULL;

static void wifi_event_handler(void* arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    ESP_LOGI(TAG, "WiFi station started, attempting connection...");
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t* disconn_event =
        (wifi_event_sta_disconnected_t*)event_data;
    ESP_LOGW(TAG, "WiFi disconnected, reason: %d", disconn_event->reason);

    if (s_retry_num < WIFI_MAXIMUM_RETRY) {
      ESP_LOGI(TAG, "Retry to connect to the AP (%d/%d)", s_retry_num + 1,
               WIFI_MAXIMUM_RETRY);
      vTaskDelay(pdMS_TO_TICKS(1000));  // Wait 1 second before retry
      esp_wifi_connect();
      s_retry_num++;
    } else {
      ESP_LOGE(TAG, "Failed to connect after %d attempts", WIFI_MAXIMUM_RETRY);
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
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

    // Set WiFi mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_wifi_initialized = true;
  }

  ESP_LOGI(TAG, "Google Sheets client initialized");
  return ESP_OK;
}

esp_err_t gsheet_client_wifi_connect(gsheet_client_t* client) {
  if (!client) {
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "Starting WiFi connection process...");

  // Stop WiFi first if it was running
  esp_wifi_stop();
  vTaskDelay(pdMS_TO_TICKS(500));  // Give it time to stop completely

  // Unregister any existing event handlers
  if (wifi_handler_instance) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_handler_instance);
    wifi_handler_instance = NULL;
  }
  if (ip_handler_instance) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          ip_handler_instance);
    ip_handler_instance = NULL;
  }

  // Delete existing event group and create new one
  if (s_wifi_event_group) {
    vEventGroupDelete(s_wifi_event_group);
  }
  s_wifi_event_group = xEventGroupCreate();
  if (!s_wifi_event_group) {
    ESP_LOGE(TAG, "Failed to create WiFi event group");
    return ESP_FAIL;
  }

  // Reset retry counter
  s_retry_num = 0;

  // Register event handlers with instances
  esp_err_t ret = esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
      &wifi_handler_instance);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register WiFi event handler: %s",
             esp_err_to_name(ret));
    return ret;
  }

  ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &wifi_event_handler, NULL,
                                            &ip_handler_instance);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register IP event handler: %s",
             esp_err_to_name(ret));
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_handler_instance);
    wifi_handler_instance = NULL;
    return ret;
  }

  // Configure WiFi
  wifi_config_t wifi_config = {
      .sta =
          {
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
              .pmf_cfg = {.capable = true, .required = false},
              .scan_method = WIFI_FAST_SCAN,
              .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
              .failure_retry_cnt = 3,
          },
  };

  // Clear the configuration first
  memset(wifi_config.sta.ssid, 0, sizeof(wifi_config.sta.ssid));
  memset(wifi_config.sta.password, 0, sizeof(wifi_config.sta.password));

  // Copy SSID and password
  strncpy((char*)wifi_config.sta.ssid, client->config.wifi_ssid,
          sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char*)wifi_config.sta.password, client->config.wifi_password,
          sizeof(wifi_config.sta.password) - 1);

  // Set WiFi configuration
  ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
    return ret;
  }

  // Start WiFi
  ret = esp_wifi_start();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "WiFi started. Connecting to %s...", client->config.wifi_ssid);

  /* Wait until either the connection is established (WIFI_CONNECTED_BIT) or
   * connection failed */
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
      pdMS_TO_TICKS(20000));  // Increased timeout to 20 seconds

  esp_err_t result = ESP_FAIL;

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to AP SSID:%s", client->config.wifi_ssid);
    client->wifi_connected = true;
    result = ESP_OK;
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGE(TAG, "Failed to connect to SSID:%s after %d attempts",
             client->config.wifi_ssid, WIFI_MAXIMUM_RETRY);
    client->wifi_connected = false;
    result = ESP_FAIL;
  } else {
    ESP_LOGE(TAG, "WiFi connection timeout after 20 seconds");
    client->wifi_connected = false;
    result = ESP_ERR_TIMEOUT;

    // Stop WiFi on timeout
    esp_wifi_stop();
  }

  // Clean up event handlers on failure
  if (result != ESP_OK) {
    if (wifi_handler_instance) {
      esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_handler_instance);
      wifi_handler_instance = NULL;
    }
    if (ip_handler_instance) {
      esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            ip_handler_instance);
      ip_handler_instance = NULL;
    }
  }

  return result;
}

esp_err_t gsheet_client_send_status(gsheet_client_t* client,
                                    gsheet_status_t status) {
  if (!client) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!client->wifi_connected) {
    ESP_LOGE(TAG, "WiFi not connected");
    return ESP_ERR_INVALID_STATE;
  }

  // Double-check WiFi connection before making HTTP request
  if (!gsheet_client_check_wifi_connection(client)) {
    ESP_LOGW(TAG, "WiFi connection lost during send attempt");
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
      .keep_alive_enable =
          false,  // Disable keep-alive to avoid connection issues
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
  esp_http_client_set_header(client->http_client, "Connection", "close");

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
      ESP_LOGW(TAG, "Connection error detected, marking WiFi as disconnected");
      client->wifi_connected = false;  // Mark as disconnected
    }
  }

  esp_http_client_cleanup(client->http_client);
  client->http_client = NULL;

  return err;
}

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
  return gsheet_client_check_wifi_connection(client);
}

void gsheet_client_deinit(gsheet_client_t* client) {
  if (!client) {
    return;
  }

  // Unregister event handlers
  if (wifi_handler_instance) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_handler_instance);
    wifi_handler_instance = NULL;
  }
  if (ip_handler_instance) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          ip_handler_instance);
    ip_handler_instance = NULL;
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