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
#include <sys/stat.h>
#include "esp_system.h"
#include "esp_log.h"
#include "cmdapi.h"

static const char *TAG = "API";

// Defined in wavplayer.c
extern const char mount_point[];
extern char gconfsd[];   // active sound-theme directory name


//----------------------------------------------------------------------------------------
// cmdapi_resolve_path
//
// Accepts one of:
//   "<file>"            → /sdcard/<file>            (root file)
//   "<theme>/<file>"    → /sdcard/<theme>/<file>    (sound-theme file)
//
// Both segments must be free of "..", '/', '\\'.  At most one '/' separator.
//----------------------------------------------------------------------------------------
static int seg_is_safe(const char *s, size_t len) {
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '/' || c == '\\') return 0;
    }
    if (len >= 2 && s[0] == '.' && s[1] == '.') return 0;
    // "..xxx" is fine; only the literal ".." run anywhere is unsafe
    for (size_t i = 0; i + 1 < len; i++) {
        if (s[i] == '.' && s[i+1] == '.') return 0;
    }
    return 1;
}

esp_err_t cmdapi_resolve_path(const char *fname, char *out, size_t out_len) {
    if (!fname || !*fname) return ESP_ERR_INVALID_ARG;
    const char *slash = strchr(fname, '/');
    if (strchr(fname, '\\')) {
        ESP_LOGW(TAG, "Rejected unsafe filename: %s", fname);
        return ESP_ERR_INVALID_ARG;
    }
    int n;
    if (slash == NULL) {
        if (!seg_is_safe(fname, strlen(fname))) {
            ESP_LOGW(TAG, "Rejected unsafe filename: %s", fname);
            return ESP_ERR_INVALID_ARG;
        }
        n = snprintf(out, out_len, "%s/%s", mount_point, fname);
    } else {
        // exactly one slash allowed
        if (strchr(slash + 1, '/')) {
            ESP_LOGW(TAG, "Rejected nested path: %s", fname);
            return ESP_ERR_INVALID_ARG;
        }
        size_t tlen = (size_t)(slash - fname);
        const char *file = slash + 1;
        if (!seg_is_safe(fname, tlen) || !seg_is_safe(file, strlen(file))) {
            ESP_LOGW(TAG, "Rejected unsafe path: %s", fname);
            return ESP_ERR_INVALID_ARG;
        }
        n = snprintf(out, out_len, "%s/%.*s/%s", mount_point, (int)tlen, fname, file);
    }
    if (n < 0 || (size_t)n >= out_len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}


//----------------------------------------------------------------------------------------
// list_directory_into  — append entries of one directory to the JSON buffer.
// `subdir` is "" for root, or the theme name for /sdcard/<theme>.
// Writes regular files only; directory entries at the root level get type "dir".
//----------------------------------------------------------------------------------------
static esp_err_t list_directory_into(char **buf_io, size_t *bufsz_io, int *pos_io,
                                     bool *first_io, const char *subdir,
                                     bool include_dirs, bool with_stat)
{
    char dpath[256];
    if (subdir[0] == '\0')
        snprintf(dpath, sizeof(dpath), "%s", mount_point);
    else
        snprintf(dpath, sizeof(dpath), "%s/%s", mount_point, subdir);

    DIR *d = opendir(dpath);
    if (!d) {
        ESP_LOGW(TAG, "Cannot open dir: %s", dpath);
        return ESP_OK;  // missing dir is not fatal — caller may have other dirs
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        bool is_dir = (e->d_type == DT_DIR);
        if (!is_dir && e->d_type != DT_REG) continue;
        if (is_dir && !include_dirs) continue;

        // stat() is expensive on FAT32 (each call rescans the directory).  Only
        // call it for small directories (root) where the editor displays size/
        // mtime; skip for theme dirs which can hold hundreds of WAVs.
        long fsz = 0;
        long fmtime = 0;
        if (with_stat) {
            char fpath[512];
            if (subdir[0] == '\0')
                snprintf(fpath, sizeof(fpath), "%s/%s", mount_point, e->d_name);
            else
                snprintf(fpath, sizeof(fpath), "%s/%s/%s", mount_point, subdir, e->d_name);
            struct stat st;
            if (stat(fpath, &st) == 0) {
                fsz    = (long)st.st_size;
                fmtime = (long)st.st_mtime;
            }
        }

        size_t need = (size_t)*pos_io + strlen(e->d_name) + strlen(subdir) + 96;
        if (need >= *bufsz_io) {
            size_t nsz = need + 256;
            char *nb = realloc(*buf_io, nsz);
            if (!nb) { closedir(d); return ESP_ERR_NO_MEM; }
            *buf_io = nb;
            *bufsz_io = nsz;
        }

        if (!*first_io) (*buf_io)[(*pos_io)++] = ',';
        *pos_io += snprintf(*buf_io + *pos_io, *bufsz_io - *pos_io,
                            "{\"name\":\"%s\",\"dir\":\"%s\",\"size\":%ld,\"mtime\":%ld%s}",
                            e->d_name, subdir, fsz, fmtime,
                            is_dir ? ",\"type\":\"dir\"" : "");
        *first_io = false;
    }
    closedir(d);
    return ESP_OK;
}

esp_err_t cmdapi_file_list_json(char **json_out) {
    *json_out = NULL;

    size_t bufsz = 1024;
    char  *buf   = malloc(bufsz);
    if (!buf) return ESP_ERR_NO_MEM;

    int  pos   = snprintf(buf, bufsz, "{\"theme\":\"%s\",\"files\":[", gconfsd);
    bool first = true;

    // Root: include regular files AND directories (so editor sees themes).
    // stat() each entry — root holds few files and the editor shows size/mtime.
    esp_err_t r = list_directory_into(&buf, &bufsz, &pos, &first, "", true, true);
    if (r != ESP_OK) { free(buf); return r; }
    // Active theme directory: only regular files, skip stat() for speed.
    if (gconfsd[0]) {
        r = list_directory_into(&buf, &bufsz, &pos, &first, gconfsd, false, false);
        if (r != ESP_OK) { free(buf); return r; }
    }

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
