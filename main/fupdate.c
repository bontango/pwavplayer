//----------------------------------------------------------------------------------------
//
// Firmware update from SD card
//
//
// based on https://esp32.com/viewtopic.php?t=19364
//
// Unsolved issues:
// --- 0x40080400: _invalid_pc_placeholder at /data/sync/esp/esp-idf/components/xtensa/xtensa_vectors.S:2235
//     >>https://esp32.com/viewtopic.php?t=44433
//     >>The 'error' is the monitor program trying to be helpful, looking up the symbol for address 0x40080400. 
//     >>It's harmless and probably unrelated to the issue you're seeing.
//


#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <esp_system.h>
#include "esp_log.h"
#include "esp_ota_ops.h"

static char *gtag = "FUP";
#define TAG gtag

#define BUFFER_SIZE 4096


typedef struct binary_data_t {
    unsigned long size;
    unsigned long remaining_size;
    void *data;
} binary_data_t;

static size_t fpread(void *buffer, size_t size, size_t nitems, size_t offset, FILE *fp) {
    if (fseek(fp, offset, SEEK_SET) != 0)
        return 0;
    return fread(buffer, size, nitems, fp);
}

// update partition, test and switch boot vector
//
void CheckFWUpdate(char *fname) {

    FILE *file = fopen(fname,"rb");
    if (file == NULL) {
        ESP_LOGI(TAG, "No new firmware found");
        return;
    }

    ESP_LOGI(TAG, "Update firmware: %s",fname);
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s - Update partition: %s", running_partition->label, update_partition->label);

    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
//    ESP_LOGI(TAG, "esp_ota_begin result = %d", err);
    binary_data_t data;

    // get file length
    fseek(file, 0, SEEK_END);
    data.size = ftell(file);
    data.remaining_size = data.size;
    fseek(file, 0, SEEK_SET);
    ESP_LOGI(TAG, "Firmware file size %lu", data.size);

    data.data = (char *) malloc(BUFFER_SIZE);
    while (data.remaining_size > 0) {
        size_t size = data.remaining_size <= BUFFER_SIZE ? data.remaining_size : BUFFER_SIZE;
        fpread(data.data, size, 1, data.size - data.remaining_size, file);
        err = esp_ota_write(update_handle, data.data, size);
        if (data.remaining_size <= BUFFER_SIZE) break;
        data.remaining_size -= BUFFER_SIZE;
    }
    fclose(file);
    
//    ESP_LOGI(TAG, "ota result = %d", err);
    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        }
        ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
        return;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        return;
    }

    // rename firmware file
    char newname[40];
    sprintf(newname,"%s_x",fname);
    rename(fname,newname);

    ESP_LOGI(TAG, "Prepare to restart system..");
    esp_restart();
}


