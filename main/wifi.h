//----------------------------------------------------------------------------------------
//
// wifi.h -- STA WiFi init for pwavplayer
//
// Reads gwifi_ssid / gwifi_pwd / gconf[CONF_WIFI_ENABLE] and connects to the
// configured access point.  Blocks up to ~15s waiting for an IP, then returns.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Initialise STA mode and attempt to connect.  Blocks until connected or timeout.
// Safe to call when WiFi is disabled — it will simply be a no-op.
void wifi_init_sta(void);

// Returns true if WiFi is currently associated + has an IP.
bool wifi_is_connected(void);

// Writes the dotted-decimal IP into out (max n bytes).  Returns ESP_OK on success.
esp_err_t wifi_get_ip_str(char *out, size_t n);
