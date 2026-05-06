//----------------------------------------------------------------------------------------
//
//   Sound Cmd Interface for Williams System 11
//
//   Ported from upstream pwavplayer 0.9.6 (if_sys11.c).  ESP32-S3 only — on the
//   WROVER target this compiles to nothing and the symbol Sys11If is provided as
//   a stub below so the linker is satisfied.
//
//   (C) 2026 colrhon.org — CC BY-NC-SA 4.0
//

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "driver/gpio.h"
#include <esp_system.h>
#include "esp_log.h"
#include "esp_check.h"
#include "soc/gpio_struct.h"
#include "rom/ets_sys.h"

// adjust
#define DEBUG
#include "platform.h"
#include "pgpio.h"
#include "pwav.h"

#ifdef ESP32_S3

#define ESP_INTR_FLAG_DEFAULT 0
extern StreamBufferHandle_t xpinevt;

//----------------------------------------------------------------------------------------
//
// Fifo buffer
//

#define FF2_SIZE 16 // must be a value 2^n
#define FF2_MASK (FF2_SIZE-1)

typedef struct {
    uint32_t tick;  // OS tick time
    uint8_t pre;    // first byte, unknown meaning
    uint8_t cmd;    // MPU => Audio command
} Ccmd;

static struct Ff2 {
    Ccmd cent[FF2_SIZE];
    uint16_t read;   // points to oldest entry
    uint16_t write;  // points to next empty slot
} ff2 = {{}, 0, 0};

static inline uint8_t Ff2In(uint32_t tick, uint8_t pre, uint8_t cmd) {
    uint16_t next = ((ff2.write + 1) & FF2_MASK);
    if (ff2.read == next) return 0;
    ff2.cent[ff2.write].tick = tick;
    ff2.cent[ff2.write].pre = pre;
    ff2.cent[ff2.write].cmd = cmd;
    ff2.write = next;
    return 1;
}

static inline uint8_t Ff2Out(uint32_t *t, uint8_t *p, uint8_t *q) {
    if (ff2.read == ff2.write) return 0;
    *t = ff2.cent[ff2.read].tick;
    *p = ff2.cent[ff2.read].pre;
    *q = ff2.cent[ff2.read].cmd;
    ff2.read = (ff2.read+1) & FF2_MASK;
    return 1;
}

//----------------------------------------------------------------------------------------
//
// Read from Sound Cmd Interface
//

static uint8_t ReadSoundCmd() {
    // read the state of CPU=>Audio data interface, bit7..bit0 directly from GPIO.in
    uint8_t sbyte = 0;
    uint32_t gin = GPIO.in;
    if (gin & (1UL << BW11_PB7)) sbyte |= 0b10000000; // MSBit S1
    if (gin & (1UL << BW11_PB6)) sbyte |= 0b01000000;
    if (gin & (1UL << BW11_PB5)) sbyte |= 0b00100000;
    if (gin & (1UL << BW11_PB4)) sbyte |= 0b00010000;
    if (gin & (1UL << BW11_PB3)) sbyte |= 0b00001000;
    if (gin & (1UL << BW11_PB2)) sbyte |= 0b00000100;
    if (gin & (1UL << BW11_PB1)) sbyte |= 0b00000010;
    if (gin & (1UL << BW11_PB0)) sbyte |= 0b00000001; // LSBit S8
    return sbyte;
}

static void IsrMPU(void* arg) {
    // timing: 0..18us first byte stable, 19..44us second byte stable
    uint8_t spre = ReadSoundCmd();
    ets_delay_us(22);
    TP_SET();
    uint8_t scmd = ReadSoundCmd();
    TP_CLR();
    Ff2In((uint32_t)xTaskGetTickCount(), spre, scmd);
}


//----------------------------------------------------------------------------------------
//
// System 11 interface task
//

void Sys11If(void *pvParameters) {

    // configure test pin
    gpio_config_t io_conf5 = {0};
    io_conf5.intr_type = GPIO_INTR_DISABLE;
    io_conf5.mode = GPIO_MODE_OUTPUT;
    io_conf5.pin_bit_mask = (1ULL << TESTPIN);
    io_conf5.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf5.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf5));
    TP_CLR();

    // input bit0..7 on GPIO
    gpio_config_t io_conf2 = {0};
    io_conf2.mode = GPIO_MODE_INPUT;
    io_conf2.intr_type = GPIO_INTR_DISABLE;
    io_conf2.pin_bit_mask =
            (1ULL << BW11_PB7)
        |   (1ULL << BW11_PB6)
        |   (1ULL << BW11_PB5)
        |   (1ULL << BW11_PB4)
        |   (1ULL << BW11_PB3)
        |   (1ULL << BW11_PB2)
        |   (1ULL << BW11_PB1)
        |   (1ULL << BW11_PB0);
    io_conf2.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf2.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf2));

    // interrupt CB1 from MPU
    gpio_config_t io_conf = {0};
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.pin_bit_mask = (1ULL << BW11_CB1);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(BW11_CB1, IsrMPU, (void*)BW11_CB1);

    // output bit0..2 on GPIO (test driver lines)
    gpio_reset_pin(BW11_PB0_T);
    gpio_reset_pin(BW11_PB1_T);
    gpio_reset_pin(BW11_PB2_T);
    gpio_config_t io_conf1 = {0};
    io_conf1.mode = GPIO_MODE_OUTPUT;
    io_conf1.intr_type = GPIO_INTR_DISABLE;
    io_conf1.pin_bit_mask =
            (1ULL << BW11_PB2_T)
        |   (1ULL << BW11_PB1_T)
        |   (1ULL << BW11_PB0_T);
    io_conf1.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf1.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf1));
    gpio_set_level(BW11_PB0_T, 1);
    gpio_set_level(BW11_PB1_T, 1);
    gpio_set_level(BW11_PB2_T, 1);

    uint8_t block4 = 0;
    while (1) {
        Rxcmd xcmd;
        uint32_t tick;
        uint8_t spre, scmd;
        while (Ff2Out(&tick, &spre, &scmd)) {

            FILE *stm = fopen("/sdcard/sys11if.prot", "a");
            if (stm == NULL) ets_printf("open sys11if.prot failed\n");
            else {
                fprintf(stm, "%lu\t0x%02x\t0x%02x\n", tick, spre, scmd);
                fclose(stm);
            }

            switch (scmd) {
            case 0:
                xcmd.cmd = 'k'; xcmd.arg = 0;
                xStreamBufferSend(xpinevt, &xcmd, sizeof(Rxcmd), 1);
                block4 = 0;
                break;
            case 0x40:
                block4 = 1;
                xcmd.cmd = 'p'; xcmd.arg = scmd;
                xStreamBufferSend(xpinevt, &xcmd, sizeof(Rxcmd), 1);
                break;
            case 0x41: case 0x42: case 0x43: case 0x44:
                if (block4 == 0) {
                    xcmd.cmd = 'p'; xcmd.arg = scmd;
                    xStreamBufferSend(xpinevt, &xcmd, sizeof(Rxcmd), 1);
                }
                break;
            case 0x9f: // stops 0x9e (roulette wheel loop)
                xcmd.cmd = 'w'; xcmd.arg = 0x9e;
                xStreamBufferSend(xpinevt, &xcmd, sizeof(Rxcmd), 1);
                xcmd.cmd = 'p'; xcmd.arg = 0x9f;
                xStreamBufferSend(xpinevt, &xcmd, sizeof(Rxcmd), 1);
                break;
            default:
                xcmd.cmd = 'p'; xcmd.arg = scmd;
                xStreamBufferSend(xpinevt, &xcmd, sizeof(Rxcmd), 1);
                break;
            }
            ets_printf("Command 0x%X\n", scmd);
        }
        vTaskDelay(1);
    }
}

#else // ESP32_WROVER and any other target — Sys11If is not implemented; provide stub
void Sys11If(void *pvParameters) {
    while (1) vTaskDelay(100);
}
#endif
