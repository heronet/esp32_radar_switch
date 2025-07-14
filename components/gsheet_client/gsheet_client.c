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
static const int WIFI_MAXIMUM_RETRY = 5;

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
      ESP_LOGI(TAG, "Retry to connect to the AP");
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

  // Initialize NVS
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

  ESP_LOGI(TAG, "Google Sheets client initialized");
  return ESP_OK;
}

esp_err_t gsheet_client_wifi_connect(gsheet_client_t* client) {
  if (!client) {
    return ESP_ERR_INVALID_ARG;
  }

  s_wifi_event_group = xEventGroupCreate();

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
      pdMS_TO_TICKS(30000));  // 30 second timeout

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to AP SSID:%s", client->config.wifi_ssid);
    client->wifi_connected = true;
    return ESP_OK;
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(TAG, "Failed to connect to SSID:%s", client->config.wifi_ssid);
    client->wifi_connected = false;
    return ESP_FAIL;
  } else {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
    return ESP_FAIL;
  }
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

  // Create HTTP client configuration
  esp_http_client_config_t config = {
      .url = client->config.apps_script_url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = client->config.timeout_ms,
      .skip_cert_common_name_check = true,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 4096,  // Match CONFIG_HTTP_BUF_SIZE
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

  // Create POST data
  char post_data[32];
  snprintf(post_data, sizeof(post_data), "status=%s",
           (status == GSHEET_STATUS_ON) ? "ON" : "OFF");

  // Set POST data
  esp_http_client_set_post_field(client->http_client, post_data,
                                 strlen(post_data));

  // Perform HTTP request
  esp_err_t err = esp_http_client_perform(client->http_client);
  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client->http_client);
    ESP_LOGI(TAG, "HTTP POST Status = %d", status_code);

    if (status_code == 200) {
      ESP_LOGI(TAG, "Status '%s' sent successfully",
               (status == GSHEET_STATUS_ON) ? "ON" : "OFF");
    } else {
      ESP_LOGW(TAG, "HTTP request completed with status code: %d", status_code);
    }
  } else {
    ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
  }

  esp_http_client_cleanup(client->http_client);
  return err;
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

  if (client->config.apps_script_url) {
    free(client->config.apps_script_url);
  }
  if (client->config.wifi_ssid) {
    free(client->config.wifi_ssid);
  }
  if (client->config.wifi_password) {
    free(client->config.wifi_password);
  }

  if (s_wifi_event_group) {
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;
  }

  memset(client, 0, sizeof(gsheet_client_t));
  ESP_LOGI(TAG, "Google Sheets client deinitialized");
}