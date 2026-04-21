//----------------------------------------------------------------------------------------
//
// GPIO events, binary encoded, Bally -35 / Stern MPU
// Data bits A..E same wiring as Gottlieb System 80 (encevG80.c),
// but read is triggered by a separate strobe line on GPIO34 (positive edge).
// v0.1
//
//

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include <esp_system.h>
#include "esp_log.h"
#include "esp_check.h"
#include "soc/gpio_struct.h"
#include "rom/ets_sys.h"

// adjust
//#define DEBUG
#include "platform.h"
#include "pgpio.h"
#include "pwav.h"


#define ESP_INTR_FLAG_DEFAULT 0


//----------------------------------------------------------------------------------------
//
// Common section
//

extern StreamBufferHandle_t xpinevt; // stream to command the WAVplayer
static gptimer_handle_t gptimer = NULL;

static uint8_t ReadBinaryCode() {
    uint8_t sbyte = 0;
    if (gpio_get_level(GPIO_NUM_27)) sbyte |= 0b00000001; // LSBit
    if (gpio_get_level(GPIO_NUM_26)) sbyte |= 0b00000010;
    if (gpio_get_level(GPIO_NUM_25)) sbyte |= 0b00000100;
    if (gpio_get_level(GPIO_NUM_33)) sbyte |= 0b00001000;
    if (gpio_get_level(GPIO_NUM_32)) sbyte |= 0b00010000; // MSBit
    return sbyte;
}

//
// ISR on strobe (GPIO34, positive edge)
// Start a short one-shot timer so data lines have time to settle
// before we sample them in the alarm callback.
//
static void IsrStrobe(void* arg) {
    gptimer_set_raw_count(gptimer,0);
    gptimer_start(gptimer);
}

//
// Timer expiry: data is valid, read it and forward to WAVPlayer
//
static bool B35_alarm_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx) {
    gptimer_stop(timer);

    uint8_t sbyte = ReadBinaryCode();
    if (sbyte != 0) {
        Rxcmd xcmd;
        xcmd.cmd = 'p';     // cmd 'play sound file'
        xcmd.arg = sbyte;   // sound number
        xStreamBufferSend(xpinevt,&xcmd,sizeof(Rxcmd),1);
    }
    return false;
}


//----------------------------------------------------------------------------------------
//
// Task to handle strobe-triggered binary encoded events for Bally -35
//

void EncEventB35(void *pvParameters) {

    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000,   // 1 tick equals 1 microsecond
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = 10,                  // 10us settle delay after the strobe edge
        .flags.auto_reload_on_alarm = false,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = B35_alarm_cb,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));

    ESP_ERROR_CHECK(gptimer_enable(gptimer));

    // Data bits A..E: plain inputs, no interrupt
    gpio_config_t data_conf = {0};
    data_conf.mode = GPIO_MODE_INPUT;
    data_conf.intr_type = GPIO_INTR_DISABLE;
    data_conf.pin_bit_mask =
            (1ULL << GPIO_NUM_27)  // A (LSBit)
        |   (1ULL << GPIO_NUM_26)  // B
        |   (1ULL << GPIO_NUM_25)  // C
        |   (1ULL << GPIO_NUM_33)  // D
        |   (1ULL << GPIO_NUM_32); // E (MSBit)
    data_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    data_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&data_conf));

    // Strobe: positive-edge interrupt
    gpio_config_t strobe_conf = {0};
    strobe_conf.mode = GPIO_MODE_INPUT;
    strobe_conf.intr_type = GPIO_INTR_POSEDGE;
    strobe_conf.pin_bit_mask = (1ULL << GPIO_NUM_34);
    strobe_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    strobe_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&strobe_conf));

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(GPIO_NUM_34, IsrStrobe, (void*)GPIO_NUM_34);

    while(1) {
        vTaskDelay(1);
    }

}
