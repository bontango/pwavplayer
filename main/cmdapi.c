//----------------------------------------------------------------------------------------
//
// cmdapi.c -- Transport-agnostic command API for PWAVplayer
//
// Implements file system operations and device control used by:
//   usbserial.c  (USB/UART transport)
//   httpserver.c (future WiFi/HTTP transport)
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "esp_system.h"
#include "esp_log.h"
#include "cmdapi.h"

static const char *TAG = "API";

// Defined in wavplayer.c
extern const char mount_point[];


//----------------------------------------------------------------------------------------
// cmdapi_resolve_path
//----------------------------------------------------------------------------------------
esp_err_t cmdapi_resolve_path(const char *fname, char *out, size_t out_len) {
    if (!fname || !*fname) return ESP_ERR_INVALID_ARG;
    // Reject path traversal and directory separators
    if (strstr(fname, "..") || strchr(fname, '/') || strchr(fname, '\\')) {
        ESP_LOGW(TAG, "Rejected unsafe filename: %s", fname);
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out, out_len, "%s/%s", mount_point, fname);
    if (n < 0 || (size_t)n >= out_len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}


//----------------------------------------------------------------------------------------
// cmdapi_file_list_json
//----------------------------------------------------------------------------------------
esp_err_t cmdapi_file_list_json(char **json_out) {
    *json_out = NULL;

    DIR *d = opendir(mount_point);
    if (!d) {
        ESP_LOGW(TAG, "Cannot open dir: %s", mount_point);
        return ESP_ERR_NOT_FOUND;
    }

    // Start with a reasonable buffer; grow as needed
    size_t bufsz = 512;
    char  *buf   = malloc(bufsz);
    if (!buf) { closedir(d); return ESP_ERR_NO_MEM; }

    int pos = snprintf(buf, bufsz, "{\"files\":[");
    bool first = true;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG) continue;  // skip subdirectories

        // Get file size
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/%s", mount_point, e->d_name);
        FILE *f = fopen(fpath, "r");
        long fsz = 0;
        if (f) { fseek(f, 0, SEEK_END); fsz = ftell(f); fclose(f); }

        // Ensure buffer has space for this entry
        size_t need = (size_t)pos + strlen(e->d_name) + 40;
        if (need >= bufsz) {
            bufsz = need + 256;
            char *nb = realloc(buf, bufsz);
            if (!nb) { free(buf); closedir(d); return ESP_ERR_NO_MEM; }
            buf = nb;
        }

        if (!first) buf[pos++] = ',';
        pos += snprintf(buf + pos, bufsz - pos,
                        "{\"name\":\"%s\",\"size\":%ld}", e->d_name, fsz);
        first = false;
    }
    closedir(d);

    // Final closing brackets
    if ((size_t)(pos + 8) >= bufsz) {
        bufsz = (size_t)pos + 8;
        char *nb = realloc(buf, bufsz);
        if (!nb) { free(buf); return ESP_ERR_NO_MEM; }
        buf = nb;
    }
    snprintf(buf + pos, bufsz - (size_t)pos, "]}");

    *json_out = buf;
    return ESP_OK;
}


//----------------------------------------------------------------------------------------
// cmdapi_file_delete
//----------------------------------------------------------------------------------------
esp_err_t cmdapi_file_delete(const char *fname) {
    char fpath[512];
    if (cmdapi_resolve_path(fname, fpath, sizeof(fpath)) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    if (remove(fpath) != 0) {
        ESP_LOGW(TAG, "Delete failed: %s", fpath);
        return ESP_FAIL;
    }
    return ESP_OK;
}


//----------------------------------------------------------------------------------------
// cmdapi_file_rename
//----------------------------------------------------------------------------------------
esp_err_t cmdapi_file_rename(const char *old_name, const char *new_name) {
    char old_path[512], new_path[512];
    if (cmdapi_resolve_path(old_name, old_path, sizeof(old_path)) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    if (cmdapi_resolve_path(new_name, new_path, sizeof(new_path)) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    if (rename(old_path, new_path) != 0) {
        ESP_LOGW(TAG, "Rename failed: %s -> %s", old_path, new_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}


//----------------------------------------------------------------------------------------
// cmdapi_reboot
//----------------------------------------------------------------------------------------
void cmdapi_reboot(void) {
    esp_restart();
}
