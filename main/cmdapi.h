//----------------------------------------------------------------------------------------
//
// cmdapi.h -- Transport-agnostic command API for PWAVplayer
//
// Shared between:
//   usbserial.c  (USB/UART transport)
//   httpserver.c (future WiFi/HTTP transport)
//
// Each transport layer calls these functions to perform file and device operations.
// The SD card mount point is resolved via the global mount_point[] from wavplayer.c.
//

#pragma once

#include <stddef.h>
#include "esp_err.h"

// Build a JSON string listing all files in the SD card root directory.
// Format: {"files":[{"name":"xxx","size":NNN},...]}
// Caller must free() the returned string.
// Returns ESP_OK on success, error code on failure (json_out set to NULL).
esp_err_t cmdapi_file_list_json(char **json_out);

// Resolve a bare filename to a full SD card path.
// Rejects filenames containing "..", "/", or "\\" (path traversal prevention).
// Returns ESP_OK on success, ESP_ERR_INVALID_ARG if unsafe, ESP_ERR_INVALID_SIZE if too long.
esp_err_t cmdapi_resolve_path(const char *fname, char *out, size_t out_len);

// Delete a file from the SD card root directory.
// Returns ESP_OK on success, ESP_ERR_INVALID_ARG for unsafe filename, ESP_FAIL on error.
esp_err_t cmdapi_file_delete(const char *fname);

// Rename a file in the SD card root directory.
// Both old_name and new_name must be bare filenames (no path components).
// Returns ESP_OK on success, ESP_ERR_INVALID_ARG for unsafe filename, ESP_FAIL on error.
esp_err_t cmdapi_file_rename(const char *old_name, const char *new_name);

// Trigger a software reset of the ESP32.  Does not return.
void cmdapi_reboot(void);
