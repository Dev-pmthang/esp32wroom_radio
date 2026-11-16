/*
 * radio_config.h
 * Cau hinh Phan cung, So do chan co dinh, Hang so FreeRTOS va Struct Trang thai He thong
 * Chuan ESP-IDF v5.3+ (Zero Runtime Heap Allocation)
 */
#ifndef RADIO_CONFIG_H
#define RADIO_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. CHON MODE MAN HINH LED / TFT & SO DO CHAN PHAN CUNG
 *    - LCD_MODE_ST7789_240X240 (0): Man hinh 1.54" ST7789 (240x240, 27MHz, CS=5/None)
 *    - LCD_MODE_ST7735_128X160 (1): Man hinh 1.8"  ST7735 (128x160, 20MHz, CS=15, Offset 2,1)
 *    Co the chon mac dinh tai day (RADIO_LCD_MODE) hoac doi truc tiep tren Web UI!
 * ========================================================================= */
#define LCD_MODE_ST7789_240X240 0
#define LCD_MODE_ST7735_128X160 1

#ifndef RADIO_LCD_MODE
#define RADIO_LCD_MODE          LCD_MODE_ST7735_128X160
#endif

// ================= KHAI BÁO CHÂN MÀN HÌNH ST7735 (128x160) =================
#define LCD_HOST                SPI2_HOST           // VSPI trên ESP32
#define LCD_PIXEL_CLOCK_HZ      (20 * 1000 * 1000)  // 20MHz (ổn định nhất, không nhiễu hạt trên ST7735)

#define PIN_NUM_MOSI            GPIO_NUM_23         // Chân SDA trên mạch TFT
#define PIN_NUM_CLK             GPIO_NUM_18         // Chân SCK trên mạch TFT
#define PIN_NUM_CS              GPIO_NUM_15         // Chân CS trên mạch TFT
#define PIN_NUM_DC              GPIO_NUM_2          // Chân A0 trên mạch TFT
#define PIN_NUM_RST             GPIO_NUM_4          // Chân RESET trên mạch TFT
#define PIN_NUM_BK_LIGHT        GPIO_NUM_32         // Chân LED trên mạch TFT

// Độ phân giải (Nếu xoay ngang màn hình thành 160x128 thì đảo ngược 2 giá trị này)
#define LCD_H_RES               128
#define LCD_V_RES               160

// Offset bù viền: Đặt (0, 0) để CASET (0..127) không vượt quá cột 127 gây xéo hình trên ST7735R
#define LCD_OFFSET_X            0
#define LCD_OFFSET_Y            0

/* Huong xoay MADCTL cho ST7735 (Portrait: 0x00 hoac 0xC0 | Landscape: 0x60 hoac 0xA0) */
#define ST7735_MADCTL_PORTRAIT  0x00
#define ST7735_MADCTL_LANDSCAPE 0x60

// ================= KHAI BÁO CHÂN MÀN HÌNH ST7789 (240x240) =================
#define PIN_TFT_SCL             PIN_NUM_CLK
#define PIN_TFT_SDA             PIN_NUM_MOSI
#define PIN_TFT_RES             PIN_NUM_RST
#define PIN_TFT_DC              PIN_NUM_DC
#define PIN_TFT_BLK             PIN_NUM_BK_LIGHT
#define PIN_TFT_CS_HW           GPIO_NUM_5          /* CS cho ST7789 (hoac man CS-less) */
#define PIN_TFT_CS_COMPAT_2     PIN_NUM_CS          /* CS = GPIO 15 cho ST7735 */

#define TFT_MAX_WIDTH           240
#define TFT_MAX_HEIGHT          240
#define TFT_WIDTH               240
#define TFT_HEIGHT              240
#define TFT_SPI_CLOCK_HZ        (27 * 1000 * 1000)  /* 27 MHz SPI DMA cho ST7789 */

/* IC Khuech dai & DAC I2S MAX98357A */
#define PIN_I2S_DIN             GPIO_NUM_25
#define PIN_I2S_BCLK            GPIO_NUM_26
#define PIN_I2S_LRC             GPIO_NUM_27
#define PIN_I2S_SD              GPIO_NUM_33     /* Hardware Mute: 0 = Mute, 1 = Unmute */

/* Nut nhan Dieu khien Truc tiep (Toggle Play / Pause) */
#define PIN_BTN_PLAY_PAUSE      GPIO_NUM_0      /* Nut BOOT, Pull-up noi, Ngat canh xuong */
#define BTN_DEBOUNCE_US         200000LL        /* 200ms Debounce ngay trong ISR */

/* =========================================================================
 * 2. THONG SO KIEN TRUC DA LUONG FREERTOS & BO NHO DEM
 * ========================================================================= */
/* Phan chia Core (Core 0: Network & Control | Core 1: Realtime Audio & UI) */
#define CORE_NETWORK_CTRL       0
#define CORE_REALTIME_AUDIO_UI  1

/* Do uu tien cac Task (Task Priorities) */
#define TASK_PRIO_UI_RENDER     4
#define TASK_PRIO_HTTP_STREAM   5
#define TASK_PRIO_WIFI_WEB      7               /* Uu tien phuc vu Web Server tren Core 0 */
#define TASK_PRIO_BTN_CTRL      10
#define TASK_PRIO_AUDIO_I2S     16              /* Cao nhat: dam bao I2S DMA khong underrun */

/* Kich thuoc Stack cua tung Task (Bytes) */
#define STACK_SIZE_UI_RENDER    5120
#define STACK_SIZE_WIFI_WEB     5120
#define STACK_SIZE_HTTP_STREAM  6144
#define STACK_SIZE_BTN_CTRL     3072
#define STACK_SIZE_AUDIO_I2S    6144

/* Cau hinh StreamBuffer (16 KB) & Pre-buffer (4 KB) - toi uu RAM cho lwIP & Web Server */
#define STREAM_BUF_SIZE_BYTES       (16 * 1024)
#define STREAM_BUF_TRIGGER_BYTES    512
#define STREAM_PREBUFFER_BYTES      (4 * 1024)

/* Cau hinh I2S DMA chuan driver/i2s_std.h */
#define I2S_DMA_DESC_NUM            8
#define I2S_DMA_FRAME_NUM           512
#define I2S_DEFAULT_SAMPLE_RATE     44100

/* =========================================================================
 * 3. BIT DONG BO EVENT GROUP & TASK NOTIFY (ZERO-BLOCKING / NO POLLING)
 * ========================================================================= */
/* EventGroup bits (g_radio_event_group) */
#define EVT_WIFI_CONNECTED          (1U << 0)
#define EVT_PLAY_REQUESTED          (1U << 1)
#define EVT_PREBUFFER_READY         (1U << 2)
#define EVT_STREAM_ABORT_REQ        (1U << 3)
#define EVT_STATION_CHANGED         (1U << 4)

/* TaskNotify bits gui toi ui_render_task */
#define UI_NOTIFY_PLAY_STATE        (1UL << 0)
#define UI_NOTIFY_METADATA          (1UL << 1)
#define UI_NOTIFY_STATION           (1UL << 2)
#define UI_NOTIFY_VOLUME            (1UL << 3)
#define UI_NOTIFY_WIFI              (1UL << 4)
#define UI_NOTIFY_BUFFERING         (1UL << 5)
#define UI_NOTIFY_VIDEO_EXIT        (1UL << 6)
#define UI_NOTIFY_VIDEO_FRAME       (1UL << 7)

/* =========================================================================
 * 4. STRUCT TRANG THAI HE THONG TOI UU RAM (STATIC ALLOCATION)
 * ========================================================================= */
typedef struct {
    /* Chuoi ky tu tinh co dinh (Zero malloc/String) - bao ve boi mutex */
    char                station_name[32];
    char                song_title[128];
    char                stream_url[160];
    char                sta_ip[20];

    /* Cac bien trang thai nguyen tu (<stdatomic.h>) truy cap khong can lock */
    atomic_bool         is_playing;         /* true = PLAY, false = PAUSE */
    atomic_bool         is_buffering;       /* true = Dang nap Pre-buffer >= 50% */
    atomic_bool         wifi_connected;     /* true = STA da co IP */
    atomic_bool         is_video_mode;      /* true = Dang stream video MP4 tu Web UI */

    /* Bien so nguyen kich thuoc nho nhat */
    uint8_t             volume;             /* 0..100 (%) */
    uint8_t             station_index;      /* Chi so kenh radio hien tai */
    uint8_t             lcd_mode;           /* 0 = ST7789 (240x240), 1 = ST7735 (128x160) */
    int8_t              wifi_rssi;          /* Cuong do song WiFi (dBm) */
    uint16_t            disc_angle;         /* Goc xoay dia than 0..359 do */

    /* Mutex tinh bao ve truy cap chuoi station_name, song_title, stream_url */
    SemaphoreHandle_t   mutex;
    StaticSemaphore_t   mutex_storage;
} radio_system_state_t;

/* Danh sach kenh Radio mac dinh luu trong Flash */
typedef struct {
    const char *name;
    const char *url;
} radio_station_preset_t;

/* =========================================================================
 * 5. BIEN TOAN CUC TINH & API TRUY CAP AN TOAN LUONG
 * ========================================================================= */
extern radio_system_state_t     g_radio_state;
extern EventGroupHandle_t       g_radio_event_group;
extern StreamBufferHandle_t     g_audio_stream_buf;

extern TaskHandle_t             g_ui_task_handle;
extern TaskHandle_t             g_http_task_handle;
extern TaskHandle_t             g_audio_task_handle;
extern TaskHandle_t             g_btn_task_handle;

void radio_state_set_station(const char *name, const char *url, uint8_t index);
void radio_state_set_song_title(const char *title);
void radio_state_get_strings(char *out_station, size_t station_len,
                             char *out_title, size_t title_len);
void radio_state_set_ip(const char *ip_str);
void radio_state_get_ip(char *out_ip, size_t ip_len);
void radio_state_set_volume(uint8_t vol_pct);
void radio_state_cycle_station(int delta);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_CONFIG_H */
