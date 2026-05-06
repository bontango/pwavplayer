//----------------------------------------------------------------------------------------
//
// Run UART/COM serial interface (RX only, GPIO 36)
//


#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

// adjust
//#define DEBUG
#include "platform.h"
#include "pgpio.h"
#include "pwav.h"

extern char *gversion; 
extern StreamBufferHandle_t xpinevt;




//----------------------------------------------------------------------------------------
//
// COM
//

static void CmdDispatch(uint8_t *msg) {
    char cmd[22];
    int arg = 0;
    cmd[0] = 0;
    int n = sscanf((char *)msg,"%s %d",cmd,&arg);
    if ((n == 2) && (strcmp(cmd,"p") == 0)) { // 'p 19' => play soundfile 19
        Rxcmd xcmd;
        xcmd.cmd = 'p';
        xcmd.arg = arg;
        xStreamBufferSend(xpinevt,&xcmd,sizeof(Rxcmd),1); // message to task WAVPlayer, play sound file
    }
}


#define UART_PORT_NUM UART_NUM_2
#define UART_BAUD_RATE 115200
#define BUFSIZE 1024

void SerialUART(void *pvParameters) {
    
    QueueHandle_t uart_event_queue;
    uart_event_t event;
    
    // configure parameters of UART driver, assign pins and install the driver
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, BUFSIZE, 0, 20, &uart_event_queue, 0));
    
    while (1) {
        if (xQueueReceive(uart_event_queue,(void *)&event,(TickType_t)portMAX_DELAY)) {
            if (event.type == UART_DATA) {
                uint8_t dtmp[BUFSIZE];
                uart_read_bytes(UART_PORT_NUM,dtmp,event.size,portMAX_DELAY);
                dtmp[event.size] = 0;
//                ESP_LOGI(LOG, "[UART EVT]: %s",(char*)dtmp);
                CmdDispatch(dtmp);
            }
        }
    }
}

