//----------------------------------------------------------------------------------------
//
// Sound events, binary encoded, Williams System 11
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



//----------------------------------------------------------------------------------------
//
// Task to handle binary encoded events for Williams System 11
//
// => this is not the final code
//

#if 1
void EncEventW11(void *pvParameters) {
    // tbd
    while(1) vTaskDelay(100);
}
#endif

// record interface events
#if 0
#include "BW11trace.c"
void EncEventW11(void *pvParameters) {
    BW11trace(pvParameters);
}
#endif

// command sound card
#if 0
#include "BW11cmd.c"
void EncEventW11(void *pvParameters) {
    BW11cmd(pvParameters);
}
#endif

