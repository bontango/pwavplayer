//----------------------------------------------------------------------------------------
//
// httpserver.c -- HTTP REST config server for pwavplayer
//
// Runs in parallel to the USB/UART config editor.  Mirrors the LISYclock API
// so the same web editor can talk to either device.
//
// Path-traversal protection and SD helpers come from cmdapi.c.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"

#include "pwav.h"
#include "cmdapi.h"
#include "httpserver.h"

static const char *TAG = "HTTP";

#define SDCARD_BASE   "/sdcard"
#define CONFIG_FILE   SDCARD_BASE "/config.txt"
#define UPDATE_FILE   SDCARD_BASE "/update.bin"
#define RECV_BUF_SIZE 1024

extern char *gversion;                        // from wavplayer.c
extern StreamBufferHandle_t xpinevt;          // from wavplayer.c

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void add_cors_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",          "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",         "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers",         "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
}

static void url_decode(const char *src, char *dst, size_t dst_len) {
    size_t i = 0, j = 0;
    while (src[i] && j < dst_len - 1) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static bool recv_to_file(httpd_req_t *req, FILE *f) {
    char buf[RECV_BUF_SIZE];
    int remaining = (int)req->content_len;
    while (remaining > 0) {
        int to_recv = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int n = httpd_req_recv(req, buf, to_recv);
        if (n <= 0) { ESP_LOGE(TAG, "recv error: %d", n); return false; }
        if ((int)fwrite(buf, 1, n, f) != n) {
            ESP_LOGE(TAG, "fwrite error, errno=%d", errno);
            return false;
        }
        remaining -= n;
    }
    return true;
}

// Extract URI filename segment after prefix, URL-decode, validate.
// Returns ESP_OK (name filled) or writes an error response and returns ESP_FAIL.
static esp_err_t extract_filename(httpd_req_t *req, const char *prefix,
                                  char *out, size_t out_len) {
    const char *uri = req->uri;
    if (strncmp(uri, prefix, strlen(prefix)) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }
    url_decode(uri + strlen(prefix), out, out_len);
    // Strip any query string
    char *q = strchr(out, '?');
    if (q) *q = '\0';
    if (out[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty filename");
        return ESP_FAIL;
    }
    if (strchr(out, '/') || strstr(out, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Very small JSON string extractor — "key":"value"
static bool extract_json_str(const char *body, const char *key, char *out, size_t out_len) {
    char search[72];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(body, search);
    if (!p) return false;
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
    return (*p == '"');
}

// Extract a JSON integer value — "key":123
static bool extract_json_int(const char *body, const char *key, long *out) {
    char search[72];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(body, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (!*p) return false;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static esp_err_t options_handler(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"version\":\"%s\",\"api_version\":%s,\"device\":\"pwavplayer\"}",
             gversion, HTTP_API_VERSION_STR);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req) {
    add_cors_headers(req);
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "config.txt not found");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain");
    char buf[RECV_BUF_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            fclose(f); return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req) {
    add_cors_headers(req);
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write config.txt");
        return ESP_OK;
    }
    bool ok = recv_to_file(req, f);
    fclose(f);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t files_list_get_handler(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char *json = NULL;
    if (cmdapi_file_list_json(&json) != ESP_OK || !json) {
        httpd_resp_sendstr(req, "{\"files\":[]}");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t files_get_handler(httpd_req_t *req) {
    add_cors_headers(req);
    char name[128];
    if (extract_filename(req, "/files/", name, sizeof(name)) != ESP_OK) return ESP_OK;

    char path[256];
    if (cmdapi_resolve_path(name, path, sizeof(path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_OK;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char buf[RECV_BUF_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            fclose(f); return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t files_put_handler(httpd_req_t *req) {
    add_cors_headers(req);
    char name[128];
    if (extract_filename(req, "/files/", name, sizeof(name)) != ESP_OK) return ESP_OK;

    char path[256];
    if (cmdapi_resolve_path(name, path, sizeof(path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_OK;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
        return ESP_OK;
    }
    bool ok = recv_to_file(req, f);
    fclose(f);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t files_delete_handler(httpd_req_t *req) {
    add_cors_headers(req);
    char name[128];
    if (extract_filename(req, "/files/", name, sizeof(name)) != ESP_OK) return ESP_OK;

    esp_err_t r = cmdapi_file_delete(name);
    if (r != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t rename_post_handler(httpd_req_t *req) {
    add_cors_headers(req);
    char buf[384];
    int to_recv = (req->content_len > 0 && req->content_len < (int)sizeof(buf) - 1)
                  ? (int)req->content_len : (int)(sizeof(buf) - 1);
    int len = httpd_req_recv(req, buf, to_recv);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    buf[len] = '\0';

    char old_name[128] = {0};
    char new_name[128] = {0};
    if (!extract_json_str(buf, "old_name", old_name, sizeof(old_name)) ||
        !extract_json_str(buf, "new_name", new_name, sizeof(new_name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing old_name or new_name");
        return ESP_OK;
    }
    if (cmdapi_file_rename(old_name, new_name) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t time_post_handler(httpd_req_t *req) {
    add_cors_headers(req);
    char buf[64];
    int to_recv = (req->content_len > 0 && req->content_len < (int)sizeof(buf) - 1)
                  ? (int)req->content_len : (int)(sizeof(buf) - 1);
    int len = httpd_req_recv(req, buf, to_recv);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    buf[len] = '\0';
    long ts = 0;
    if (!extract_json_int(buf, "unix_timestamp", &ts) || ts <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid unix_timestamp");
        return ESP_OK;
    }
    struct timeval tv = { .tv_sec = (time_t)ts, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req) {
    add_cors_headers(req);
    char buf[64];
    int to_recv = (req->content_len > 0 && req->content_len < (int)sizeof(buf) - 1)
                  ? (int)req->content_len : (int)(sizeof(buf) - 1);
    int len = httpd_req_recv(req, buf, to_recv);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    buf[len] = '\0';
    if (!strstr(buf, "\"confirm\"") || !strstr(buf, "\"reboot\"")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Confirm required: {\"confirm\":\"reboot\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    vTaskDelay(pdMS_TO_TICKS(100));
    cmdapi_reboot();
    return ESP_OK;
}

static esp_err_t update_post_handler(httpd_req_t *req) {
    add_cors_headers(req);
    FILE *f = fopen(UPDATE_FILE, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create update.bin");
        return ESP_OK;
    }
    bool ok = recv_to_file(req, f);
    fclose(f);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    vTaskDelay(pdMS_TO_TICKS(200));
    cmdapi_reboot();
    return ESP_OK;
}

// POST /play  — trigger a sound by ID via the xpinevt stream buffer
static esp_err_t play_post_handler(httpd_req_t *req) {
    add_cors_headers(req);
    char buf[64];
    int to_recv = (req->content_len > 0 && req->content_len < (int)sizeof(buf) - 1)
                  ? (int)req->content_len : (int)(sizeof(buf) - 1);
    int len = httpd_req_recv(req, buf, to_recv);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    buf[len] = '\0';
    long id = 0;
    if (!extract_json_int(buf, "id", &id) || id <= 0 || id > 65535) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid id");
        return ESP_OK;
    }
    Rxcmd cmd = { .cmd = 'p', .arg = (uint16_t)id };
    xStreamBufferSend(xpinevt, &cmd, sizeof(Rxcmd), 1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// httpserver_start
// ---------------------------------------------------------------------------

esp_err_t httpserver_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn    = httpd_uri_match_wildcard;
    cfg.stack_size      = 8192;
    cfg.max_uri_handlers = 16;
    cfg.server_port     = HTTP_SERVER_PORT;
    cfg.ctrl_port       = 32769;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

#define REG(uri_, method_, handler_) do {                                \
    static const httpd_uri_t _u = { .uri = (uri_), .method = (method_),  \
                                    .handler = (handler_) };             \
    httpd_register_uri_handler(server, &_u);                             \
} while (0)

    REG("/status",   HTTP_GET,    status_get_handler);
    REG("/config",   HTTP_GET,    config_get_handler);
    REG("/config",   HTTP_POST,   config_post_handler);
    REG("/files",    HTTP_GET,    files_list_get_handler);
    REG("/files/*",  HTTP_GET,    files_get_handler);
    REG("/files/*",  HTTP_PUT,    files_put_handler);
    REG("/files/*",  HTTP_DELETE, files_delete_handler);
    REG("/rename",   HTTP_POST,   rename_post_handler);
    REG("/time",     HTTP_POST,   time_post_handler);
    REG("/reboot",   HTTP_POST,   reboot_post_handler);
    REG("/update",   HTTP_POST,   update_post_handler);
    REG("/play",     HTTP_POST,   play_post_handler);
    REG("/*",        HTTP_OPTIONS, options_handler);
#undef REG

    ESP_LOGI(TAG, "HTTP server started on port %d", cfg.server_port);
    return ESP_OK;
}
