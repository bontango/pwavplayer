//----------------------------------------------------------------------------------------
//
// wifi.c -- STA WiFi init for pwavplayer
//
// Based on the LISYclock pattern (main/lisyclock.cpp): native ESP-IDF WiFi in
// STA mode, ~15 s timeout, modem-sleep disabled once associated.
//

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"

#include "pwav.h"
#include "wifi.h"

static const char *TAG = "WIFI";

extern uint16_t gconf[CONF_MAX];
extern char gwifi_ssid[];
extern char gwifi_pwd[];

static volatile bool s_connected = false;
static esp_netif_t *s_sta_netif = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) {
            ESP_LOGW(TAG, "WiFi disconnected");
            s_connected = false;
        }
    }
}

void wifi_init_sta(void) {
    if (!gconf[CONF_WIFI_ENABLE] || gwifi_ssid[0] == '\0') {
        ESP_LOGI(TAG, "WiFi disabled");
        return;
    }

    // NVS is required by the WiFi driver
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                               &wifi_event_handler, NULL);

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, gwifi_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, gwifi_pwd, sizeof(cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    ESP_LOGI(TAG, "Connecting to SSID \"%s\"…", gwifi_ssid);

    esp_netif_ip_info_t ip;
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK
            && ip.ip.addr != 0) {
            s_connected = true;
            char buf[16];
            esp_ip4addr_ntoa(&ip.ip, buf, sizeof(buf));
            ESP_LOGI(TAG, "WiFi connected, IP %s", buf);
            // Disable modem sleep for snappy HTTP response
            esp_wifi_set_ps(WIFI_PS_NONE);
            return;
        }
    }
    ESP_LOGW(TAG, "WiFi connect timeout");
}

bool wifi_is_connected(void) {
    return s_connected;
}

esp_err_t wifi_get_ip_str(char *out, size_t n) {
    if (!s_connected || !s_sta_netif) return ESP_FAIL;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK) return ESP_FAIL;
    esp_ip4addr_ntoa(&ip.ip, out, n);
    return ESP_OK;
}
