//----------------------------------------------------------------------------------------
//
// usbserial.c -- USB/UART config-editor transport for PWAVplayer
//
// Implements the frame-based serial protocol defined in webapp/API.md.
// Called as the "UsbSerial" FreeRTOS task when ser=uart in config.txt.
//
// Protocol summary:
//   Handshake         RX: 0x55 (single byte)         TX: "OK:READY\r\n"
//   File list         RX: "FILE:LIST"                 TX: DATA frame (JSON)
//   File download     RX: "FILE:DOWNLOAD=<name>"      TX: BINDATA frame
//   File upload       RX: "FILE:UPLOAD=<name>"
//                     RX: BINDATA frame               TX: "OK:FILE_SAVED\r\n"
//   Reboot            RX: "REBOOT"                    TX: "OK:REBOOTING\r\n" + restart
//   Play (legacy)     RX: "p <num>"                   -> WAVPlayer stream buffer
//
// Uses synchronous byte-by-byte UART reads (no event queue).
// Modelled after the proven LISYclock usb_com.c approach.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"

#include "platform.h"
#include "pgpio.h"
#include "pwav.h"
#include "cmdapi.h"
#include "wifi.h"

extern StreamBufferHandle_t xpinevt;
extern uint16_t gconf[CONF_MAX];
extern char gwifi_ssid[];

static const char *TAG = "USB";

// UART configuration
#define USB_UART_PORT   UART_NUM_0
#define USB_RXBUF_SZ    4096

extern uint32_t gusbbaud;

// Protocol constants
#define USB_LINE_MAX    256
#define B64_CHUNK       720     // raw bytes per base64 line (-> 960 chars encoded)
#define B64_OUT_MAX     1024
#define DECODE_BUF_SZ   200
#define ACK_EVERY       8
#define ACK_TIMEOUT_MS  5000
#define RX_BYTE_TIMEOUT_MS 10000  // max wait per byte during frame reception


//========================================================================================
// Helpers
//========================================================================================

static void usb_tx(const char *s) {
    uart_write_bytes(USB_UART_PORT, s, strlen(s));
}

// Read a single byte from UART with timeout.  Returns 1 on success, 0 on timeout.
static int usb_read_byte(uint8_t *out, int timeout_ms) {
    return uart_read_bytes(USB_UART_PORT, out, 1, pdMS_TO_TICKS(timeout_ms));
}

// Read a complete line (up to '\n') into buf[].  Returns length, or -1 on timeout.
// Strips \r, null-terminates.
static int usb_read_line(char *buf, int max, int timeout_ms) {
    int idx = 0;
    while (1) {
        uint8_t b;
        if (usb_read_byte(&b, timeout_ms) != 1) return -1;
        if (b == '\r') continue;
        if (b == '\n') { buf[idx] = '\0'; return idx; }
        if (idx < max - 1) buf[idx++] = (char)b;
    }
}


//========================================================================================
// Base64
//========================================================================================

static const char B64T[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t *src, size_t src_len, char *out) {
    size_t j = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        uint8_t a = src[i];
        uint8_t b = (i + 1 < src_len) ? src[i + 1] : 0;
        uint8_t c = (i + 2 < src_len) ? src[i + 2] : 0;
        out[j++] = B64T[a >> 2];
        out[j++] = B64T[((a & 0x03) << 4) | (b >> 4)];
        out[j++] = (i + 1 < src_len) ? B64T[((b & 0x0f) << 2) | (c >> 6)] : '=';
        out[j++] = (i + 2 < src_len) ? B64T[c & 0x3f] : '=';
    }
    out[j] = '\0';
}

static int b64_cv(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *b64, uint8_t *out, size_t out_max) {
    int len = (int)strlen(b64);
    while (len > 0 && (b64[len-1] == '=' || b64[len-1] == ' ' || b64[len-1] == '\r'))
        len--;

    size_t out_len = 0;
    for (int i = 0; i < len; i += 4) {
        int v0 = b64_cv(b64[i]);
        int v1 = (i + 1 < len) ? b64_cv(b64[i + 1]) : -1;
        int v2 = (i + 2 < len) ? b64_cv(b64[i + 2]) : -1;
        int v3 = (i + 3 < len) ? b64_cv(b64[i + 3]) : -1;
        if (v0 < 0 || v1 < 0) break;
        if (out_len < out_max) out[out_len++] = (uint8_t)((v0 << 2) | (v1 >> 4));
        if (v2 >= 0 && out_len < out_max) out[out_len++] = (uint8_t)(((v1 & 0x0f) << 4) | (v2 >> 2));
        if (v3 >= 0 && out_len < out_max) out[out_len++] = (uint8_t)(((v2 & 0x03) << 6) | v3);
    }
    return (int)out_len;
}


//========================================================================================
// Blocking frame I/O
//========================================================================================

// Wait for an "OK:..." line from the editor (used during downloads).
static bool wait_for_ack(void) {
    char tmp[40];
    int n = usb_read_line(tmp, sizeof(tmp), ACK_TIMEOUT_MS);
    if (n < 0) { ESP_LOGW(TAG, "ACK timeout"); return false; }
    return (n >= 2 && tmp[0] == 'O' && tmp[1] == 'K');
}

// Receive a BINDATA frame and write it to an open file.  Blocking.
// Returns true on success, false on error/timeout.
static bool recv_binary_frame(FILE *fp) {
    char line[USB_LINE_MAX];
    int line_count = 0;

    // First line must be BINDATA:BEGIN=<size>
    int n = usb_read_line(line, sizeof(line), RX_BYTE_TIMEOUT_MS);
    if (n < 0 || strncmp(line, "BINDATA:BEGIN=", 14) != 0) {
        ESP_LOGW(TAG, "Expected BINDATA:BEGIN, got: %s", n >= 0 ? line : "(timeout)");
        return false;
    }

    // Read base64 lines until BINDATA:END
    while (1) {
        n = usb_read_line(line, sizeof(line), RX_BYTE_TIMEOUT_MS);
        if (n < 0) {
            ESP_LOGW(TAG, "Upload timeout at line %d", line_count);
            return false;
        }
        if (strcmp(line, "BINDATA:END") == 0) break;

        // Decode and write
        uint8_t dec[DECODE_BUF_SZ];
        int dn = b64_decode(line, dec, sizeof(dec));
        if (dn > 0) {
            if (fwrite(dec, 1, (size_t)dn, fp) != (size_t)dn) {
                ESP_LOGE(TAG, "SD write error");
                return false;
            }
        }

        line_count++;
        if (line_count % ACK_EVERY == 0) {
            usb_tx("OK:CHUNK_ACK\r\n");
        }
    }

    usb_tx("OK:BINDATA_RECEIVED\r\n");
    return true;
}


//========================================================================================
// Command handlers
//========================================================================================

// FILE:DOWNLOAD=<name> — send file as BINDATA frame
static void send_file_bindata(const char *fname) {
    char fpath[128];
    if (cmdapi_resolve_path(fname, fpath, sizeof(fpath)) != ESP_OK) {
        usb_tx("ERR:INVALID_FILENAME\r\n");
        return;
    }

    FILE *fp = fopen(fpath, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "File not found: %s", fpath);
        usb_tx("ERR:FILE_NOT_FOUND\r\n");
        return;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "BINDATA:BEGIN=%ld\r\n", fsize);
    usb_tx(hdr);

    uint8_t raw[B64_CHUNK];
    char    b64[B64_OUT_MAX];
    int     line_count = 0;
    bool    ok = true;

    while (ok) {
        int n = (int)fread(raw, 1, B64_CHUNK, fp);
        if (n <= 0) break;
        b64_encode(raw, (size_t)n, b64);
        usb_tx(b64);
        usb_tx("\r\n");
        line_count++;
        if (line_count % ACK_EVERY == 0) {
            if (!wait_for_ack()) {
                ESP_LOGW(TAG, "Download aborted: no ACK at line %d", line_count);
                ok = false;
            }
        }
    }

    fclose(fp);

    if (ok) {
        usb_tx("BINDATA:END\r\n");
        wait_for_ack();
    }
}

// FILE:LIST — send JSON file listing as DATA frame
static void send_file_list(void) {
    char *json = NULL;
    if (cmdapi_file_list_json(&json) != ESP_OK || !json) {
        usb_tx("ERR:SD_NOT_MOUNTED\r\n");
        return;
    }

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "DATA:BEGIN=%d\r\n", (int)strlen(json));
    usb_tx(hdr);
    usb_tx(json);
    usb_tx("\r\n");
    usb_tx("DATA:END\r\n");
    free(json);

    wait_for_ack();
}

// FILE:UPLOAD=<name> — receive BINDATA frame and save to SD
static void handle_file_upload(const char *fname) {
    if (!fname || !*fname) {
        usb_tx("ERR:INVALID_FILENAME\r\n");
        return;
    }

    char fpath[128];
    if (cmdapi_resolve_path(fname, fpath, sizeof(fpath)) != ESP_OK) {
        usb_tx("ERR:INVALID_FILENAME\r\n");
        return;
    }

    FILE *fp = fopen(fpath, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open for write: %s", fpath);
        usb_tx("ERR:FILE_SAVE_FAILED\r\n");
        return;
    }

    if (recv_binary_frame(fp)) {
        fclose(fp);
        usb_tx("OK:FILE_SAVED\r\n");
    } else {
        fclose(fp);
        usb_tx("ERR:FILE_SAVE_FAILED\r\n");
    }
}


//========================================================================================
// UsbSerial task
//========================================================================================

void UsbSerial(void *pvParameters) {

    uart_config_t cfg = {
        .baud_rate  = (int)gusbbaud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };

    // UART0 is also used by the IDF console; delete existing driver first
    uart_driver_delete(USB_UART_PORT);
    ESP_ERROR_CHECK(uart_param_config(USB_UART_PORT, &cfg));
    // No event queue — synchronous reads only (like LISYclock)
    ESP_ERROR_CHECK(uart_driver_install(USB_UART_PORT,
                                        USB_RXBUF_SZ, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "USB serial ready @ %u baud", (unsigned)gusbbaud);

    // Announce readiness proactively
    usb_tx("OK:READY\r\n");

    char line[USB_LINE_MAX];
    int  idx = 0;

    while (1) {
        uint8_t b;
        if (usb_read_byte(&b, 100) != 1) continue;

        // Handshake: 0x55 as first byte on an empty line
        if (b == 0x55 && idx == 0) {
            usb_tx("OK:READY\r\n");
            continue;
        }

        if (b == '\r') continue;

        if (b == '\n') {
            line[idx] = '\0';
            if (idx > 0) {
                // ---- FILE:UPLOAD (blocking: reads entire BINDATA frame) ----
                if (strncmp(line, "FILE:UPLOAD=", 12) == 0) {
                    handle_file_upload(line + 12);
                }
                // ---- FILE:DOWNLOAD ----
                else if (strncmp(line, "FILE:DOWNLOAD=", 14) == 0) {
                    send_file_bindata(line + 14);
                }
                // ---- FILE:LIST ----
                else if (strcmp(line, "FILE:LIST") == 0) {
                    send_file_list();
                }
                // ---- FILE:DELETE ----
                else if (strncmp(line, "FILE:DELETE=", 12) == 0) {
                    esp_err_t r = cmdapi_file_delete(line + 12);
                    usb_tx(r == ESP_OK ? "OK:FILE_DELETED\r\n" : "ERR:DELETE_FAILED\r\n");
                }
                // ---- FILE:RENAME ----
                else if (strncmp(line, "FILE:RENAME=", 12) == 0) {
                    char *comma = strchr(line + 12, ',');
                    if (comma) {
                        *comma = '\0';
                        esp_err_t r = cmdapi_file_rename(line + 12, comma + 1);
                        usb_tx(r == ESP_OK ? "OK:FILE_RENAMED\r\n" : "ERR:RENAME_FAILED\r\n");
                    } else {
                        usb_tx("ERR:INVALID_ARGS\r\n");
                    }
                }
                // ---- SET:TIME=<unix_seconds> ----
                else if (strncmp(line, "SET:TIME=", 9) == 0) {
                    long t = atol(line + 9);
                    if (t > 0) {
                        struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = 0 };
                        settimeofday(&tv, NULL);
                        ESP_LOGI(TAG, "System time set to %ld", t);
                    }
                    usb_tx("OK:TIME_SET\r\n");
                }
                // ---- WIFI:STATUS ----
                else if (strcmp(line, "WIFI:STATUS") == 0) {
                    char resp[96];
                    if (!gconf[CONF_WIFI_ENABLE]) {
                        snprintf(resp, sizeof(resp), "OK:WIFI_STATUS=disabled\r\n");
                    } else if (gwifi_ssid[0] == '\0') {
                        snprintf(resp, sizeof(resp), "OK:WIFI_STATUS=no_ssid\r\n");
                    } else if (wifi_is_connected()) {
                        char ip[20] = {0};
                        if (wifi_get_ip_str(ip, sizeof(ip)) == ESP_OK && ip[0]) {
                            snprintf(resp, sizeof(resp),
                                     "OK:WIFI_STATUS=connected,ip=%s,ssid=%s\r\n", ip, gwifi_ssid);
                        } else {
                            snprintf(resp, sizeof(resp),
                                     "OK:WIFI_STATUS=connected,ssid=%s\r\n", gwifi_ssid);
                        }
                    } else {
                        snprintf(resp, sizeof(resp),
                                 "OK:WIFI_STATUS=disconnected,ssid=%s\r\n", gwifi_ssid);
                    }
                    usb_tx(resp);
                }
                // ---- REBOOT ----
                else if (strcmp(line, "REBOOT") == 0) {
                    usb_tx("OK:REBOOTING\r\n");
                    vTaskDelay(pdMS_TO_TICKS(120));
                    cmdapi_reboot();
                }
                // ---- Legacy play command: "p <num>" ----
                else if (line[0] == 'p' && line[1] == ' ') {
                    int num = atoi(line + 2);
                    if (num > 0) {
                        Rxcmd cmd = { .cmd = 'p', .arg = (uint16_t)num };
                        xStreamBufferSend(xpinevt, &cmd, sizeof(Rxcmd), 1);
                    }
                }
            }
            idx = 0;
            continue;
        }

        if (idx < USB_LINE_MAX - 1) line[idx++] = (char)b;
    }
}
