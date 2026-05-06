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

// Build a JSON string listing files at the SD root and inside the active sound
// theme directory.
// Format:
//   {"theme":"orgsnd",
//    "files":[
//      {"name":"config.txt","dir":"","size":NNN,"mtime":TTT},
//      {"name":"orgsnd","dir":"","size":0,"mtime":TTT,"type":"dir"},
//      {"name":"0001-xbxx-100-foo.wav","dir":"orgsnd","size":NNN,"mtime":TTT},
//      ...
//    ]}
// Caller must free() the returned string.
// Returns ESP_OK on success, error code on failure (json_out set to NULL).
esp_err_t cmdapi_file_list_json(char **json_out);

// Resolve a filename or "<theme>/<file>" path to a full SD card path.
// Rejects ".." segments and backslashes; at most one '/' separator allowed.
// Returns ESP_OK on success, ESP_ERR_INVALID_ARG if unsafe, ESP_ERR_INVALID_SIZE if too long.
esp_err_t cmdapi_resolve_path(const char *fname, char *out, size_t out_len);

// Delete a file from the SD card.  Accepts bare filename or "<theme>/<file>".
// Returns ESP_OK on success, ESP_ERR_INVALID_ARG for unsafe filename, ESP_FAIL on error.
esp_err_t cmdapi_file_delete(const char *fname);

// Rename a file on the SD card.  Both names accept bare or "<theme>/<file>" form.
// Returns ESP_OK on success, ESP_ERR_INVALID_ARG for unsafe filename, ESP_FAIL on error.
esp_err_t cmdapi_file_rename(const char *old_name, const char *new_name);

// Trigger a software reset of the ESP32.  Does not return.
void cmdapi_reboot(void);
