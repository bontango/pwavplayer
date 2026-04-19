//----------------------------------------------------------------------------------------
//
// GPIO events (flat events 1..10)
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

extern StreamBufferHandle_t xpinevt;

typedef struct {
    gpio_num_t pin;   // to the input number corresponding GPIO number
    uint16_t debce;   // debounce
    uint16_t restpd;  // rest period (moving time frame)
    TickType_t last;  // last interrupt seen @ tick
    TickType_t until; // interrupt disabled until tick
} Tpa;

#define MAXPINS 10
static Tpa spl[MAXPINS+1] ={
    {0,0,0,0,0},            // not used
    {GPIO_NUM_27,5,60,0,0}, // pin #1
    {GPIO_NUM_26,5,60,0,0}, // pin #2
    {GPIO_NUM_25,5,60,0,0},
    {GPIO_NUM_33,5,60,0,0},
    {GPIO_NUM_32,5,60,0,0},
    {GPIO_NUM_35,5,60,0,0},
    {GPIO_NUM_34,5,60,0,0},
    {GPIO_NUM_39,5,60,0,0},
    {GPIO_NUM_36,5,60,0,0},
    {GPIO_NUM_19,5,60,0,0}  // pin #10
    };

static uint16_t FindSfNumber(gpio_num_t id) {
    for (uint16_t i = 1; i <= MAXPINS; i++) if (spl[i].pin == id) return i;  
    return 0;
}


//----------------------------------------------------------------------------------------
//
// Task to handle flat GPIO events, external inputs 1..10
//
// Old version, default up to Rel 0.9.1
// In config.txt use
//    evt=flat0
//    deb=10
//

static void IsrFlat0(void* arg) {

//  TP_SET();
    gpio_num_t id = (uint32_t)arg;
    uint16_t sf = FindSfNumber(id);
    if (sf == 0) return;
    
    TickType_t tc = xTaskGetTickCount();
    if (gpio_get_level(id) == 0) { // pin stat is low
        gpio_intr_disable(id);
        spl[sf].until = tc + pdMS_TO_TICKS(spl[sf].debce);
        Rxcmd xcmd;
        xcmd.cmd = 'p';
        xcmd.arg = sf;
        xStreamBufferSend(xpinevt,&xcmd,sizeof(Rxcmd),1); // message to task WAVPlayer, play sound file
    }
    spl[sf].last = tc;  // register last seen
//  TP_CLR();

}

void PinEvents0(void *pvParameters) {

    // set debounce value (same value for all pins)
    for (uint16_t i = 1; i <= MAXPINS; i++) spl[i].debce = *((uint16_t *)pvParameters);

    gpio_config_t io_conf = {0};
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask =
          (1ULL << GPIO_NUM_27)
        | (1ULL << GPIO_NUM_26)
        | (1ULL << GPIO_NUM_25)
        | (1ULL << GPIO_NUM_33)
        | (1ULL << GPIO_NUM_32)
        | (1ULL << GPIO_NUM_35)
        | (1ULL << GPIO_NUM_34)
        | (1ULL << GPIO_NUM_39)
        | (1ULL << GPIO_NUM_36)
        | (1ULL << GPIO_NUM_19);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE; // using external pullup 10kOhm
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(GPIO_NUM_27, IsrFlat0, (void*)GPIO_NUM_27);
    gpio_isr_handler_add(GPIO_NUM_26, IsrFlat0, (void*)GPIO_NUM_26);
    gpio_isr_handler_add(GPIO_NUM_25, IsrFlat0, (void*)GPIO_NUM_25);
    gpio_isr_handler_add(GPIO_NUM_33, IsrFlat0, (void*)GPIO_NUM_33);
    gpio_isr_handler_add(GPIO_NUM_32, IsrFlat0, (void*)GPIO_NUM_32);
    gpio_isr_handler_add(GPIO_NUM_35, IsrFlat0, (void*)GPIO_NUM_35);
    gpio_isr_handler_add(GPIO_NUM_34, IsrFlat0, (void*)GPIO_NUM_34);
    gpio_isr_handler_add(GPIO_NUM_39, IsrFlat0, (void*)GPIO_NUM_39);
    gpio_isr_handler_add(GPIO_NUM_36, IsrFlat0, (void*)GPIO_NUM_36);
    gpio_isr_handler_add(GPIO_NUM_19, IsrFlat0, (void*)GPIO_NUM_19);

    while(1) {
        vTaskDelay(1);
        TickType_t tc = xTaskGetTickCount();
        for (uint16_t i = 1; i <= MAXPINS; i++) {
            if (spl[i].until > 0 && spl[i].until <= tc) {
                spl[i].until = 0;
                gpio_intr_enable(spl[i].pin);
            }
        }
    }
}

//----------------------------------------------------------------------------------------
//
// Task to handle flat GPIO events, external inputs 1..10
//
// New version, starting with Rel 0.9.2
// In config.txt use
//    evt=flat
//    deb=10
//    rpd=40
//

static void IsrFlat(void* arg) {

//  TP_SET();
    gpio_num_t id = (uint32_t)arg;
    uint16_t sf = FindSfNumber(id);
    if (sf == 0) return;
    
    TickType_t tc = xTaskGetTickCount();

    // check rest period
    if ((tc - spl[sf].last) < spl[sf].restpd) {
        spl[sf].last = tc;  // move last seen forward
        return;
    }

    if (gpio_get_level(id) == 0) { // pin stat is low
        gpio_intr_disable(id);
        spl[sf].until = tc + spl[sf].debce;
        Rxcmd xcmd;
        xcmd.cmd = 'p';
        xcmd.arg = sf;
        xStreamBufferSend(xpinevt,&xcmd,sizeof(Rxcmd),1); // message to task WAVPlayer, play sound file
    }
    spl[sf].last = tc;  // register last seen
//  TP_CLR();

}

void PinEvents(void *pvParameters) {

    // set debounce value
    // set rest period
    for (uint16_t i = 1; i <= MAXPINS; i++) {
        spl[i].debce = pdMS_TO_TICKS(((uint16_t *)pvParameters)[0]);
        spl[i].restpd = pdMS_TO_TICKS(((uint16_t *)pvParameters)[1]);
        }

    gpio_config_t io_conf = {0};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask =
          (1ULL << GPIO_NUM_27)
        | (1ULL << GPIO_NUM_26)
        | (1ULL << GPIO_NUM_25)
        | (1ULL << GPIO_NUM_33)
        | (1ULL << GPIO_NUM_32)
        | (1ULL << GPIO_NUM_35)
        | (1ULL << GPIO_NUM_34)
        | (1ULL << GPIO_NUM_39)
        | (1ULL << GPIO_NUM_36)
        | (1ULL << GPIO_NUM_19);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE; // using external pullup 10kOhm
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(GPIO_NUM_27, IsrFlat, (void*)GPIO_NUM_27);
    gpio_isr_handler_add(GPIO_NUM_26, IsrFlat, (void*)GPIO_NUM_26);
    gpio_isr_handler_add(GPIO_NUM_25, IsrFlat, (void*)GPIO_NUM_25);
    gpio_isr_handler_add(GPIO_NUM_33, IsrFlat, (void*)GPIO_NUM_33);
    gpio_isr_handler_add(GPIO_NUM_32, IsrFlat, (void*)GPIO_NUM_32);
    gpio_isr_handler_add(GPIO_NUM_35, IsrFlat, (void*)GPIO_NUM_35);
    gpio_isr_handler_add(GPIO_NUM_34, IsrFlat, (void*)GPIO_NUM_34);
    gpio_isr_handler_add(GPIO_NUM_39, IsrFlat, (void*)GPIO_NUM_39);
    gpio_isr_handler_add(GPIO_NUM_36, IsrFlat, (void*)GPIO_NUM_36);
    gpio_isr_handler_add(GPIO_NUM_19, IsrFlat, (void*)GPIO_NUM_19);

    while(1) {
        vTaskDelay(1);
        TickType_t tc = xTaskGetTickCount();
        for (uint16_t i = 1; i <= MAXPINS; i++) {
            if (spl[i].until > 0 && spl[i].until <= tc) {
                spl[i].until = 0;
                spl[i].last = tc;
                gpio_intr_enable(spl[i].pin);
            }
        }
    }
}

