//
//
//   PWAVplayer includes
// 
//


#ifdef DEBUG
#define TP_SET() (GPIO.out_w1tc = 1UL << TESTPIN)
#define TP_CLR() (GPIO.out_w1ts = 1UL << TESTPIN)
//#define LED_ON()
//#define LED_OFF()
//#define LED_ON() (GPIO.out_w1tc = 1UL << TESTPIN)
//#define LED_OFF() (GPIO.out_w1ts = 1UL << TESTPIN)
#else
#define TP_SET()
#define TP_CLR()
//#define LED_ON() (GPIO.out_w1tc = 1UL << TESTPIN)
//#define LED_OFF() (GPIO.out_w1ts = 1UL << TESTPIN)
#endif


// interprocess communication between WAVPlayer and PinEvents
#define IP_BUF_SZ 8

// command structure sent over xStreamBuffer
typedef struct {
    uint16_t cmd;
    uint16_t arg;
} Rxcmd;

// Config entries
#define CONF_DAC        0
#define CONF_DAC_12         1   // dac=12
#define CONF_DAC_16         2   // dac=16
#define CONF_MIX        1
#define CONF_MIX_SUM        1   // mix=sum
#define CONF_MIX_DIV2       2   // mix=div2
#define CONF_MIX_SQRT       3   // mix=sqrt
#define CONF_EVT        2
#define CONF_EVT_NONE       1   // evt=none
#define CONF_EVT_FLAT       2   // evt=flat (new version)
#define CONF_EVT_BW11       3   // evt=bw11
#define CONF_EVT_BG80       4   // evt=bg80
#define CONF_EVT_FLAT0      5   // evt=flat0 (old version)
#define CONF_DEB        3       // debounce in ms
#define CONF_SER        4
#define CONF_SER_NONE       1   // ser=none
#define CONF_SER_I2C        2   // ser=i2c
#define CONF_SER_UART       3   // ser=uart
#define CONF_I2C_ADDR   5       // I2C slave address
#define CONF_RESTPD     6       // rest period in ms
#define CONF_MAX        7

