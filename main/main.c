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
extern void SerialI2C(void *pvParameters);
extern void UsbSerial(void *pvParameters);
extern void PinEvents(void *pvParameters);
extern void PinEvents0(void *pvParameters);
extern void EncEventW11(void *pvParameters);
extern void EncEventG80(void *pvParameters);
extern void EncEventB35(void *pvParameters);
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

    xpinevt = xStreamBufferCreate(IP_BUF_SZ,1);
    xTaskCreatePinnedToCore(&WAVPlayer, "WAVplayer", 8192, NULL, tskIDLE_PRIORITY+10, NULL, CORE_0);
//    xTaskCreatePinnedToCore(&WAVDummy, "WAVdummy", 4096, NULL, tskIDLE_PRIORITY+10, NULL, CORE_0);

    // Config-editor USB serial (UART0) always available, independent of ser= setting
    xTaskCreatePinnedToCore(&UsbSerial, "UsbSerial", 8192, NULL, (tskIDLE_PRIORITY + 2), NULL, CORE_1);

    switch (gconf[CONF_SER]) {
    case CONF_SER_UART:
        xTaskCreatePinnedToCore(&SerialUART, "SerialUART", 4096, NULL, (tskIDLE_PRIORITY + 2), NULL, CORE_1);
        break;
    case CONF_SER_I2C:
        xTaskCreatePinnedToCore(&SerialI2C, "SerialI2C", 4096, &(gconf[CONF_I2C_ADDR]), (tskIDLE_PRIORITY + 2), NULL, CORE_1);
        break;
    default: break;
    }

    switch(gconf[CONF_EVT]) {
    case CONF_EVT_FLAT: {
        uint16_t garg[2];
        garg[0] = gconf[CONF_DEB];
        garg[1] = gconf[CONF_RESTPD];
        xTaskCreatePinnedToCore(&PinEvents, "PinEvents", 4096, garg, (tskIDLE_PRIORITY + 3), NULL, CORE_1);
        }
        break;
    case CONF_EVT_FLAT0:
        xTaskCreatePinnedToCore(&PinEvents0, "PinEvents0", 4096, &(gconf[CONF_DEB]), (tskIDLE_PRIORITY + 3), NULL, CORE_1);
        break;
    case CONF_EVT_BW11:
        xTaskCreatePinnedToCore(&EncEventW11, "EncEventW11", 4096, NULL, (tskIDLE_PRIORITY + 3), NULL, CORE_1);
        break;
    case CONF_EVT_BG80:
        xTaskCreatePinnedToCore(&EncEventG80, "EncEventG80", 4096, NULL, (tskIDLE_PRIORITY + 3), NULL, CORE_1);
        break;
    case CONF_EVT_BY35:
        xTaskCreatePinnedToCore(&EncEventB35, "EncEventB35", 4096, NULL, (tskIDLE_PRIORITY + 3), NULL, CORE_1);
        break;
    default: break;
    }

}

