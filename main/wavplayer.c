//----------------------------------------------------------------------------------------------------
//
//   PWAVplayer
//
//   A polyphone WAV player based on ESP32
//
//   (C) 2025 colrhon.org
//   This program is released under the Creative Commons Public License, CC BY-NC-SA 4.0
// 
//   Firmware development with IDF-ESP, hardware is LilyGo TTGO T8 (ESP32-WROVER-E) Board, V1.8
//   Nov 2025: Ported to ESP32-S3
//
//   Some terminology used:
//   A sound file is a file containing a header and audio data.
//   A track is a sound file being currently played, possibly together with other tracks.
//   The mixer takes of each track the foremost sound sample and mixes them to become a DAC value.
//   The fifo-buffer holds DAC values; it is being fed by the mixer while being unfed by the feeder.
//   The feeder periodically sends a DAC value to the DAC.
//

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include <esp_system.h>
#include "esp_log.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "driver/gptimer.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "soc/gpio_struct.h"
#include "rom/ets_sys.h"
#include "esp_app_desc.h"
#include "esp_random.h"

// adjust
#define DEBUG
#include "platform.h"
#include "pgpio.h"
#include "pwav.h"

// Version
#define VERSION "1.0.8"
char *gversion = VERSION;

// ---------------------------------------------------------------------------
// Activity log — small ring buffer of recent sound-card activity, exposed
// over the HTTP API (/activity) for the web editor Debug tab.  USB transport
// does not consume this; USB users see the same information in the regular
// serial console via ets_printf().
// ---------------------------------------------------------------------------
#define ACT_LOG_SIZE   64
#define ACT_LOG_MSGLEN 56
typedef struct {
    uint32_t seq;
    uint32_t ts_ms;
    char     msg[ACT_LOG_MSGLEN];
} ActEntry;
static ActEntry  act_log[ACT_LOG_SIZE];
static uint32_t  act_next_seq = 1;           // next seq to assign (0 = unused)
static portMUX_TYPE act_mux = portMUX_INITIALIZER_UNLOCKED;

void activity_log_add(const char *msg) {
    if (!msg) return;
    uint32_t ts = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    portENTER_CRITICAL(&act_mux);
    uint32_t seq = act_next_seq++;
    ActEntry *e = &act_log[(seq - 1) % ACT_LOG_SIZE];
    e->seq = seq;
    e->ts_ms = ts;
    size_t i = 0;
    while (msg[i] && i < ACT_LOG_MSGLEN - 1) { e->msg[i] = msg[i]; i++; }
    e->msg[i] = '\0';
    portEXIT_CRITICAL(&act_mux);
}

char *activity_log_get_json(long since_seq) {
    // Snapshot under lock, then format outside.
    ActEntry snap[ACT_LOG_SIZE];
    uint32_t next;
    portENTER_CRITICAL(&act_mux);
    next = act_next_seq;
    for (int i = 0; i < ACT_LOG_SIZE; i++) snap[i] = act_log[i];
    portEXIT_CRITICAL(&act_mux);

    // Oldest still-retained seq:
    uint32_t oldest = (next > ACT_LOG_SIZE) ? next - ACT_LOG_SIZE : 1;
    uint32_t start  = (since_seq < 0) ? oldest
                    : ((uint32_t)since_seq + 1 < oldest ? oldest : (uint32_t)since_seq + 1);

    size_t cap = 256 + ACT_LOG_SIZE * (ACT_LOG_MSGLEN + 48);
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t len = 0;
    len += snprintf(out + len, cap - len, "{\"next\":%u,\"entries\":[", (unsigned)next);
    bool first = true;
    for (uint32_t s = start; s < next; s++) {
        ActEntry *e = &snap[(s - 1) % ACT_LOG_SIZE];
        if (e->seq != s) continue;
        // Escape backslash and quote in msg
        char esc[ACT_LOG_MSGLEN * 2 + 1];
        size_t j = 0;
        for (size_t i = 0; e->msg[i] && j < sizeof(esc) - 2; i++) {
            char c = e->msg[i];
            if (c == '"' || c == '\\') esc[j++] = '\\';
            if ((unsigned char)c < 0x20) c = ' ';
            esc[j++] = c;
        }
        esc[j] = '\0';
        int n = snprintf(out + len, cap - len,
                         "%s{\"seq\":%u,\"ts\":%u,\"msg\":\"%s\"}",
                         first ? "" : ",",
                         (unsigned)e->seq, (unsigned)e->ts_ms, esc);
        if (n < 0 || (size_t)n >= cap - len) break;
        len += n;
        first = false;
    }
    len += snprintf(out + len, cap - len, "]}");
    return out;
}

// Attract mode state (configured by *.atr file on SD)
static uint16_t  attract_group_id     = 0;     // 0 = disabled
static uint32_t  attract_timeout_ms   = 0;     // TT * 60000
static uint32_t  attract_interval_ms  = 0;     // EE * 60000
static TickType_t attract_last_activity = 0;
static TickType_t attract_last_play     = 0;
static bool      attract_active       = false;
static bool      attract_playing_now  = false;

// Local defines
#define MAX_INT16 (32767)
#define MIN_INT16 (-32768)
#define MAX_UINT16 (65535)
#define FPATHLEN 200
#define FAT_MAX_FILES 12  // max number of open files, see mount configuration for FatFS

// interprocess communication between WAVPlayer and PinEvents
#define IP_BUF_SZ 8
StreamBufferHandle_t xpinevt;

static char *glogmk = "WAV";
#define LOG glogmk

// Special ids
#define ID_VERSION  11111
#define ID_I_GROUP  11112
#define ID_IL_GROUP 11113

// Statistics
static struct stac {
    uint32_t isrodac; // FifoOut delivered a value
    uint32_t isrmiss; // FifoOut did not deliver a value
    uint32_t mixxcnt; // Mixer mixed at least 1 sample
} stac;

// Config data
uint16_t gconf[CONF_MAX];
uint32_t gusbbaud;

// Sound theme directory (subdir of mount_point holding all sound/group files)
char gconfsd[NSTHEME + 1] = "orgsnd";

// WiFi credentials (strings — kept out of gconf[] which is uint16_t)
#define WIFI_STR_MAX 65
char gwifi_ssid[WIFI_STR_MAX] = {0};
char gwifi_pwd[WIFI_STR_MAX]  = {0};

// Sound file descriptor
#define NSATTR 4
typedef struct sfile {
    struct sfile *next;
    uint16_t id;
    uint8_t attr[NSATTR];
    uint16_t vol;
    char fpath[FPATHLEN+1];
} Sfile;

// Sound group member
typedef struct gmember {
    struct gmember *next;
    uint16_t id; // id of sound
} Gmember;

// Sound group descriptor
typedef struct sgroup {
    struct sgroup *next;
    uint16_t id;
    uint8_t attr; // m (=1) or r (=2) flag
    uint8_t nom;  // number of members
    uint8_t nmp;  // next member to be played
    Gmember *first;
    char fname[FPATHLEN+1];
} Sgroup;

// Running track
#define BUFSZ 32 // best buffer size: 16 < BUFSZ < 100
#define NMODES 1
typedef struct track {
    struct track *next;
    int fh;         // file handle
    uint32_t tcnt;  // total samples
    uint32_t rcnt;  // samples remaining
    uint32_t stpos; // start position of pcm data in file
    int16_t buf[BUFSZ];
    uint16_t bufl;  // buffer fill level
    uint16_t bidx;  // buffer index 
    Sfile *sf;
    char mode[NMODES]; // 0=delete
} Track;


// VFS FAT32
const char mount_point[] = "/sdcard";


//----------------------------------------------------------------------------------------
//
// Configuration data
//

// read config file and override default values
//
// trim leading/trailing whitespace and surrounding quotes, write into dst (size n)
static void cfg_trim_copy(const char *src, char *dst, size_t n) {
    while (*src == ' ' || *src == '\t') src++;
    size_t L = strlen(src);
    while (L > 0 && (src[L-1]=='\r' || src[L-1]=='\n' || src[L-1]==' ' || src[L-1]=='\t')) L--;
    if (L >= 2 && ((src[0]=='"' && src[L-1]=='"') || (src[0]=='\'' && src[L-1]=='\''))) {
        src++; L -= 2;
    }
    if (L >= n) L = n - 1;
    memcpy(dst, src, L);
    dst[L] = '\0';
}

void ReadConfig(char *fname) {
    FILE *fp;
    if ((fp = fopen(fname,"r")) == NULL) {
        ESP_LOGI(LOG, "No config found, using defaults");
        return;
    }
#   define LBUF_SZ 200
    char lbuf[LBUF_SZ];
    while (1) {
        if (fgets(lbuf,LBUF_SZ,fp) == NULL) break;
        // Split on first '=' — allows values with spaces/special chars (WiFi SSID/password)
        char *p = lbuf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        char key[32], val[LBUF_SZ];
        size_t klen = (size_t)(eq - p);
        while (klen > 0 && (p[klen-1] == ' ' || p[klen-1] == '\t')) klen--;
        if (klen == 0 || klen >= sizeof(key)) continue;
        memcpy(key, p, klen);
        key[klen] = '\0';
        cfg_trim_copy(eq + 1, val, sizeof(val));
        {
            if (strcmp(key,"dac") == 0) {
                if (strcmp(val,"12") == 0) gconf[CONF_DAC] = CONF_DAC_12;
                if (strcmp(val,"16") == 0) gconf[CONF_DAC] = CONF_DAC_16;
            }
            if (strcmp(key,"mix") == 0) {
                if (strcmp(val,"sum") == 0) gconf[CONF_MIX] = CONF_MIX_SUM;
                if (strcmp(val,"div2") == 0) gconf[CONF_MIX] = CONF_MIX_DIV2;
                if (strcmp(val,"sqrt") == 0) gconf[CONF_MIX] = CONF_MIX_SQRT;
            }
            if (strcmp(key,"evt") == 0) {
                if (strcmp(val,"none") == 0) gconf[CONF_EVT] = CONF_EVT_NONE;
                if (strcmp(val,"flat") == 0) gconf[CONF_EVT] = CONF_EVT_FLAT;
                if (strcmp(val,"flat0") == 0) gconf[CONF_EVT] = CONF_EVT_FLAT0;
                if (strcmp(val,"bw11") == 0) gconf[CONF_EVT] = CONF_EVT_BW11;
                if (strcmp(val,"bg80") == 0) gconf[CONF_EVT] = CONF_EVT_BG80;
                if (strcmp(val,"by35") == 0) gconf[CONF_EVT] = CONF_EVT_BY35;
            }
            if (strcmp(key,"deb") == 0) {
                gconf[CONF_DEB] = atoi(val);
            }
            if (strcmp(key,"rpd") == 0) {
                gconf[CONF_RESTPD] = atoi(val);
            }
            if (strcmp(key,"ser") == 0) {
                if (strcmp(val,"none") == 0) gconf[CONF_SER] = CONF_SER_NONE;
                if (strcmp(val,"i2c") == 0) gconf[CONF_SER] = CONF_SER_I2C;
                if (strcmp(val,"uart") == 0) gconf[CONF_SER] = CONF_SER_UART;
            }
            if (strcmp(key,"addr") == 0) {
                gconf[CONF_I2C_ADDR] = (uint16_t)strtol(val,NULL,16);
            }
            if (strcmp(key,"usbbaud") == 0) {
                gusbbaud = (uint32_t)strtoul(val,NULL,10);
            }
            if (strcmp(key,"wifi_enable") == 0) {
                gconf[CONF_WIFI_ENABLE] =
                    (val[0] == 'y' || val[0] == 'Y' || val[0] == '1'
                     || strcmp(val,"on") == 0 || strcmp(val,"true") == 0) ? 1 : 0;
            }
            if (strcmp(key,"wifi_ssid") == 0) {
                strncpy(gwifi_ssid, val, WIFI_STR_MAX - 1);
                gwifi_ssid[WIFI_STR_MAX - 1] = '\0';
            }
            if (strcmp(key,"wifi_pwd") == 0) {
                strncpy(gwifi_pwd, val, WIFI_STR_MAX - 1);
                gwifi_pwd[WIFI_STR_MAX - 1] = '\0';
            }
            if (strcmp(key,"stheme") == 0) {
                strncpy(gconfsd, val, NSTHEME);
                gconfsd[NSTHEME] = '\0';
            }
            if (strcmp(key,"log") == 0) {
                if (strcmp(val,"no") == 0)   gconf[CONF_LOG] = CONF_LOG_NO;
                if (strcmp(val,"yes") == 0)  gconf[CONF_LOG] = CONF_LOG_YES;
                if (strcmp(val,"only") == 0) gconf[CONF_LOG] = CONF_LOG_ONLY;
            }
            if (strcmp(key,"volv") == 0) {
                gconf[CONF_VOLV] = (uint16_t)atoi(val);
            }
            if (strcmp(key,"vols") == 0) {
                gconf[CONF_VOLS] = (uint16_t)atoi(val);
            }
        }
    }
    fclose(fp);
}

void InitConfig(void) {
    // set all defaults
    gconf[CONF_DAC] = CONF_DAC_12;
    gconf[CONF_MIX] = CONF_MIX_DIV2;
    gconf[CONF_EVT] = CONF_EVT_BG80;
    gconf[CONF_DEB] = 10; // 10ms
    gconf[CONF_RESTPD] = 60; // 60ms
    gconf[CONF_SER] = CONF_SER_NONE;
    gconf[CONF_I2C_ADDR] = 0x66;
    gconf[CONF_WIFI_ENABLE] = 0;
    gconf[CONF_LOG]  = CONF_LOG_NO;
    gconf[CONF_VOLV] = 100;
    gconf[CONF_VOLS] = 100;
    gusbbaud = 115200;
    gwifi_ssid[0] = '\0';
    gwifi_pwd[0]  = '\0';
    strcpy(gconfsd, "orgsnd");
}

//----------------------------------------------------------------------------------------
//
// Persistent event log on SD (log.txt) — written when log=yes or log=only.
// Independent of the in-RAM activity_log_* ring buffer (used by /activity HTTP).
//
static FILE *g_eventlog_fp = NULL;

void event_log_open(void) {
    if (gconf[CONF_LOG] != CONF_LOG_YES && gconf[CONF_LOG] != CONF_LOG_ONLY) return;
    char p[40];
    snprintf(p, sizeof(p), "%s/log.txt", mount_point);
    g_eventlog_fp = fopen(p, "a");
    if (!g_eventlog_fp) {
        ESP_LOGW(LOG, "Cannot open %s for append", p);
        return;
    }
    fprintf(g_eventlog_fp, "---- boot ----\n");
    fflush(g_eventlog_fp);
}

void event_log_add(const char *msg) {
    if (!g_eventlog_fp || !msg) return;
    uint32_t ts = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    fprintf(g_eventlog_fp, "%u %s\n", (unsigned)ts, msg);
    fflush(g_eventlog_fp);
}


//----------------------------------------------------------------------------------------
//
// WAV file
//

// Strip header from WAV file
// quick but not bombensicher
//

static int StripWavHeader(int fh, uint32_t *len, uint32_t *stpos) {
#   define QBUF 400
    char buf[QBUF];
    int n = read(fh,buf,QBUF);
    if (n != QBUF) return 1; // cannot read header
    if (strncmp(buf,"RIFF",4) != 0) return 2; // wrong file format
    if (strncmp(&buf[8],"WAVE",4) != 0) return 3; // wrong file content
    for (int i=0; i<QBUF; i++) {
        if ((buf[i]=='d') && (buf[i+1]=='a') && (buf[i+2]=='t') && (buf[i+3]=='a')) {
            uint32_t *p = (void *)&(buf[i+4]);
            *len = *p;
            lseek(fh,i+8,SEEK_SET);
            *stpos = i+8;
            return 0;
        }
    }
    return 4; // no 'data' chunk found
}


//----------------------------------------------------------------------------------------
//
// Chain of running tracks
//

static Track *tchain = NULL;

// On-board LED helpers — active LOW (pin LOW = LED ON).
// Only configured on WROVER and only when ser=none, since LED_D1/D2 share pins
// with the optional UART/I2C interface.
static bool g_leds_active = false;
static inline void LedD1(bool on) { if (g_leds_active) gpio_set_level(LED_D1, on ? 0 : 1); }
static inline void LedD2(bool on) { if (g_leds_active) gpio_set_level(LED_D2, on ? 0 : 1); }

static void LedsInit(void) {
#ifdef ESP32_WROVER
    if (gconf[CONF_SER] != CONF_SER_NONE) return;  // pins owned by UART/I2C
    gpio_config_t io = {0};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << LED_D1) | (1ULL << LED_D2);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    if (gpio_config(&io) == ESP_OK) {
        g_leds_active = true;
        gpio_set_level(LED_D1, 1);  // off
        gpio_set_level(LED_D2, 1);  // off
    }
#endif
}

// Position-independent attribute test
static int HasAttribute(Sfile *s, char attr) {
    for (int i = 0; i < NSATTR; i++) if (s->attr[i] == attr) return 1;
    return 0;
}

// Create and insert a new track
//
static Track *NewTrack(Sfile *s) {
    if (s == NULL) return NULL;
    // open file
    int fh = open(s->fpath,O_RDONLY);
    if (fh < 0) {
        ESP_LOGE(LOG,"cannot open file %s",s->fpath);
        return NULL;
    }
    uint32_t tlen,stpos;
    int err = StripWavHeader(fh,&tlen,&stpos);
    if (err != 0) {
        ESP_LOGE(LOG,"not a valid WAV file, error %d -  %s",err,s->fpath);
        return NULL;
    }
    Track *t = (void *)malloc(sizeof(Track));
    t->next = tchain;
    tchain = t;
    t->fh = fh;
    t->tcnt = t->rcnt = tlen/2;
    t->stpos = stpos;
    t->bufl = 0;
    t->bidx = 0;
    t->sf = s; // refer soundfile
    for (int k = 0; k < NMODES; k++) t->mode[k] = 0;
    return t;
}

// Step through the track chain and remove all tracks which are marked for deletion
//
static void DeleteTracks(void) {
    for (Track **p = &tchain; *p != NULL; ) {
        Track *t = *p;
        if (t->mode[0] == 1) {
            close(t->fh);
            *p = t->next;
            free(t);
        }
        else {
            p = &(t->next);
        }
    }
}

// Mark tracks for deletion.
// id < 0  : hard kill (all tracks).
// id >= 0 : kill tracks whose sound id matches.
// Soft kill (sparing init/background sounds) is now done via MarkTracksByAttr(1,"i").
//
static void MarkTracksById(int16_t id) {
    for (Track *p = tchain; p != NULL; p = p->next) {
        if (id < 0) p->mode[0] = 1;
        else if (p->sf->id == (uint16_t)id) p->mode[0] = 1;
    }
}

// Mark tracks for deletion based on attribute presence.
//   inv == 0 : mark tracks that HAVE any of the chars in `attr`
//   inv != 0 : mark tracks that have NONE of the chars in `attr`
static void MarkTracksByAttr(uint8_t inv, const char *attr) {
    for (Track *p = tchain; p != NULL; p = p->next) {
        uint16_t vv = 0;
        for (const char *q = attr; *q != 0; q++) if (HasAttribute(p->sf,*q)) vv++;
        if (inv) {
            if (vv == 0) p->mode[0] = 1;
        } else {
            if (vv > 0) p->mode[0] = 1;
        }
    }
}

static void PrintTracks(void) {
    ets_printf("Track list:\n");
    for (Track *p = tchain; p != NULL; p = p->next) {
        ets_printf("  Track: %s\n",p->sf->fpath);
    }
}

static int NoTrack(void) {
    return(tchain == NULL);
}

// True if no finite (non-looping) tracks remain.
static int NoFiniteTracks(void) {
    for (Track *p = tchain; p != NULL; p = p->next) {
        if (! HasAttribute(p->sf,'l')) return 0;
    }
    return 1;
}


//----------------------------------------------------------------------------------------
//
// Fifo buffer
//

// Fifo is a ring buffer holding precalculated DAC values.
// For a smooth data stream to the DAC a buffer size of 1024 is required.
// 
//#define FIFO_SIZE 512 // must be a value 2^n (512, 1024, 2048, ..)
#define FIFO_SIZE 1024 // must be a value 2^n (512, 1024, 2048, ..)
#define FIFO_MASK (FIFO_SIZE-1)

struct Fifo {
    uint16_t data[FIFO_SIZE];
    uint16_t read;   // points to oldest entry
    uint16_t write;  // points to next empty slot
} fifo = {{}, 0, 0};

// Put value into the buffer, returns 0 if buffer is full
//
static inline uint8_t FifoIn(uint16_t mxval) {
    uint16_t next = ((fifo.write + 1) & FIFO_MASK);
    if (fifo.read == next) return 0;
    fifo.data[fifo.write] = mxval;
    fifo.write = next;
    return 1;
}

// Get a value from the buffer, return 0 if buffer empty
//
static inline uint8_t FifoOut(uint16_t *p) {
    if (fifo.read == fifo.write) return 0;
    *p = fifo.data[fifo.read];
    fifo.read = (fifo.read+1) & FIFO_MASK;
    return 1;
}

static inline uint8_t FifoFull() {
    uint16_t next = ((fifo.write + 1) & FIFO_MASK);
    return (fifo.read == next);
}

// Return the filling level of the buffer
//
static inline uint16_t FifoLevel(void) {
    if (fifo.write >= fifo.read) return fifo.write-fifo.read;
    else return FIFO_SIZE-(fifo.read-fifo.write);
}


//----------------------------------------------------------------------------------------
//
// Mixer
//

// return 1 to cause deletion afterwards
//
static void StartBackgroundSound(void);
//
static int TryCloseTrack(Track *p) {
    if (HasAttribute(p->sf,'i')) {
        // try start another background sound
        // this will only be successful if the ID_IL_GROUP is not empty
        StartBackgroundSound();
    }
    else if (HasAttribute(p->sf,'l')) {
        // file marked with attribute 'l' (loop)
        lseek(p->fh,p->stpos,SEEK_SET);
        p->rcnt = p->tcnt;
        p->bufl = 0;
        p->bidx = 0;
        return 0;
    }
    // close down
    p->mode[0] = 1;
    return 1;
}

// Step through the track chain and mix one sample of each track
// There are different methods to mix, see configuration 'mix'
// Return 1 if chain contains an entry to be deleted, 0 otherwise
//
static uint8_t RunMixer() {

    if (FifoFull()) return 0; // nothing to do

    int cnt = 0;
    uint8_t touch = 1;
    uint8_t dflag = 0;
    int32_t tval = 0;
    for (Track *p = tchain; p != NULL; p = p->next) {
        if (p->mode[0]) dflag = 1;
        else if (p->rcnt == 0) dflag = TryCloseTrack(p);
        else {
            int32_t val = 0;
            if (p->bidx < p->bufl) {
                val = (int32_t)p->buf[p->bidx];
                p->bidx++;
                p->rcnt--;
            }
            else {
                // refill buffer from file
                ssize_t bcnt = read(p->fh,&(p->buf[0]),BUFSZ*sizeof(int16_t));
                if (bcnt == 0) { // eof
                    dflag = TryCloseTrack(p);
                }
                else if (bcnt < 0) { // read error
                    p->mode[0] = 1;
                    dflag = 1;
                }
                else { // continue
                    p->bufl = bcnt/sizeof(int16_t);
                    val = (int32_t)p->buf[0];
                    p->bidx = 1;
                    p->rcnt--;
                }
            }

            // adjust volume
            if (p->sf->vol != 100) val = (val * p->sf->vol)/100;

            // mixer method
            switch (gconf[CONF_MIX]) {
            case CONF_MIX_SUM:
                tval += val;
                break;
            case CONF_MIX_DIV2:
                tval += val/2;
                break;
            case CONF_MIX_SQRT:
                tval += val;
                cnt++;
                break;
            }

            // statistics
            if (touch) {
                stac.mixxcnt++;
                touch = 0;
            }
        }
    }

    // xxxxx may remove this mixing method
    if (gconf[CONF_MIX] == CONF_MIX_SQRT) {
        switch (cnt) {
        case 0: case 1:  // do nothing
            break;
        case 2:  // div by 1.4
            tval = (10*tval)/14;
            break;
        case 3:  // div by 1.7
            tval = (10*tval)/17;
            break;
        default: // div by 2
            tval = tval/2;
            break;
        }
    }
    
    // clip to int16_t range, add 1/2 range to make all positive,
    // then convert from int32 to uint16 and insert in Fifo
    //
    if (tval > MAX_INT16) tval = MAX_INT16; // clip volume
    if (tval < MIN_INT16) tval = MIN_INT16; // clip volume
    tval += -(MIN_INT16);
    FifoIn((uint16_t)tval);
    return dflag;
}

//
// Experimental
// Add a new track to the pre-calculated DAC values
// Accesses fifo-buffer w/out abstraction => needs improvement, tbd 
//
static void CorrFifo(Track *p) {
    uint16_t k;
    int16_t buf[FIFO_SIZE];

    if (p == NULL) return;
    k = FifoLevel();
    ssize_t bcnt = read(p->fh,&(buf[0]),k*sizeof(int16_t)); 
    int32_t tval;
    uint16_t j = fifo.read;
    for (uint16_t i = 0; i < bcnt/sizeof(uint16_t); i++) {
        tval = fifo.data[j];
        switch (gconf[CONF_MIX]) {
        case CONF_MIX_SUM:
            tval += buf[i];
            break;
        case CONF_MIX_DIV2:
            tval += buf[i]/2;
            break;
        case CONF_MIX_SQRT:
            tval += buf[i]/2; //cheating
            break;
        }
        if (tval > MAX_UINT16) tval = MAX_UINT16; // clip volume
        if (tval < 0) tval = 0; // clip volume
        fifo.data[j] = (uint16_t)tval;
        j = (j+1)&FIFO_MASK;
    }
    p->rcnt -= bcnt/sizeof(uint16_t);
}


//----------------------------------------------------------------------------------------
//
// DAC
// Configure internal DAC of EPS32
// Configure SPI bus and add external DAC as device
//

#if 1 // bitbang version, SPI version see below
//
// The bitbang version is much faster then the SPI bus version, see below
// Duration of interrupt: 2.7us (vs 12.5us with SPI)
//

static void InitExtDAC(void) {

    // configure GPIO for output
    gpio_config_t io_conf = {0};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << DAC_MOSI)|(1ULL << DAC_CLK)|(1ULL << DAC_CS);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE; // remove
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(DAC_CS,1);
    gpio_set_level(DAC_MOSI,0);
    gpio_set_level(DAC_CLK,0);
}

#ifdef ESP32_WROVER
static inline void LoadExtDAC(uint16_t val) {

// execution time for LoadExtDAC():
// - using high level function gpio_set_level() => 12us => too slow
// - output to register GPIO.out_w1tc.val => 3.4us 
// Period is 22.5us (44.1kHz sample rate)
    
//  TP_SET();
    GPIO.out_w1tc = 1UL << DAC_CS; // CS low
    for (uint8_t i=0; i<16; i++) { 
        if (val & 0x8000) GPIO.out_w1ts = 1UL << DAC_MOSI;
        else GPIO.out_w1tc = 1UL << DAC_MOSI;
        GPIO.out_w1ts = 1UL << DAC_CLK;
        GPIO.out_w1tc = 1UL << DAC_CLK;
        val = val<<1;
    }
    GPIO.out_w1ts = 1UL << DAC_CS; // CS high
    GPIO.out_w1tc = 1UL << DAC_MOSI;
    GPIO.out_w1tc = 1UL << DAC_CLK;
//  TP_CLR();
}
#endif

#ifdef ESP32_S3
static inline void LoadExtDAC(uint16_t val) {

// execution time for LoadExtDAC():
// - using high level function gpio_set_level() => 12us => too slow
// - output to register GPIO.out_w1tc.val => 3.4us 
// Period is 22.5us (44.1kHz sample rate)
    
// porting to ESP32-S3
// GPIO.out_w1tc covers bit 0..31
// setting bit 32..63 has to done in the next word, GPIO.out1_w1tc
// this results in a offset of 32 in the gpio number
#define CORRB(x) ((x)-32) 

// TP_SET();
    GPIO.out1_w1tc.val = 1UL << CORRB(DAC_CS); // CS low
    for (uint8_t i=0; i<16; i++) { 
        if (val & 0x8000) GPIO.out_w1ts = 1UL << DAC_MOSI;
        else GPIO.out_w1tc = 1UL << DAC_MOSI;
        GPIO.out1_w1ts.val = 1UL << CORRB(DAC_CLK);
        GPIO.out1_w1tc.val = 1UL << CORRB(DAC_CLK);
        val = val<<1;
    }
    GPIO.out1_w1ts.val = 1UL << CORRB(DAC_CS); // CS high
    GPIO.out_w1tc = 1UL << DAC_MOSI;
    GPIO.out1_w1tc.val = 1UL << CORRB(DAC_CLK);
// TP_CLR();
}
#endif

#else // SPI version
//
// data transfer with SPI3 and spi_device_polling_transmit()
// 
// not used, because transmitting small data chunks consumes too much time
// initiating the transfer with spi_device_polling_transmit() takes 8us,
// exit (after transfer) another 3us,
// a total of 11us overhead, see scope screenshot sd-01.png
//
// see https://esp32.com/viewtopic.php?t=10546
// see https://esp32.com/viewtopic.php?t=24774
// see https://esp32.com/viewtopic.php?t=25417
// see https://esp32.com/viewtopic.php?t=8720
// all same problem, no solution
// 

spi_device_handle_t spi_device;

// configure SPI3 host (VSPI) for external DAC
//
static void InitExtDAC(void) {

    ESP_LOGI(LOG,"Initializing SPI bus...");
    // configuration for the SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = DAC_MOSI,
        .miso_io_num = -1,   // not used
        .sclk_io_num = DAC_CLK,
        .quadwp_io_num = -1, // not used
        .quadhd_io_num = -1,  // not used
        .max_transfer_sz = 32 // xxxx Max transfer size in bytes
    };

    // initialize the SPI bus
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST,&buscfg,SPI_DMA_DISABLED));
    ESP_LOGI(LOG,"SPI bus initialized.");

    ESP_LOGI(LOG,"Adding SPI device...");
    // configuration for the DAC MCP4821
    spi_device_interface_config_t devcfg = {0};
    devcfg.clock_speed_hz = 10 * 1000 * 1000; // clock out @ 10MHz (up to 20MHz)
    devcfg.mode = 0;                          // SPI mode 0 (CPOL=0, CPHA=0)
    devcfg.spics_io_num = DAC_CS;             // CS pin
    devcfg.queue_size = 1;                    // xxxxx

    // add device
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST,&devcfg,&spi_device));
    ESP_LOGI(LOG,"SPI device added.");
}

static inline void LoadExtDAC(uint16_t val) {
#   define DAC_CONFIG_BITS 0x30
    uint8_t sbuf[2];
//  TP_SET();
    sbuf[0] = val&0xff;
    // 12bit, DAC MCP4821
    if (gconf[CONF_DAC] == CONF_DAC_12) sbuf[1] = ((val>>8) & 0x0f) + DAC_CONFIG_BITS;
    else sbuf[1] = val>>8;
    spi_transaction_t t;
    memset(&t,0,sizeof(t)); // zero out the transaction structure
    t.length = 16;
    t.tx_buffer = sbuf;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_device,&t));
//  TP_CLR();
}

#endif

//----------------------------------------------------------------------------------------
//
// DAC Feeder (ISR)
// Calling period 22.7us (44.1kHz)
// In case fifo-buffer is empty, simply leave DAC to hold its current value 1 tick longer

static bool LoadDAC12_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx) {
    uint16_t mxval = 0;
    if (FifoOut(&mxval)) {
        // 12bit, DAC MCP4821
#       define DAC_CONFIG_BITS 0x3000;
        mxval = (mxval>>4) | DAC_CONFIG_BITS;
        LoadExtDAC(mxval);
        stac.isrodac++;
    }
    else stac.isrmiss++;
    return false;
}

static bool LoadDAC16_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx) {
    uint16_t mxval = 0;
    if (FifoOut(&mxval)) {
        // 16bit
        LoadExtDAC(mxval);
        stac.isrodac++;
    }
    else stac.isrmiss++;
    return false;
}

// Setup periodic interrupt @ 44.1 kHz
//
static void SetupDACFeeder(void) {

    gptimer_handle_t gptimer = NULL;
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT, // select the default clock source
        .direction = GPTIMER_COUNT_UP,      // counting direction is up
//        .resolution_hz = 1 * 1000 * 1000,   // resolution is 1 MHz, i.e., 1 tick equals 1 microsecond
        .resolution_hz = 1 * 1000 * 1058,   // 1 tick equals 0.9452 microsecond
    };
    // create a timer instance
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));
    

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,      // when the alarm event occurs, the timer will automatically reload to 0
        .alarm_count = 24,      // 44.1 kHz @ res 0.9452 us
        .flags.auto_reload_on_alarm = true, // Enable auto-reload function
    };
    // set the timer's alarm action
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    // set different callbacks for 12bit or 16bit DAC
    gptimer_event_callbacks_t cbs = {};
    if (gconf[CONF_DAC] == CONF_DAC_12) cbs.on_alarm = LoadDAC12_cb;
    else cbs.on_alarm = LoadDAC16_cb;
    
    // register timer event callback functions
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    ESP_ERROR_CHECK(gptimer_start(gptimer));
}


//----------------------------------------------------------------------------------------
//
// SDMMC
// mount SD card
//

static sdmmc_card_t *card;

esp_err_t MountSDCard(void) {

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; // 40MHz; disable to reduce to 20MHz

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
#ifdef ESP32_S3
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_DAT0;
    slot.d1 = SD_DAT1;
    slot.d2 = SD_DAT2;
    slot.d3 = SD_DAT3;    
#endif
#ifdef ESP32_WROVER
    slot.d1 = SD_DAT1;
    slot.d2 = SD_DAT2;
    slot.d3 = SD_DAT3;    
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = FAT_MAX_FILES,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGE(LOG,"SD card, mount failed: %s", esp_err_to_name(err));
        ESP_LOGE(LOG,"Check whether SD card is inserted");
        return err;
        }

    // print card properties
    sdmmc_card_print_info(stdout, card);
    
    return ESP_OK;
}

//----------------------------------------------------------------------------------------
//
// Sound Groups
//

// chain of all groups
static Sgroup *gchain = NULL;

static Sgroup *FindGroup(uint16_t gnum) {
    Sgroup *p = gchain;
    for (; p != NULL; p = p->next) if (p->id == gnum) return p;
    return NULL;
}

// groups for startup sounds and looping background music
//
static Sgroup *CreateSpecialGroups() {
    // create a group for the i flagged sound files (startup sounds)
    Sgroup *p = malloc(sizeof(Sgroup));
    p->id = ID_I_GROUP;
    p->attr = (uint8_t)'m';
    p->nom = 0;
    p->nmp = 0;
    p->first = NULL;
    strcpy(p->fname,"i-group");
    p->next = NULL;
    // create a group for the i & l flagged sound files (background music)
    Sgroup *q = malloc(sizeof(Sgroup));
    q->id = ID_IL_GROUP;
    q->attr = (uint8_t)'m';
    q->nom = 0;
    q->nmp = 0;
    q->first = NULL;
    strcpy(q->fname,"il-group");
    q->next = p;
    return q;
}

static void AppendGmember(Gmember **rp, uint16_t gmem) {
    Gmember *p = malloc(sizeof(Gmember));
    p->id = gmem;
    p->next = NULL;
    Gmember **q = rp;
    for (; *q != NULL; q = &((*q)->next));
    *q = p;
}

static void AddGroupMember(uint16_t gnum, uint16_t gmem) {
    for (Sgroup *p = gchain; p != NULL; p = p->next) {
        if (p->id == gnum) {
            AppendGmember(&(p->first),gmem);
            p->nom++;
            return;
        }
    }
    // silently discard if not found
}

static Sgroup *NewGroup(char *fname) {
    int gnum;
    char  atc;
    Gmember *gmembers = NULL;
    uint8_t nom = 0;
    
    char buffer[FPATHLEN+1];
    strcpy(buffer,fname);
    char *sep = "-";
    char *tok = strtok(buffer,sep);
    if (tok) {
        int d;
        if (sscanf(tok,"%1d%1d%1d%1d",&d,&d,&d,&d) != 4) goto abort;
        if (sscanf(tok,"%d",&gnum) != 1) goto abort;
    }
    else goto abort;
    tok = strtok(NULL,sep);
    if (tok) {
        if (sscanf(tok,"%c",&atc) != 1) goto abort;
    }
    else goto abort;
    tok = strtok(NULL,sep);
    if (tok) {
        int gmem;
        if (sscanf(tok,"%d",&gmem) == 1) {
            AppendGmember(&gmembers,gmem);
            nom++;
            tok = strtok(NULL,sep);
            while (tok) {
                if (sscanf(tok,"%d",&gmem) == 1) {
                    AppendGmember(&gmembers,gmem);
                    nom++;
                    tok = strtok(NULL,sep);
                }
                else break;
            }
        }
        else {
            printf("Empty group => tbd\n");
            // try open the file to read the members
            // tbd
        }
    }
    else goto abort;

    Sgroup *p = malloc(sizeof(Sgroup));
//    strncpy(p->fname,fname,FPATHLEN);
    strcpy(p->fname,fname);
    p->id = gnum;
    p->attr = (uint8_t)atc;
    p->nom = nom;
    p->nmp = 0;
    p->first = gmembers;
    return p;
    
abort:
    // not a valid group expression
    return NULL;
}


//----------------------------------------------------------------------------------------
//
// Sound files
//

// chain of all sound files
static Sfile *schain = NULL;

static Sfile *NewSound(char *fpath, uint16_t id, uint8_t a[], uint16_t vol) {
    ESP_LOGI(LOG,"insert sound file %s",fpath);
    Sfile *s = (void *)malloc(sizeof(Sfile));
    s->id = id;
    for (int i = 0; i < NSATTR; i++) s->attr[i] = a[i];
    s->vol = vol;
    if (s->vol > 100) s->vol = 100;
    strncpy(s->fpath,fpath,FPATHLEN);
    s->fpath[FPATHLEN] = 0;
    s->next = NULL;

    // Per-class volume scaling (voice vs sound)
    if (HasAttribute(s,'v')) s->vol = (s->vol * gconf[CONF_VOLV]) / 100;
    else                     s->vol = (s->vol * gconf[CONF_VOLS]) / 100;
    if (s->vol > 100) s->vol = 100;

    return s;
}


//----------------------------------------------------------------------------------------
//
// Read catalog of entries (sound files and groups) from SD
// Setup sound chain (schain) and sound group chain (gchain)
//

// Create and insert an entry into schain (sounds) or gchain (groups)
// An entry is either a sound file or a sound group
//
static int16_t InsertEntry(char *fpath, char *fname) {
    char buf[300];
    strcpy(buf,fpath);
    strcat(buf,fname);
    ESP_LOGI(LOG,"consider file %s",buf);

    uint16_t id;
    uint8_t a[4];
    uint16_t vol;

    // file extension
    char *ext = strrchr(fname,'.');
    if (ext == NULL) return -1;
    
    // try sound file
    // Example for the name of a sound file:
    // 0012-xbxx-100-ring-my-bell.wav 
    
    int n = sscanf(fname,"%hu-%c%c%c%c-%hu-%*s",&id,&(a[0]),&(a[1]),&(a[2]),&(a[3]),&vol);
    if ((n == 6) && (strcmp(ext,".wav") == 0)) { 
        // this is a sound file
        Sfile *s = NewSound(buf,id,a,vol);
        if (s) {
            s->next = schain;
            schain = s;
            // if file has an i or i&l attribute, add it to its special group
            if (HasAttribute(s,'i')) {
                if (HasAttribute(s,'l')) AddGroupMember(ID_IL_GROUP,id);
                else                     AddGroupMember(ID_I_GROUP,id);
            }
            return s->id;
        }
        return -1;
    }

    // try sound group
    // Example of a group entry:
    // 0009-m-15-71-12-exit-lane-left.grp
    
    n = sscanf(fname,"%hu-%c-%*s",&id,&(a[0]));
    if ((n == 2) && (strcmp(ext,".grp") == 0)) {
        // this is a sound group
        Sgroup *g = NewGroup(fname);
        if (g) {
            g->next = gchain;
            gchain = g;
            return g->id;
        }
        return -1;
    }

    // try attract-mode file
    // Example: 0009-02-01-night-attract.atr
    // NNNN-TT-EE-desc.atr : group id, inactivity timeout (min), replay interval (min)
    {
        uint16_t nnnn;
        uint8_t  tt, ee;
        n = sscanf(fname,"%hu-%hhu-%hhu-%*s",&nnnn,&tt,&ee);
        if ((n == 3) && (strcmp(ext,".atr") == 0) && tt > 0 && ee > 0) {
            attract_group_id    = nnnn;
            attract_timeout_ms  = (uint32_t)tt * 60000UL;
            attract_interval_ms = (uint32_t)ee * 60000UL;
            ESP_LOGI(LOG,"attract mode: group=%u timeout=%umin interval=%umin",
                     nnnn, tt, ee);
            return (int16_t)nnnn;
        }
    }
    return -1;
}

static void ReadCatalogFromSD() {
    char fpath0[64];
    snprintf(fpath0, sizeof(fpath0), "%s/%s/",mount_point,gconfsd);
    ESP_LOGI(LOG,"read catalog %s",fpath0);
    uint16_t ne = 0;
    DIR *dp = opendir(fpath0);
    if (dp == NULL) {
        // theme directory missing — try to create it then re-open.
        char dpath[64];
        snprintf(dpath, sizeof(dpath), "%s/%s",mount_point,gconfsd);
        ESP_LOGW(LOG,"theme dir missing, creating %s",dpath);
        mkdir(dpath, 0777);
        dp = opendir(fpath0);
    }
    if (dp != NULL) {
        struct dirent *ep;
        while ((ep = readdir(dp)) != NULL) {
            if (InsertEntry(fpath0,ep->d_name) < 0) continue;
            ne++;
        }
        closedir(dp);
        ESP_LOGI(LOG,"read %u entries",ne);
    }
    else {
        ESP_LOGE(LOG,"read failed %s",fpath0);
    }
}

// Reset next-member pointer of every sound group.
static void ResetGroups(void) {
    for (Sgroup *p = gchain; p != NULL; p = p->next) p->nmp = 0;
}

// Advance next-member pointer of one specific group.
static void IncNextMember(uint16_t gnum) {
    for (Sgroup *p = gchain; p != NULL; p = p->next) {
        if (p->id == gnum && p->nom > 0) {
            p->nmp = (p->nmp + 1) % p->nom;
            return;
        }
    }
}

//----------------------------------------------------------------------------------------
//
// Start playing sound file
//

static Sfile *PickMember(Gmember *first, uint16_t pos) {
//  position is zero-based
    Gmember *m = first;
    for (int16_t i = 0; i < pos; i++) if (m) m = m->next;
    if (m == NULL) return NULL;
    for (Sfile *s = schain; s != NULL; s = s->next) if (s->id == m->id) return s; 
    return NULL;
}

static Sfile *LookupSound(uint16_t id) {
    Sfile *s;
    for (s = schain; s != NULL; s = s->next) if (s->id == id) return s; 
    Sgroup *p;
    for (p = gchain; p != NULL; p = p->next) {
        if (p->id == id) {
            if (p->nom == 0) return NULL;
            if (p->attr == 'm') {
                // random sound
                p->nmp = esp_random() % p->nom;
                s = PickMember(p->first,p->nmp);
                return s;
            }
            if (p->attr == 'r') {
                // next sound
                s = PickMember(p->first,p->nmp);
                p->nmp = (p->nmp+1) % p->nom;
                return s;
            }
            // attr not valid
            return NULL;
        }
    }
    // no match
    return NULL; 
}

// start a background sound
//
static void StartBackgroundSound() {
    Sfile *s = LookupSound(ID_IL_GROUP);
    if (s) NewTrack(s);
}

// initial start of sound files marked with attribute 'i'
//
static void StartInitSound() {
    Sfile *s = LookupSound(ID_I_GROUP);
    if (s == NULL) s = LookupSound(ID_IL_GROUP);
    if (s) {
        NewTrack(s);
        ets_printf("InitSound => sfile %d\n",s->id);
        {
            char m[48];
            snprintf(m, sizeof(m), "InitSound => sfile %u", (unsigned)s->id);
            activity_log_add(m);
            event_log_add(m);
        }
        }
}

// Insert sound as a track into the track list
// Abort silently if something goes wrong
//
static void StartSound(uint16_t id) {

    ets_printf("Start sound %d\n",id);
    {
        char m[48];
        snprintf(m, sizeof(m), "Start sound %u%s", (unsigned)id,
                 attract_playing_now ? " (attract)" : "");
        activity_log_add(m);
        event_log_add(m);
    }

    // reset attract inactivity timer on regular (non-attract) playback
    if (!attract_playing_now && attract_group_id != 0) {
        attract_last_activity = xTaskGetTickCount();
        if (attract_active) {
            attract_active = false;
            LedD2(false);
        }
    }

    Sfile *s = LookupSound(id);
    if (s == NULL) {
        char m[48];
        snprintf(m, sizeof(m), "Lookup sound %u: not found", (unsigned)id);
        activity_log_add(m);
        return;
    }

    ets_printf("Lookup sound %d\n",s->id);
    {
        char m[48];
        snprintf(m, sizeof(m), "Lookup sound %u", (unsigned)s->id);
        activity_log_add(m);
    }

    // log=only suppresses audio playback but the activity entries above were emitted
    if (gconf[CONF_LOG] == CONF_LOG_ONLY) return;

    if (HasAttribute(s,'k')) {          // hard kill — stop everything, then play
        MarkTracksById(-1);
        NewTrack(s);
    }
    else if (HasAttribute(s,'c')) {     // soft kill — spare init/background sounds
        MarkTracksByAttr(1, "i");
        NewTrack(s);
    }
    else if (HasAttribute(s,'q')) {     // quit — spare looping and voice sounds
        MarkTracksByAttr(1, "lv");
        NewTrack(s);
    }
    else if (HasAttribute(s,'b')) {     // break — only this id
        MarkTracksById((int16_t)id);
        NewTrack(s);
    }
    else {
        CorrFifo(NewTrack(s));
    }
}


//----------------------------------------------------------------------------------------
//
// WAV Player
//
//

static void PrintAllStruct(void);


// Attract-mode tick: called from the main loop. If no regular sound has been
// triggered for attract_timeout_ms, periodically re-trigger the attract group
// every attract_interval_ms.
//
static void AttractTick(void) {
    TickType_t now = xTaskGetTickCount();
    if (!attract_active) {
        LedD2(false);
        if ((uint32_t)(now - attract_last_activity) >= pdMS_TO_TICKS(attract_timeout_ms)) {
            attract_active = true;
            LedD2(true);
            // trigger the first attract sound immediately
            attract_last_play = now - pdMS_TO_TICKS(attract_interval_ms);
            ets_printf("Attract mode ON (group %u)\n", attract_group_id);
            {
                char m[48];
                snprintf(m, sizeof(m), "Attract mode ON (group %u)", (unsigned)attract_group_id);
                activity_log_add(m);
                event_log_add(m);
            }
        }
        return;
    }
    if ((uint32_t)(now - attract_last_play) >= pdMS_TO_TICKS(attract_interval_ms)) {
        attract_playing_now = true;
        StartSound(attract_group_id);
        attract_playing_now = false;
        attract_last_play = now;
    }
}

static void ExecCommand(Rxcmd *k) {
    switch (k->cmd) {
    case 'p': // play sound
        StartSound(k->arg);
        break;
    case 'k': // kill all sounds
        MarkTracksById(-1);
        break;
    case 't': // kill all sounds except voices, reset group next-member pointers
        MarkTracksByAttr(1, "v");
        ResetGroups();
        break;
    case 'n': // increment group next-member pointer
        IncNextMember(k->arg);
        break;
    case 'w': // kill a specific sound id
        MarkTracksById((int16_t)k->arg);
        break;
    case 'm': // kill all looping sounds
        MarkTracksByAttr(0, "l");
        break;
    default:
        break;
    }
}

void WAVPlayer(void *pvParameters) {

    // configure test & LED pin
    gpio_config_t io_conf = {0};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << TESTPIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    char buf[100];
    const esp_app_desc_t *ppd = esp_app_get_description();
    printf("Version: %s\n",ppd->version);

    // spoken version
    sprintf(buf,"%s/spokenvers/version-%c-%c-%c.wav",mount_point,ppd->version[0],ppd->version[2],ppd->version[4]);
    int fh = open(buf,O_RDONLY);
    if (fh >= 0) {
        close(fh);
        uint8_t attr[NSATTR];
        for (int i = 0; i < NSATTR; i++) attr[i] = 'x';
        // Create an entry for the spoken version         
        schain = NewSound(buf,ID_VERSION,attr,100);
        StartSound(ID_VERSION);
    }
    
    LedsInit();
    gchain = CreateSpecialGroups();
    ReadCatalogFromSD();
    {
        char m[48];
        snprintf(m, sizeof(m), "pwavplayer v%s ready", gversion);
        activity_log_add(m);
    }
    attract_last_activity = xTaskGetTickCount();
    InitExtDAC();
    SetupDACFeeder();

    // play spoken version if linked into schain
    // this is done in its own loop to make sure version comes well before the startup sound
    uint8_t run = 1;
    while (run) {
        if (RunMixer()) DeleteTracks();
        if (FifoFull()) vTaskDelay(1);
        if (NoTrack()) run = 0;
    }
    
#if 0
    //StartSound(16); // you want to play, let's play 'i'
    //StartSound(18); // horn, loop 'l'
    //StartSound(5);  // kill 'k'
    //StartSound(6);  // break 'b'
    //StartSound(7);
    //StartSound(50);  // Fred Wesley, House Party
    //StartSound(101);  // Sinus 1kHz
    //PrintTracks();
#endif

    PrintAllStruct();
    
    // statistics
    stac.isrmiss = 0;
    stac.isrodac = 0;
    stac.mixxcnt = 0;
    StartInitSound();

    // main loop
    Rxcmd xcmd;
    bool led1_on = false;
    run = 1;
    while (run) {
        if (RunMixer()) DeleteTracks();
        if (FifoFull()) vTaskDelay(1);
        if (xStreamBufferReceive(xpinevt,&xcmd,sizeof(Rxcmd),0) == sizeof(Rxcmd)) ExecCommand(&xcmd);
        if (attract_group_id != 0) AttractTick();

        // D1: ON while at least one track is playing.  Edge-triggered to avoid
        // hammering gpio_set_level on every iteration.
        bool playing = (tchain != NULL);
        if (playing != led1_on) {
            led1_on = playing;
            LedD1(playing);
        }
        
#if 0
        if (stac.mixxcnt == 200000) StartSound(1);
        if (stac.mixxcnt == 280000) StartSound(2);
        if (stac.mixxcnt == 300000) StartSound(3);
        if (stac.mixxcnt == 320000) StartSound(4);
        if (stac.mixxcnt == 600000) StartSound(5);
        //if ((stac.mixxcnt) > 650000) run=0; // exit
        //if (NoTrack()) run=0; // exit after last track ends
#endif
        
    }
    
}

void WAVDummy(void *pvParameters) {
    const esp_app_desc_t *ppd = esp_app_get_description();
    printf("Version: %s\n",ppd->version);
    int run = 1;
    while (run) {
        vTaskDelay(100);
    }
}

static void PrintStatistics(void) {
    ets_printf("Statistik:\n");
    ets_printf("  ISR odac: %lu (total von Fifo geliefert und an DAC gesendete Werte)\n", stac.isrodac);
    ets_printf("  ISR miss: %lu (total Anzahl Fifo leer)\n", stac.isrmiss);
    ets_printf("  Mix xcnt: %lu (total von Mixer gemischte Werte mit mind. 1 Track\n", stac.mixxcnt);
    ets_printf("  odac + miss = total DAC Feeder Aufrufe\n");
}

static void PrintAllStruct(void) {
    ets_printf("Soundfile list:\n");
    for (Sfile *s = schain; s != NULL; s = s->next) ets_printf("  Sound: %d (%s)\n",s->id,s->fpath);
    ets_printf("Group list:\n");
    for (Sgroup *s = gchain; s != NULL; s = s->next) {
        ets_printf("  Group: %d (%s)\n",s->id,s->fname);
        for (Gmember *q = s->first; q != NULL; q = q->next) {
            ets_printf("    Member: %d\n",q->id);
        }
    }
    ets_printf("End.\n");
}

