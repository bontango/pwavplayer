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
#define CONF_EVT_BY35       6   // evt=by35 (Bally -35, strobe-triggered)
#define CONF_DEB        3       // debounce in ms
#define CONF_SER        4
#define CONF_SER_NONE       1   // ser=none
#define CONF_SER_I2C        2   // ser=i2c
#define CONF_SER_UART       3   // ser=uart
#define CONF_I2C_ADDR   5       // I2C slave address
#define CONF_RESTPD     6       // rest period in ms
#define CONF_LOG        7
#define CONF_LOG_NO         1   // log=no (default)
#define CONF_LOG_YES        2   // log=yes — write events to log.txt
#define CONF_LOG_ONLY       3   // log=yes, but suppress audio playback
#define CONF_VOLV       8       // voice-volume scaling %, default 100
#define CONF_VOLS       9       // sound-volume scaling %, default 100
#define CONF_WIFI_ENABLE 10     // 0=off, 1=on
#define CONF_MAX        11

// Sound theme directory (subfolder of SD root containing all sound/group files)
#define NSTHEME 32

// Activity log (ring buffer in wavplayer.c, queried via HTTP /activity)
void activity_log_add(const char *msg);
// Returns newly-allocated malloc'd JSON; caller frees. since_seq < 0 = all.
// Format: {"next":<N>,"entries":[{"seq":N,"ts":ms,"msg":"..."}, ...]}
char *activity_log_get_json(long since_seq);

// Persistent event log (log.txt on SD).  No-op if log=no.
void event_log_open(void);
void event_log_add(const char *msg);

