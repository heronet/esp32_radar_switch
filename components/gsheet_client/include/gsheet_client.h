#ifndef GSHEET_CLIENT_H
#define GSHEET_CLIENT_H

#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Google Sheets client configuration
 */
typedef struct {
  char* apps_script_url;  ///< Google Apps Script Web App URL
  char* wifi_ssid;        ///< WiFi SSID
  char* wifi_password;    ///< WiFi password
  int timeout_ms;         ///< HTTP request timeout in milliseconds
} gsheet_config_t;

/**
 * @brief Google Sheets client handle
 */
typedef struct {
  gsheet_config_t config;
  bool wifi_connected;
  esp_http_client_handle_t http_client;
} gsheet_client_t;

/**
 * @brief Status types for Google Sheets logging
 */
typedef enum { GSHEET_STATUS_OFF = 0, GSHEET_STATUS_ON = 1 } gsheet_status_t;

/**
 * @brief Initialize Google Sheets client
 *
 * @param client Pointer to gsheet_client_t structure
 * @param config Configuration parameters
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t gsheet_client_init(gsheet_client_t* client,
                             const gsheet_config_t* config);

/**
 * @brief Connect to WiFi
 *
 * @param client Pointer to gsheet_client_t structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t gsheet_client_wifi_connect(gsheet_client_t* client);

/**
 * @brief Send status to Google Sheets
 *
 * @param client Pointer to gsheet_client_t structure
 * @param status Status to send (ON or OFF)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t gsheet_client_send_status(gsheet_client_t* client,
                                    gsheet_status_t status);

/**
 * @brief Check if WiFi is connected
 *
 * @param client Pointer to gsheet_client_t structure
 * @return true if connected, false otherwise
 */
bool gsheet_client_is_wifi_connected(gsheet_client_t* client);

/**
 * @brief Deinitialize Google Sheets client
 *
 * @param client Pointer to gsheet_client_t structure
 */
void gsheet_client_deinit(gsheet_client_t* client);

#ifdef __cplusplus
}
#endif

#endif  // GSHEET_CLIENT_H