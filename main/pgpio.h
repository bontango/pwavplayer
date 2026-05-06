//
//
//   GPIO numbers
//   
//   Assign symbolic names to GPIO numbers to improve portability
//


#ifdef ESP32_WROVER

// common
#define SD_CLK
#define SD_CMD
#define SD_DAT0
#define SD_DAT1  GPIO_NUM_4
#define SD_DAT2  GPIO_NUM_12
#define SD_DAT3  GPIO_NUM_13
#define DAC_MOSI GPIO_NUM_23
#define DAC_CLK  GPIO_NUM_18
#define DAC_CS   GPIO_NUM_5
#define RX_SDA   GPIO_NUM_21
#define TX_SCL   GPIO_NUM_22
#define UART_RX_PIN GPIO_NUM_36  // input-only pad, no internal pull-up

// On-board indicator LEDs (active LOW — driven against GND).
#define LED_D1   GPIO_NUM_21    // ON while at least one track is playing
#define LED_D2   GPIO_NUM_22    // ON while attract mode is active

#define TESTPIN  GPIO_NUM_0

// trace prototype
#define BW11T_PB7   GPIO_NUM_27    
#define BW11T_PB6   GPIO_NUM_26    
#define BW11T_PB5   GPIO_NUM_25    
#define BW11T_PB4   GPIO_NUM_33    
#define BW11T_PB3   GPIO_NUM_32    
#define BW11T_PB2   GPIO_NUM_35    
#define BW11T_PB1   GPIO_NUM_34    
#define BW11T_PB0   GPIO_NUM_39    
#define BW11T_CB1   GPIO_NUM_36
#define BW11T_CB2   GPIO_NUM_19

// cmd prototype
#define BW11C_PB7   GPIO_NUM_27
#define BW11C_PB6   GPIO_NUM_26
#define BW11C_PB5   GPIO_NUM_25
#define BW11C_PB4   GPIO_NUM_33
#define BW11C_PB3   GPIO_NUM_32
#define BW11C_PB2   GPIO_NUM_18
#define BW11C_PB1   GPIO_NUM_23
#define BW11C_PB0   GPIO_NUM_0
#define BW11C_CB1   GPIO_NUM_5
#define BW11C_RES   GPIO_NUM_19

#endif

#ifdef ESP32_S3

// common
#define SD_CLK      GPIO_NUM_35
#define SD_CMD      GPIO_NUM_36
#define SD_DAT0     GPIO_NUM_11
#define SD_DAT1     GPIO_NUM_12
#define SD_DAT2     GPIO_NUM_13
#define SD_DAT3     GPIO_NUM_14
#define DAC_MOSI    GPIO_NUM_21
#define DAC_CLK     GPIO_NUM_47
#define DAC_CS      GPIO_NUM_48
#define RX_SDA      GPIO_NUM_37
#define TX_SCL      GPIO_NUM_38

// Sys11 Sound Cmd Bus
#define BW11_PB7    GPIO_NUM_8
#define BW11_PB6    GPIO_NUM_18
#define BW11_PB5    GPIO_NUM_17
#define BW11_PB4    GPIO_NUM_16
#define BW11_PB3    GPIO_NUM_15
#define BW11_PB2    GPIO_NUM_7
#define BW11_PB1    GPIO_NUM_6
#define BW11_PB0    GPIO_NUM_5
#define BW11_CB1    GPIO_NUM_9
#define BW11_CB2    GPIO_NUM_10
#define BW11_RES    GPIO_NUM_4
#define BW11_PB0_T  GPIO_NUM_43
#define BW11_PB1_T  GPIO_NUM_44
#define BW11_PB2_T  GPIO_NUM_1
#define BW11_VOL    GPIO_NUM_40

#define TESTPIN     GPIO_NUM_0

// sound card TAS5411 special
#define BW11S_SELF  GPIO_NUM_2

// sound card TPA3125 special
#define BW11O_SELF  GPIO_NUM_45
#define BW11_P_CS   GPIO_NUM_2
#define BW11_P_CLK  GPIO_NUM_42
#define BW11_P_DAT  GPIO_NUM_41

// cmd card special
#define BW11_ADC_CLK GPIO_NUM_47
#define BW11_ADC_DAT GPIO_NUM_21
#define BW11_ADC_CS  GPIO_NUM_48

#endif
