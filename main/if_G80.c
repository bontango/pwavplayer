//----------------------------------------------------------------------------------------
//
// GPIO events, binary encoded, Gottlieb 1/80/80A/80B
// System 1 to be checked!
// v0.3
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

// Gottlieb Sys 80 audio board is based on ESP32 WROVER
#ifdef ESP32_WROVER

#define ESP_INTR_FLAG_DEFAULT 0


//----------------------------------------------------------------------------------------
//
// Common section
//

extern StreamBufferHandle_t xpinevt; // stream to command the WAVplayer
static gptimer_handle_t gptimer = NULL;
static uint8_t intrdis = 1; // interrupt enabled/disabled status; avoid too many system calls in main loop
    
static void EnableInterrupts() {
    gpio_intr_enable(GPIO_NUM_27);
    gpio_intr_enable(GPIO_NUM_26);
    gpio_intr_enable(GPIO_NUM_25);
    gpio_intr_enable(GPIO_NUM_33);
//    gpio_intr_enable(GPIO_NUM_32); // no INT for sound 16
    intrdis = 0;
}

static void DisableInterrupts() {
    gpio_intr_disable(GPIO_NUM_27);
    gpio_intr_disable(GPIO_NUM_26);
    gpio_intr_disable(GPIO_NUM_25);
    gpio_intr_disable(GPIO_NUM_33);
//    gpio_intr_disable(GPIO_NUM_32); // no INT for sound 16
    intrdis = 1;
}

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
// ISR
//
static void IsrEncoded(void* arg) {
//  TP_SET();
    // we dont know how many interrupts we shall receive, at least 1, at most 5
    // every interrupt will reset the timer
    // timer will only expire after the last interrupt
    // expiry time must be longer then the longest interval between the interrupts
    // see .alarm_count
    gptimer_set_raw_count(gptimer,0);
    gptimer_start(gptimer);
//  TP_CLR();
}

//
// Timer expiry
//
static bool G80_alarm_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx) {
    
//  TP_SET();
    gptimer_stop(timer);
    DisableInterrupts();
    
    // all bits A..E have been set
    // read the binary code
    uint8_t sbyte = ReadBinaryCode();
    if (sbyte != 0) {
        // play sound
        Rxcmd xcmd;
        xcmd.cmd = 'p';     // cmd 'play sound file'
        xcmd.arg = sbyte;   // sound number
        xStreamBufferSend(xpinevt,&xcmd,sizeof(Rxcmd),1); // message to task WAVPlayer, play sound file
    }
    
//  TP_CLR();
    return false;
}


//----------------------------------------------------------------------------------------
//
// Task to handle binary encoded events for Gottlieb 1/80/80B
//

void G80If(void *pvParameters) {

    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000,   // 1 tick equals 1 microsecond
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = 20,                  // 20us delay after the last interrupt to read the binary encoded value
                                            // carfeful: time must not expire before the last interrupt occurs! 
        .flags.auto_reload_on_alarm = false,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = G80_alarm_cb,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));

    // enable the timer
    ESP_ERROR_CHECK(gptimer_enable(gptimer));

	// input bit A..E, Gottlieb binary encoded event
    // interrupt positive edge
    // only bit A..D can cause an interrupt, same ISR for all
    gpio_config_t io_conf = {0};
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.pin_bit_mask =
            (1ULL << GPIO_NUM_27)  // A (LSBit)
        |   (1ULL << GPIO_NUM_26)  // B
        |   (1ULL << GPIO_NUM_25)  // C
        |   (1ULL << GPIO_NUM_33)  // D
        |   (1ULL << GPIO_NUM_32); // E (MSBit)
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(GPIO_NUM_27, IsrEncoded, (void*)GPIO_NUM_27);
    gpio_isr_handler_add(GPIO_NUM_26, IsrEncoded, (void*)GPIO_NUM_26);
    gpio_isr_handler_add(GPIO_NUM_25, IsrEncoded, (void*)GPIO_NUM_25);
    gpio_isr_handler_add(GPIO_NUM_33, IsrEncoded, (void*)GPIO_NUM_33);
//    gpio_isr_handler_add(GPIO_NUM_32, IsrEncoded, (void*)GPIO_NUM_32); // no INT for sound 16
    EnableInterrupts();

    while(1) {
        vTaskDelay(1); // 1 tick equals 5ms (tick rate is 200Hz)
        if (intrdis && (ReadBinaryCode() == 0)) EnableInterrupts();
    }

}


#endif // ESP32_WROVER
