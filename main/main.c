//----------------------------------------------------------------------------------------
//
// Main
//
// This app_main() for ESP32_WROVER and its modules
//

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include <esp_system.h>
#include "esp_log.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "driver/gptimer.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "soc/gpio_struct.h"
#include "rom/ets_sys.h"
#include "esp_app_desc.h"

// adjust
//#define DEBUG
#include "platform.h"
#include "pgpio.h"
#include "pwav.h"
#include "wifi.h"
#include "httpserver.h"

// Firmware file name
#define FIRMWARE_NAME "update.bin"

// Config file name
#define CONFIG_NAME "config.txt"

#define CORE_0 0
#define CORE_1 1

extern esp_err_t MountSDCard(void);
extern void InitConfig(void);
extern void ReadConfig(char *fname);
extern char mount_point[];
extern void CheckFWUpdate(char *fname);
extern void WAVPlayer(void *pvParameters);
extern void WAVDummy(void *pvParameters);
extern void SerialUART(void *pvParameters);
extern void UsbSerial(void *pvParameters);
extern void FlatIf(void *pvParameters);
extern void FlatIf0(void *pvParameters);
extern void Sys11If(void *pvParameters);
extern void G80If(void *pvParameters);
extern void B35If(void *pvParameters);
extern void event_log_open(void);
extern StreamBufferHandle_t xpinevt;
extern uint16_t gconf[CONF_MAX];

void app_main(void) {
    char fpath0[30];

    if (MountSDCard() != ESP_OK) return;

    sprintf(fpath0, "%s/%s",mount_point,FIRMWARE_NAME);
    CheckFWUpdate(fpath0);
    
    InitConfig();
    sprintf(fpath0, "%s/%s",mount_point,CONFIG_NAME);
    ReadConfig(fpath0);

    // Open persistent event log on SD (no-op if log=no)
    event_log_open();

    xpinevt = xStreamBufferCreate(IP_BUF_SZ,1);

    // WiFi + HTTP server (runs in parallel to USB serial editor).
    // No-op if wifi_enable=no or SSID is empty.
    wifi_init_sta();
    if (wifi_is_connected()) {
        httpserver_start();
    }
    xTaskCreatePinnedToCore(&WAVPlayer, "WAVplayer", 8192, NULL, tskIDLE_PRIORITY+10, NULL, CORE_0);
//    xTaskCreatePinnedToCore(&WAVDummy, "WAVdummy", 4096, NULL, tskIDLE_PRIORITY+10, NULL, CORE_0);

    // Config-editor USB serial (UART0) always available, independent of ser= setting
    xTaskCreatePinnedToCore(&UsbSerial, "UsbSerial", 8192, NULL, (tskIDLE_PRIORITY + 2), NULL, CORE_1);

    if (gconf[CONF_SER] == CONF_SER_UART) {
        xTaskCreatePinnedToCore(&SerialUART, "SerialUART", 4096, NULL, (tskIDLE_PRIORITY + 2), NULL, CORE_1);
    }

    switch(gconf[CONF_EVT]) {
    case CONF_EVT_FLAT:
        xTaskCreatePinnedToCore(&FlatIf,  "FlatIf",  4096, NULL, (tskIDLE_PRIORITY + 3), NULL, CORE_1);
        break;
    case CONF_EVT_FLAT0:
        xTaskCreatePinnedToCore(&FlatIf0, "FlatIf0", 4096, NULL, (tskIDLE_PRIORITY + 3), NULL, CORE_1);
        break;
    case CONF_EVT_BW11:
        xTaskCreatePinnedToCore(&Sys11If, "Sys11If", 4096, NULL, (tskIDLE_PRIORITY + 3), NULL, CORE_1);
        break;
    case CONF_EVT_BG80:
        xTaskCreatePinnedToCore(&G80If,   "G80If",   4096, NULL, (tskIDLE_PRIORITY + 3), NULL, CORE_1);
        break;
    case CONF_EVT_BY35:
        xTaskCreatePinnedToCore(&B35If,   "B35If",   4096, NULL, (tskIDLE_PRIORITY + 3), NULL, CORE_1);
        break;
    default: break;
    }

}

