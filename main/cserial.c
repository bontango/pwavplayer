//----------------------------------------------------------------------------------------
//
// Run UART/COM or I2C serial interface
//
// same 2 GPIO pins used for UART and I2C; therefore mutually exclusive
//


#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/i2c_slave.h"
#include "esp_log.h"

// adjust
//#define DEBUG
#include "platform.h"
#include "pgpio.h"
#include "pwav.h"

extern char *gversion; 
extern StreamBufferHandle_t xpinevt;


static char *glogmk = "SER";
#define LOG glogmk


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
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, TX_SCL, RX_SDA, -1, -1));
    // define level if uart plug is not inserted
    gpio_pullup_en(RX_SDA);
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, BUFSIZE, BUFSIZE, 20, &uart_event_queue, 0));
    
    while (1) {
        if (xQueueReceive(uart_event_queue,(void *)&event,(TickType_t)portMAX_DELAY)) {
            if (event.type == UART_DATA) {
                uint8_t dtmp[BUFSIZE];
                uart_read_bytes(UART_PORT_NUM,dtmp,event.size,portMAX_DELAY);
                dtmp[event.size] = 0;
//                ESP_LOGI(LOG, "[UART EVT]: %s",(char*)dtmp);
                CmdDispatch(dtmp);
                // echo back
                uart_write_bytes(UART_PORT_NUM,(const char*)dtmp,event.size);
            }
        }
    }
}


//----------------------------------------------------------------------------------------
//
// I2C Slave
//

typedef enum {
    I2C_SLAVE_EVT_RX,
    I2C_SLAVE_EVT_TX
} i2c_slave_event_t;

typedef struct i2c_context {
    QueueHandle_t event_queue;
    uint8_t msg[100];
    uint32_t mlen;
} I2Ccontext;

#if 0
static bool i2c_slave_request_cb(i2c_slave_dev_handle_t i2c_slave, const i2c_slave_request_event_data_t *evt_data, void *arg) {
    I2Ccontext *context = (I2Ccontext *)arg;
    i2c_slave_event_t evt = I2C_SLAVE_EVT_TX;
    BaseType_t xTaskWoken = 0;
    xQueueSendFromISR(context->event_queue, &evt, &xTaskWoken);
    return xTaskWoken;
}
#endif

static bool i2c_slave_receive_cb(i2c_slave_dev_handle_t i2c_slave, const i2c_slave_rx_done_event_data_t *evt_data, void *arg) {
    I2Ccontext *context = (I2Ccontext *)arg;
    i2c_slave_event_t evt = I2C_SLAVE_EVT_RX;
    BaseType_t xTaskWoken = 0;
    context->mlen = evt_data->length;
    for (int i = 0; i < evt_data->length; i++) context->msg[i] = evt_data->buffer[i];
    xQueueSendFromISR(context->event_queue, &evt, &xTaskWoken);
    return xTaskWoken;
}

void SerialI2C(void *pvParameters) {

    I2Ccontext context = {0};
    uint16_t i2c_saddr = *((uint16_t *)pvParameters);
    i2c_slave_dev_handle_t shandle;

    context.event_queue = xQueueCreate(16, sizeof(i2c_slave_event_t));
    if (!context.event_queue) {
        ESP_LOGE(LOG, "Creating queue failed");
        return;
    }

    i2c_slave_config_t sconfig = {
        .i2c_port = -1, // auto 
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = RX_SDA,
        .scl_io_num = TX_SCL,
        .send_buf_depth = 100,
        .receive_buf_depth = 100,
        .slave_addr = i2c_saddr,
        .intr_priority = 2,
    };

    ESP_ERROR_CHECK(i2c_new_slave_device(&sconfig, &shandle));
    i2c_slave_event_callbacks_t cbs = {
        .on_receive = i2c_slave_receive_cb,
//        .on_request = i2c_slave_request_cb,
    };
    ESP_ERROR_CHECK(i2c_slave_register_event_callbacks(shandle, &cbs, &context));

    while (1) {
        i2c_slave_event_t evt;
        if (xQueueReceive(context.event_queue, &evt, pdMS_TO_TICKS(1000)) == pdTRUE) {

            if (evt == I2C_SLAVE_EVT_RX) {
                switch (context.msg[0]) {
                case 2: // cmd 2 => echo msg
                    uint32_t wlen;
                    ESP_ERROR_CHECK(i2c_slave_write(shandle,context.msg,context.mlen,&wlen,1000));
                    if (wlen == 0) ESP_LOGE(LOG, "Write error or timeout");
                    break;
                case 3: { // cmd 3 => return board id & software version
                    uint8_t biv[10];
                    uint32_t wlen;
                    biv[0] = i2c_saddr;
                    biv[1] = biv[2] = 'A';
                    biv[3] = '1';
                    sscanf(gversion,"%hhu.%hhu.%hhu",&biv[4],&biv[5],&biv[6]);
                    ESP_ERROR_CHECK(i2c_slave_write(shandle,biv,7,&wlen,1000));
                    if (wlen == 0) ESP_LOGE(LOG, "Write error or timeout");
                    }
                    break;
                case 20: { // cmd 20 => play sound
                    Rxcmd xcmd;
                    xcmd.cmd = 'p';
                    xcmd.arg = (uint16_t)context.msg[1];
                    xStreamBufferSend(xpinevt,&xcmd,sizeof(Rxcmd),1); // message to task WAVPlayer, play sound file
                    }
                    break;
                default:
                    // nop
                    break;
                }
            }
            
            if (evt == I2C_SLAVE_EVT_TX) {
                // not used
            }

        }
    }
}

