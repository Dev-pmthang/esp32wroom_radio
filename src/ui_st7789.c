/*
 * ui_st7789.c
 * Giao dien "Vinyl Player - Now Playing" (Warm Retro Analog Aesthetics)
 * tren man hinh ST7789 240x240 su dung driver goc esp_lcd (SPI DMA):
 *   - Hien thi dia chi IP ngay goc trai man hinh (Y: 0..15) sau khi ket noi WiFi
 *   - Nen kem giay am (#F4F1EA), Dia than Den Tuyen (#000000) + Bong do Ellipse (#D4CFC4)
 *   - Nhan tam Do Gach Terracotta (#C24B38) + Can kim Vang Dong thang (#C0A266)
 *   - Dirty-Rectangle Direct Render qua esp_lcd_panel_draw_bitmap()
 *   - Bang tra cuu luong giac const int16_t sin_lut[360], cos_lut[360] (Fixed-Point >> 10)
 *   - Zero float, Zero runtime malloc, Zero polling (dong bo qua xTaskNotifyWait)
 */
#include "ui_st7789.h"
#include "radio_config.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <string.h>
#include <stdio.h>
#include <limits.h>

/* =========================================================================
 * Macro chuyen doi RGB888 sang RGB565 Big-Endian (chuan SPI ST7789 DMA)
 * Tinh toan hoan toan tai compile-time (Zero runtime byte-swap overhead)
 * ========================================================================= */
#define RAW_RGB565(r, g, b) \
    ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | (((uint16_t)(g) & 0xFCU) << 3) | (((uint16_t)(b) & 0xF8U) >> 3)))
#define BE_RGB565(r, g, b) \
    ((uint16_t)((RAW_RGB565(r, g, b) >> 8) | (RAW_RGB565(r, g, b) << 8)))

/* =========================================================================
 * Bang mau "Vinyl Player - Now Playing" (Warm Retro Analog Aesthetics)
 * ========================================================================= */
#define COL_BG_CREAM         BE_RGB565(244, 241, 234)  /* #F4F1EA Nen giay kem am */
#define COL_SHADOW_CORE      BE_RGB565(212, 207, 196)  /* #D4CFC4 Bong do ellipse duoi dia */
#define COL_SHADOW_SOFT      BE_RGB565(228, 224, 215)  /* #E4E0D7 Vien mem bong do */

#define COL_VINYL_BLACK      0x0000U                   /* #000000 Mat dia than DEN TUYEN */
#define COL_VINYL_RIM        BE_RGB565(26,  26,  26)   /* #1A1A1A Vien ngoai dia than */
#define COL_VINYL_SHEEN      BE_RGB565(18,  18,  18)   /* #121212 Anh bong nhe tren nen den */
#define COL_VINYL_GROOVE     BE_RGB565(50,  50,  50)   /* #323232 Ranh dong tam sac net */
#define COL_GROOVE_GLINT1    BE_RGB565(82,  82,  82)   /* #525252 Ranh bat sang mem */
#define COL_GROOVE_GLINT2    BE_RGB565(115, 115, 115)  /* #737373 Ranh phan quang khi quay */

#define COL_LABEL_TERRA      BE_RGB565(194, 75,  56)   /* #C24B38 Do cam dat nung (Terracotta) */
#define COL_LABEL_DARK       BE_RGB565(166, 58,  42)   /* #A63A2A Vien nhan tam */
#define COL_LABEL_RING       BE_RGB565(224, 152, 136)  /* #E09888 Vong chi kem hong trong nhan */
#define COL_LABEL_TEXT       BE_RGB565(250, 245, 238)  /* #FAF5EE Chu trang kem "SLOW RIVER" */
#define COL_SPINDLE_BLACK    0x0000U                   /* #000000 Lo truc tam mau den */

#define COL_TONEARM_GOLD     BE_RGB565(192, 162, 102)  /* #C0A266 Can kim vang dong (Brass/Gold) */
#define COL_TONEARM_EDGE     BE_RGB565(156, 128, 72)   /* #9C8048 Vien chot xoay can kim */

#define COL_TEXT_DARK        BE_RGB565(28,  28,  28)   /* #1C1C1C Chu tieu de dam & icon */
#define COL_TEXT_MUTED       BE_RGB565(132, 126, 118)  /* #847E76 Chu phu xam am */
#define COL_SLIDER_TRACK     BE_RGB565(208, 203, 194)  /* #D0CBC2 Thanh tien trinh xam nhat */
#define COL_BTN_CIRCLE       BE_RGB565(228, 224, 215)  /* #E4E0D7 Nen tron nut Prev/Next */
#define COL_WIFI_ONLINE      BE_RGB565(46,  148, 74)   /* #2E944A Cham xanh ket noi WiFi */
#define COL_PURE_WHITE       0xFFFFU                   /* #FFFFFF Tam giac Play / Pause trang */

/* =========================================================================
 * Kich thuoc 5 vung Dirty-Rectangle khong chong lan tren man hinh 240x240
 * ========================================================================= */
#define TOP_IP_BAR_Y         0       /* Y: 0..15 (16 px: Dia chi IP goc trai khi co WiFi) */
#define TOP_IP_BAR_H         16

#define VINYL_W              148
#define VINYL_H              132
#define VINYL_STRIP_H        33      /* 132 / 4 = 33 dong moi strip DMA */
#define VINYL_SCREEN_X       46      /* (240 - 148) / 2 = 46 */
#define VINYL_SCREEN_Y       16      /* Y: 16..147 */
#define VINYL_CX             74      /* Tam X trong vung 148x132 (Man hinh X = 120) */
#define VINYL_CY             58      /* Tam Y trong vung 148x132 (Man hinh Y = 74) */
#define VINYL_OUTER_R        54      /* Duong kinh dia than = 108 px */
#define VINYL_LABEL_R        21      /* Duong kinh nhan tam = 42 px */
#define VINYL_HOLE_R         3       /* Lo truc den o giua */

#define TITLE_BAR_Y          148     /* Y: 148..179 (32 px: Title + Subtitle) */
#define TITLE_BAR_H          32

#define SLIDER_BAR_Y         180     /* Y: 180..199 (20 px: Thanh tien trinh + Time/Vol) */
#define SLIDER_BAR_H         20

#define BOTTOM_BAR_Y         200     /* Y: 200..239 (40 px: 5 nut dieu khien tron) */
#define BOTTOM_BAR_H         40

/* =========================================================================
 * Bang tra cuu Luong giac Fixed-Point (Scale = 1024 = 1 << 10) luu trong Flash
 * sin_lut[deg] = round(sin(deg * PI / 180) * 1024)
 * cos_lut[deg] = round(cos(deg * PI / 180) * 1024)
 * ========================================================================= */
static const int16_t sin_lut[360] = {
       0,   18,   36,   54,   71,   89,  107,  125,  143,  160,  178,  195,  213,  230,  248,  265,
     282,  299,  316,  333,  350,  367,  384,  400,  416,  433,  449,  465,  481,  496,  512,  527,
     543,  558,  573,  587,  602,  616,  630,  644,  658,  672,  685,  698,  711,  724,  737,  749,
     761,  773,  784,  796,  807,  818,  828,  839,  849,  859,  868,  878,  887,  896,  904,  912,
     920,  928,  935,  943,  949,  956,  962,  968,  974,  979,  984,  989,  994,  998, 1002, 1005,
    1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024, 1024, 1024, 1023, 1023, 1022, 1020,
    1018, 1016, 1014, 1011, 1008, 1005, 1002,  998,  994,  989,  984,  979,  974,  968,  962,  956,
     949,  943,  935,  928,  920,  912,  904,  896,  887,  878,  868,  859,  849,  839,  828,  818,
     807,  796,  784,  773,  761,  749,  737,  724,  711,  698,  685,  672,  658,  644,  630,  616,
     602,  587,  573,  558,  543,  527,  512,  496,  481,  465,  449,  433,  416,  400,  384,  367,
     350,  333,  316,  299,  282,  265,  248,  230,  213,  195,  178,  160,  143,  125,  107,   89,
      71,   54,   36,   18,    0,  -18,  -36,  -54,  -71,  -89, -107, -125, -143, -160, -178, -195,
    -213, -230, -248, -265, -282, -299, -316, -333, -350, -367, -384, -400, -416, -433, -449, -465,
    -481, -496, -512, -527, -543, -558, -573, -587, -602, -616, -630, -644, -658, -672, -685, -698,
    -711, -724, -737, -749, -761, -773, -784, -796, -807, -818, -828, -839, -849, -859, -868, -878,
    -887, -896, -904, -912, -920, -928, -935, -943, -949, -956, -962, -968, -974, -979, -984, -989,
    -994, -998,-1002,-1005,-1008,-1011,-1014,-1016,-1018,-1020,-1022,-1023,-1023,-1024,-1024,-1024,
   -1023,-1023,-1022,-1020,-1018,-1016,-1014,-1011,-1008,-1005,-1002, -998, -994, -989, -984, -979,
    -974, -968, -962, -956, -949, -943, -935, -928, -920, -912, -904, -896, -887, -878, -868, -859,
    -849, -839, -828, -818, -807, -796, -784, -773, -761, -749, -737, -724, -711, -698, -685, -672,
    -658, -644, -630, -616, -602, -587, -573, -558, -543, -527, -512, -496, -481, -465, -449, -433,
    -416, -400, -384, -367, -350, -333, -316, -299, -282, -265, -248, -230, -213, -195, -178, -160,
    -143, -125, -107,  -89,  -71,  -54,  -36,  -18
};

static const int16_t cos_lut[360] = {
    1024, 1024, 1023, 1023, 1022, 1020, 1018, 1016, 1014, 1011, 1008, 1005, 1002,  998,  994,  989,
     984,  979,  974,  968,  962,  956,  949,  943,  935,  928,  920,  912,  904,  896,  887,  878,
     868,  859,  849,  839,  828,  818,  807,  796,  784,  773,  761,  749,  737,  724,  711,  698,
     685,  672,  658,  644,  630,  616,  602,  587,  573,  558,  543,  527,  512,  496,  481,  465,
     449,  433,  416,  400,  384,  367,  350,  333,  316,  299,  282,  265,  248,  230,  213,  195,
     178,  160,  143,  125,  107,   89,   71,   54,   36,   18,    0,  -18,  -36,  -54,  -71,  -89,
    -107, -125, -143, -160, -178, -195, -213, -230, -248, -265, -282, -299, -316, -333, -350, -367,
    -384, -400, -416, -433, -449, -465, -481, -496, -512, -527, -543, -558, -573, -587, -602, -616,
    -630, -644, -658, -672, -685, -698, -711, -724, -737, -749, -761, -773, -784, -796, -807, -818,
    -828, -839, -849, -859, -868, -878, -887, -896, -904, -912, -920, -928, -935, -943, -949, -956,
    -962, -968, -974, -979, -984, -989, -994, -998,-1002,-1005,-1008,-1011,-1014,-1016,-1018,-1020,
   -1022,-1023,-1023,-1024,-1024,-1024,-1023,-1023,-1022,-1020,-1018,-1016,-1014,-1011,-1008,-1005,
   -1002, -998, -994, -989, -984, -979, -974, -968, -962, -956, -949, -943, -935, -928, -920, -912,
    -904, -896, -887, -878, -868, -859, -849, -839, -828, -818, -807, -796, -784, -773, -761, -749,
    -737, -724, -711, -698, -685, -672, -658, -644, -630, -616, -602, -587, -573, -558, -543, -527,
    -512, -496, -481, -465, -449, -433, -416, -400, -384, -367, -350, -333, -316, -299, -282, -265,
    -248, -230, -213, -195, -178, -160, -143, -125, -107,  -89,  -71,  -54,  -36,  -18,    0,   18,
      36,   54,   71,   89,  107,  125,  143,  160,  178,  195,  213,  230,  248,  265,  282,  299,
     316,  333,  350,  367,  384,  400,  416,  433,  449,  465,  481,  496,  512,  527,  543,  558,
     573,  587,  602,  616,  630,  644,  658,  672,  685,  698,  711,  724,  737,  749,  761,  773,
     784,  796,  807,  818,  828,  839,  849,  859,  868,  878,  887,  896,  904,  912,  920,  928,
     935,  943,  949,  956,  962,  968,  974,  979,  984,  989,  994,  998, 1002, 1005, 1008, 1011,
    1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024
};

/* =========================================================================
 * Font ASCII 5x7 day du (0x20..0x7E: Hoa, Thuong, So, Ky hieu) luu trong Flash
 * ========================================================================= */
static const uint8_t kFont5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x08,0x04,0x08,0x10,0x08}
};

/* =========================================================================
 * Bo dem DMA tinh cho Dirty-Rectangle Rendering (Zero Runtime Malloc)
 * ========================================================================= */
static DMA_ATTR uint16_t s_bar_buf[TFT_WIDTH * BOTTOM_BAR_H];         /* 240x40 = 19,200 B */
static DMA_ATTR uint16_t s_vinyl_strip_buf[VINYL_W * VINYL_STRIP_H];  /* 148x33 =  9,768 B */

static esp_lcd_panel_io_handle_t s_io_handle    = NULL;
static esp_lcd_panel_handle_t    s_panel_handle = NULL;
static SemaphoreHandle_t         s_dma_sem      = NULL;
static StaticSemaphore_t         s_dma_sem_storage;

/* Callback IRAM_ATTR duoc esp_lcd goi ngay khi SPI DMA truyen xong 1 Dirty-Rectangle */
static bool IRAM_ATTR on_lcd_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                              esp_lcd_panel_io_event_data_t *edata,
                                              void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t high_task_woken = pdFALSE;
    if (s_dma_sem != NULL) {
        xSemaphoreGiveFromISR(s_dma_sem, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

/* Day Dirty-Rectangle qua esp_lcd_panel_draw_bitmap va cho DMA hoan tat (Zero-Polling) */
static void lcd_draw_bitmap_dma_sync(int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    if (esp_lcd_panel_draw_bitmap(s_panel_handle, x_start, y_start, x_end, y_end, color_data) == ESP_OK) {
        (void)xSemaphoreTake(s_dma_sem, pdMS_TO_TICKS(100));
    }
}

/* =========================================================================
 * Cac ham ve do hoa 2D tren Bo dem Tinh (In-RAM Dirty Region Primitives)
 * ========================================================================= */
static inline int32_t i32_abs(int32_t v)
{
    return (v < 0) ? -v : v;
}

static void buf_fill_rect(uint16_t *buf, int16_t bw, int16_t bh,
                          int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > bw) { w = bw - x; }
    if (y + h > bh) { h = bh - y; }
    if (w <= 0 || h <= 0) return;

    for (int16_t r = 0; r < h; r++) {
        uint16_t *row = &buf[(y + r) * bw + x];
        for (int16_t c = 0; c < w; c++) {
            row[c] = col;
        }
    }
}

static inline void buf_put_pixel(uint16_t *buf, int16_t bw, int16_t bh,
                                 int16_t x, int16_t y, uint16_t col)
{
    if (x >= 0 && x < bw && y >= 0 && y < bh) {
        buf[y * bw + x] = col;
    }
}

static void buf_fill_circle(uint16_t *buf, int16_t bw, int16_t bh,
                            int16_t cx, int16_t cy, int16_t r, uint16_t col)
{
    int32_t r2 = (int32_t)r * r;
    for (int16_t dy = -r; dy <= r; dy++) {
        int16_t py = cy + dy;
        if (py < 0 || py >= bh) continue;
        for (int16_t dx = -r; dx <= r; dx++) {
            if ((int32_t)dx * dx + (int32_t)dy * dy <= r2) {
                int16_t px = cx + dx;
                if (px >= 0 && px < bw) {
                    buf[py * bw + px] = col;
                }
            }
        }
    }
}

static void buf_draw_line(uint16_t *buf, int16_t bw, int16_t bh,
                          int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t col)
{
    int16_t dx = (int16_t)i32_abs(x1 - x0);
    int16_t dy = (int16_t)i32_abs(y1 - y0);
    int16_t steps = (dx > dy) ? dx : dy;
    if (steps == 0) {
        buf_put_pixel(buf, bw, bh, x0, y0, col);
        return;
    }
    for (int16_t i = 0; i <= steps; i++) {
        int16_t px = x0 + (int16_t)(((int32_t)(x1 - x0) * i) / steps);
        int16_t py = y0 + (int16_t)(((int32_t)(y1 - y0) * i) / steps);
        buf_put_pixel(buf, bw, bh, px, py, col);
    }
}

static void buf_draw_wide_line_strip(uint16_t *buf, int16_t bw, int16_t strip_y0, int16_t strip_h,
                                     int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                     int16_t r, uint16_t col)
{
    int16_t dx = (int16_t)i32_abs(x1 - x0);
    int16_t dy = (int16_t)i32_abs(y1 - y0);
    int16_t steps = (dx > dy) ? dx : dy;
    if (steps == 0) steps = 1;

    for (int16_t i = 0; i <= steps; i++) {
        int16_t px = x0 + (int16_t)(((int32_t)(x1 - x0) * i) / steps);
        int16_t py = y0 + (int16_t)(((int32_t)(y1 - y0) * i) / steps);
        for (int16_t oy = -r; oy <= r; oy++) {
            int16_t ly = (py + oy) - strip_y0;
            if (ly < 0 || ly >= strip_h) continue;
            for (int16_t ox = -r; ox <= r; ox++) {
                if (ox * ox + oy * oy <= r * r + 1) {
                    buf_put_pixel(buf, bw, strip_h, px + ox, ly, col);
                }
            }
        }
    }
}

static int16_t text_pixel_width(const char *str, uint8_t scale)
{
    if (str == NULL || str[0] == '\0') return 0;
    size_t len = strlen(str);
    return (int16_t)(len * 6U * scale - scale);
}

static void buf_draw_text(uint16_t *buf, int16_t bw, int16_t bh,
                          const char *str, int16_t x, int16_t y,
                          uint8_t scale, uint16_t col)
{
    if (str == NULL) return;
    for (size_t i = 0; str[i] != '\0'; i++) {
        int16_t char_x = x + (int16_t)(i * 6U * scale);
        if (char_x >= bw) break;
        if (char_x + 5 * scale < 0) continue;

        uint8_t c = (uint8_t)str[i];
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *glyph = kFont5x7[c - 0x20];

        for (int16_t col_idx = 0; col_idx < 5; col_idx++) {
            uint8_t bits = glyph[col_idx];
            for (int16_t row_idx = 0; row_idx < 7; row_idx++) {
                if ((bits & (1U << row_idx)) != 0) {
                    buf_fill_rect(buf, bw, bh,
                                  char_x + col_idx * scale,
                                  y + row_idx * scale,
                                  scale, scale, col);
                }
            }
        }
    }
}

/* =========================================================================
 * 1. DIRTY-RECTANGLE: TOP-LEFT WIFI IP ADDRESS (Y: 0..15)
 * Hien thi dia chi IP ngay goc trai man hinh sau khi da ket noi duoc WiFi
 * ========================================================================= */
static void render_top_ip_dirty_rect(bool wifi_ok, const char *ip_str)
{
    buf_fill_rect(s_bar_buf, TFT_WIDTH, TOP_IP_BAR_H, 0, 0, TFT_WIDTH, TOP_IP_BAR_H, COL_BG_CREAM);

    if (wifi_ok && ip_str != NULL && ip_str[0] != '\0') {
        /* Cham tron xanh bao ket noi WiFi + Dia chi IP ngay goc trai (X=6, Y=4) */
        buf_fill_circle(s_bar_buf, TFT_WIDTH, TOP_IP_BAR_H, 8, 7, 2, COL_WIFI_ONLINE);
        buf_draw_text(s_bar_buf, TFT_WIDTH, TOP_IP_BAR_H, ip_str, 14, 4, 1, COL_TEXT_DARK);
    }

    lcd_draw_bitmap_dma_sync(0, TOP_IP_BAR_Y, TFT_WIDTH, TOP_IP_BAR_Y + TOP_IP_BAR_H, s_bar_buf);
}

/* =========================================================================
 * 2. DIRTY-RECTANGLE: PITCH-BLACK VINYL + SHADOW + GOLD TONEARM (Y: 16..147)
 * Thiet ke giong het anh mau "Vinyl Player - Now Playing":
 *   - Nen kem #F4F1EA + Bong do ellipse mem #D4CFC4 duoi day dia
 *   - Dia than Den Tuyen (#000000) voi 6 vong ranh dong tam sac net
 *   - Nhan tam Do Gach Terracotta (#C24B38), vong kem trong & chu "SLOW RIVER" xoay
 *   - Can kim Vang Dong thang (#C0A266) voi chot tron goc tren phai
 * ========================================================================= */
static void render_vinyl_dirty_rect(uint16_t angle_deg, bool is_playing)
{
    uint16_t deg = angle_deg % 360U;
    int32_t cos_a = cos_lut[deg];
    int32_t sin_a = sin_lut[deg];

    const int16_t cx = VINYL_CX; /* 74 */
    const int16_t cy = VINYL_CY; /* 58 */

    const int32_t r_vinyl_outer_sq = VINYL_OUTER_R * VINYL_OUTER_R;               /* 54^2 = 2916 */
    const int32_t r_vinyl_rim_sq   = (VINYL_OUTER_R - 2) * (VINYL_OUTER_R - 2);   /* 52^2 = 2704 */
    const int32_t r_label_outer_sq = VINYL_LABEL_R * VINYL_LABEL_R;               /* 21^2 =  441 */
    const int32_t r_label_ring1_sq = (VINYL_LABEL_R - 3) * (VINYL_LABEL_R - 3);   /* 18^2 =  324 */
    const int32_t r_label_ring2_sq = (VINYL_LABEL_R - 4) * (VINYL_LABEL_R - 4);   /* 17^2 =  289 */
    const int32_t r_hole_sq        = VINYL_HOLE_R * VINYL_HOLE_R;                 /*  3^2 =    9 */

    /* Toa do bong do Ellipse nam ngay duoi dia than (sy = 120, a = 51, b = 5) */
    const int16_t shadow_y  = 120;
    const int32_t shadow_a2 = 51 * 51; /* 2601 */
    const int32_t shadow_b2 = 5 * 5;   /*   25 */
    const int32_t shadow_core_lim = shadow_a2 * shadow_b2;        /* 65,025 */
    const int32_t shadow_soft_lim = shadow_core_lim + 18000;      /* 83,025 */

    /* Toa do Can kim Vang Dong thang (Straight Gold Tonearm) o goc tren phai */
    const int16_t pivot_x = 124;
    const int16_t pivot_y = 9;
    const int16_t tip_x   = is_playing ? 90 : 118;
    const int16_t tip_y   = is_playing ? 46 : 52;

    /* Render 148x132 theo 4 strip 148x33 qua SPI DMA */
    for (int16_t strip = 0; strip < 4; strip++) {
        int16_t y0 = strip * VINYL_STRIP_H;

        for (int16_t sy = 0; sy < VINYL_STRIP_H; sy++) {
            int16_t y   = y0 + sy;
            int32_t dy  = y - cy;
            int32_t dy2 = dy * dy;
            int32_t sdy = y - shadow_y;
            int32_t sdy_term = (sdy * sdy) * shadow_a2;
            uint16_t *line = &s_vinyl_strip_buf[sy * VINYL_W];

            for (int16_t x = 0; x < VINYL_W; x++) {
                int32_t dx = x - cx;
                int32_t d2 = dx * dx + dy2;

                if (d2 > r_vinyl_outer_sq) {
                    /* Ngoai dia than: Kiem tra vung bong do Ellipse phia duoi */
                    if (y >= 112 && y <= 128) {
                        int32_t ell = (dx * dx) * shadow_b2 + sdy_term;
                        if (ell <= shadow_core_lim) {
                            line[x] = COL_SHADOW_CORE;
                        } else if (ell <= shadow_soft_lim) {
                            line[x] = COL_SHADOW_SOFT;
                        } else {
                            line[x] = COL_BG_CREAM;
                        }
                    } else {
                        line[x] = COL_BG_CREAM;
                    }
                } else if (d2 > r_vinyl_rim_sq) {
                    /* Vien ngoai dia than */
                    line[x] = COL_VINYL_RIM;
                } else if (d2 <= r_hole_sq) {
                    /* Lo truc giua mau den tuyen */
                    line[x] = COL_SPINDLE_BLACK;
                } else if (d2 <= r_label_outer_sq) {
                    /* Nhan tam Terracotta (#C24B38) voi vong chi kem & chu xoay */
                    if (d2 >= r_label_ring2_sq && d2 <= r_label_ring1_sq) {
                        line[x] = COL_LABEL_RING;
                    } else if (d2 > r_label_outer_sq - 32) {
                        line[x] = COL_LABEL_DARK;
                    } else {
                        int32_t rx_fp = (dx * cos_a + dy * sin_a) >> 10;
                        int32_t ry_fp = (-dx * sin_a + dy * cos_a) >> 10;
                        int32_t abs_rx = i32_abs(rx_fp);

                        /* Dong chu lon "SLOW RIVER" phia tren lo truc (xoay theo dia) */
                        if (ry_fp >= -9 && ry_fp <= -6 && abs_rx <= 11) {
                            line[x] = (((rx_fp + 16) & 3) != 0) ? COL_LABEL_TEXT : COL_LABEL_TERRA;
                        }
                        /* 2 dong chu nho "June Halberd" / "SIDE A . 33 RPM" phia duoi lo truc */
                        else if (ry_fp == 6 && abs_rx <= 8 && ((rx_fp + 12) & 3) != 0) {
                            line[x] = COL_LABEL_RING;
                        } else if (ry_fp == 9 && abs_rx <= 9 && abs_rx != 1) {
                            line[x] = COL_LABEL_RING;
                        } else {
                            line[x] = COL_LABEL_TERRA;
                        }
                    }
                } else {
                    /* Mat dia than DEN TUYEN (#000000) + 6 vong ranh dong tam sac net */
                    int32_t rx_fp = (dx * cos_a + dy * sin_a) >> 10;
                    int32_t ry_fp = (-dx * sin_a + dy * cos_a) >> 10;
                    int32_t abs_rx = i32_abs(rx_fp);
                    int32_t abs_ry = i32_abs(ry_fp);

                    bool in_spec_core = (abs_ry * 100 < abs_rx * 12);
                    bool in_spec_soft = (abs_ry * 100 < abs_rx * 25);

                    bool is_groove = (d2 >= 25 * 25 - 18 && d2 <= 25 * 25 + 18) ||
                                     (d2 >= 30 * 30 - 22 && d2 <= 30 * 30 + 22) ||
                                     (d2 >= 35 * 35 - 26 && d2 <= 35 * 35 + 26) ||
                                     (d2 >= 40 * 40 - 30 && d2 <= 40 * 40 + 30) ||
                                     (d2 >= 45 * 45 - 34 && d2 <= 45 * 45 + 34) ||
                                     (d2 >= 49 * 49 - 38 && d2 <= 49 * 49 + 38);

                    if (is_groove) {
                        if (in_spec_core)      line[x] = COL_GROOVE_GLINT2;
                        else if (in_spec_soft) line[x] = COL_GROOVE_GLINT1;
                        else                   line[x] = COL_VINYL_GROOVE;
                    } else {
                        if (in_spec_core)      line[x] = COL_VINYL_SHEEN;
                        else                   line[x] = COL_VINYL_BLACK;
                    }
                }
            }
        }

        /* Ve Can kim Vang Dong thang (Straight Gold Tonearm) + Chot xoay tron */
        buf_draw_wide_line_strip(s_vinyl_strip_buf, VINYL_W, y0, VINYL_STRIP_H,
                                 pivot_x, pivot_y, tip_x, tip_y, 1, COL_TONEARM_GOLD);
        buf_draw_wide_line_strip(s_vinyl_strip_buf, VINYL_W, y0, VINYL_STRIP_H,
                                 pivot_x, pivot_y, pivot_x, pivot_y, 5, COL_TONEARM_EDGE);
        buf_draw_wide_line_strip(s_vinyl_strip_buf, VINYL_W, y0, VINYL_STRIP_H,
                                 pivot_x, pivot_y, pivot_x, pivot_y, 4, COL_TONEARM_GOLD);

        /* Day strip Dirty-Rectangle qua esp_lcd_panel_draw_bitmap */
        lcd_draw_bitmap_dma_sync(
            VINYL_SCREEN_X,
            VINYL_SCREEN_Y + y0,
            VINYL_SCREEN_X + VINYL_W,
            VINYL_SCREEN_Y + y0 + VINYL_STRIP_H,
            s_vinyl_strip_buf
        );
    }
}

/* =========================================================================
 * 3. DIRTY-RECTANGLE: SONG TITLE & SUBTITLE (Y: 148..179)
 *   - Dong 1 (Scale 2 Bold #1C1C1C): Ten bai hat ("Slow River"), tu dong Marquee 1px neu dai
 *   - Dong 2 (Scale 1 Muted #847E76): Ten kenh & Nghe si ("Coastal Letters . June Halberd")
 * ========================================================================= */
static void split_artist_and_song(const char *raw_title, const char *station_name,
                                  char *out_song, size_t song_sz,
                                  char *out_subtitle, size_t sub_sz)
{
    const char *sep = strstr(raw_title, " - ");
    if (sep != NULL && sep > raw_title && *(sep + 3) != '\0') {
        size_t artist_len = (size_t)(sep - raw_title);
        char artist[48];
        if (artist_len >= sizeof(artist)) {
            artist_len = sizeof(artist) - 1;
        }
        memcpy(artist, raw_title, artist_len);
        artist[artist_len] = '\0';

        strncpy(out_song, sep + 3, song_sz - 1);
        out_song[song_sz - 1] = '\0';

        snprintf(out_subtitle, sub_sz, "%s . %s", station_name, artist);
    } else {
        strncpy(out_song, raw_title, song_sz - 1);
        out_song[song_sz - 1] = '\0';
        snprintf(out_subtitle, sub_sz, "%s . 33 RPM", station_name);
    }
}

static void render_title_dirty_rect(const char *station_name, const char *raw_title,
                                    int16_t marquee_offset_px, int16_t *out_song_width)
{
    char song_str[96];
    char sub_str[64];
    split_artist_and_song(raw_title, station_name, song_str, sizeof(song_str), sub_str, sizeof(sub_str));

    buf_fill_rect(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, 0, 0, TFT_WIDTH, TITLE_BAR_H, COL_BG_CREAM);

    /* Dong 1: Ten bai hat in dam (Scale = 2, mau #1C1C1C) */
    int16_t tw = text_pixel_width(song_str, 2) + 1;
    if (out_song_width != NULL) {
        *out_song_width = tw;
    }

    int16_t max_visible_w = TFT_WIDTH - 24; /* 216 px */
    int16_t title_y = 2;

    if (tw <= max_visible_w) {
        int16_t tx = (TFT_WIDTH - tw) / 2;
        buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, song_str, tx,     title_y, 2, COL_TEXT_DARK);
        buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, song_str, tx + 1, title_y, 2, COL_TEXT_DARK);
    } else {
        int16_t start_x = 12 - marquee_offset_px;
        buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, song_str, start_x,     title_y, 2, COL_TEXT_DARK);
        buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, song_str, start_x + 1, title_y, 2, COL_TEXT_DARK);

        const int16_t gap_px = 48;
        int16_t second_x = start_x + tw + gap_px;
        if (second_x < TFT_WIDTH - 12) {
            buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, " . ", start_x + tw + 8, title_y, 2, COL_LABEL_TERRA);
            buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, song_str, second_x,     title_y, 2, COL_TEXT_DARK);
            buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, song_str, second_x + 1, title_y, 2, COL_TEXT_DARK);
        }

        /* Le kem 2 ben khi cuon chu Marquee */
        buf_fill_rect(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, 0, 0, 10, 18, COL_BG_CREAM);
        buf_fill_rect(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, TFT_WIDTH - 10, 0, 10, 18, COL_BG_CREAM);
    }

    /* Dong 2: Subtitle "Coastal Letters . June Halberd" (Scale = 1, mau xam am #847E76) */
    int16_t sub_w = text_pixel_width(sub_str, 1);
    int16_t sub_x = (TFT_WIDTH - sub_w) / 2;
    if (sub_x < 6) sub_x = 6;
    buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, sub_str, sub_x, 21, 1, COL_TEXT_MUTED);

    lcd_draw_bitmap_dma_sync(0, TITLE_BAR_Y, TFT_WIDTH, TITLE_BAR_Y + TITLE_BAR_H, s_bar_buf);
}

/* =========================================================================
 * 4. DIRTY-RECTANGLE: MINIMALIST PROGRESS / VOLUME LINE (Y: 180..199)
 *   - Thanh ngang manh (#D0CBC2) + Phan da phat mau Do Terracotta (#C24B38)
 *   - Nut tron Terracotta (r = 4px) + Chi so thoi gian "0:09" va "-3:28" / "VOL"
 * ========================================================================= */
static void render_slider_dirty_rect(uint32_t elapsed_sec, uint8_t volume_pct, bool show_vol_label)
{
    buf_fill_rect(s_bar_buf, TFT_WIDTH, SLIDER_BAR_H, 0, 0, TFT_WIDTH, SLIDER_BAR_H, COL_BG_CREAM);

    const int16_t bar_x0 = 18;
    const int16_t bar_x1 = TFT_WIDTH - 18; /* 222 */
    const int16_t bar_w  = bar_x1 - bar_x0; /* 204 */
    const int16_t bar_y  = 5;

    /* Thanh nen xam nhat 2px */
    buf_fill_rect(s_bar_buf, TFT_WIDTH, SLIDER_BAR_H, bar_x0, bar_y, bar_w, 2, COL_SLIDER_TRACK);

    int16_t knob_offset;
    if (show_vol_label) {
        if (volume_pct > 100U) volume_pct = 100U;
        knob_offset = (int16_t)(((int32_t)bar_w * volume_pct) / 100);
    } else {
        uint32_t pos_sec = (elapsed_sec + 9U) % 218U;
        knob_offset = (int16_t)(((int32_t)bar_w * (int32_t)pos_sec) / 218);
        if (knob_offset < 10) knob_offset = 10;
    }

    int16_t knob_x = bar_x0 + knob_offset;
    buf_fill_rect(s_bar_buf, TFT_WIDTH, SLIDER_BAR_H, bar_x0, bar_y, knob_offset, 2, COL_LABEL_TERRA);
    buf_fill_circle(s_bar_buf, TFT_WIDTH, SLIDER_BAR_H, knob_x, bar_y, 4, COL_LABEL_TERRA);

    /* Thoi gian ben trai ("0:09") */
    uint32_t disp_sec = (elapsed_sec + 9U) % 218U;
    char left_str[12];
    snprintf(left_str, sizeof(left_str), "%u:%02u", (unsigned)(disp_sec / 60U), (unsigned)(disp_sec % 60U));
    buf_draw_text(s_bar_buf, TFT_WIDTH, SLIDER_BAR_H, left_str, bar_x0, 12, 1, COL_TEXT_MUTED);

    /* Chi so ben phai ("-3:28" hoac "VOL 75%") */
    char right_str[16];
    if (show_vol_label) {
        snprintf(right_str, sizeof(right_str), "VOL %u%%", (unsigned)volume_pct);
    } else {
        uint32_t rem_sec = 217U - disp_sec;
        snprintf(right_str, sizeof(right_str), "-%u:%02u", (unsigned)(rem_sec / 60U), (unsigned)(rem_sec % 60U));
    }
    int16_t rw = text_pixel_width(right_str, 1);
    buf_draw_text(s_bar_buf, TFT_WIDTH, SLIDER_BAR_H, right_str, bar_x1 - rw, 12, 1, COL_TEXT_MUTED);

    lcd_draw_bitmap_dma_sync(0, SLIDER_BAR_Y, TFT_WIDTH, SLIDER_BAR_Y + SLIDER_BAR_H, s_bar_buf);
}

/* =========================================================================
 * 5. DIRTY-RECTANGLE: BOTTOM TRANSPORT BAR (Y: 200..239)
 *   - Trai cung (X=30): Icon Shuffle
 *   - Giua trai (X=73): Nut tron xam nhat (#E4E0D7) + Icon Previous (|<)
 *   - Chinh giua (X=120): Nut tron lon Do Terracotta (#C24B38, r=17) + Play/Pause Trang
 *   - Giua phai (X=167): Nut tron xam nhat (#E4E0D7) + Icon Next (>|)
 *   - Phai cung (X=210): Icon Repeat
 * ========================================================================= */
static void draw_shuffle_icon(uint16_t *buf, int16_t bw, int16_t bh, int16_t cx, int16_t cy, uint16_t col)
{
    buf_fill_rect(buf, bw, bh, cx - 6, cy - 3, 3, 1, col);
    buf_draw_line(buf, bw, bh, cx - 3, cy - 3, cx + 3, cy + 3, col);
    buf_fill_rect(buf, bw, bh, cx + 3, cy + 3, 4, 1, col);
    buf_put_pixel(buf, bw, bh, cx + 5, cy + 2, col);
    buf_put_pixel(buf, bw, bh, cx + 4, cy + 1, col);
    buf_put_pixel(buf, bw, bh, cx + 5, cy + 4, col);
    buf_put_pixel(buf, bw, bh, cx + 4, cy + 5, col);

    buf_fill_rect(buf, bw, bh, cx - 6, cy + 3, 3, 1, col);
    buf_draw_line(buf, bw, bh, cx - 3, cy + 3, cx + 3, cy - 3, col);
    buf_fill_rect(buf, bw, bh, cx + 3, cy - 3, 4, 1, col);
    buf_put_pixel(buf, bw, bh, cx + 5, cy - 4, col);
    buf_put_pixel(buf, bw, bh, cx + 4, cy - 5, col);
    buf_put_pixel(buf, bw, bh, cx + 5, cy - 2, col);
    buf_put_pixel(buf, bw, bh, cx + 4, cy - 1, col);
}

static void draw_repeat_icon(uint16_t *buf, int16_t bw, int16_t bh, int16_t cx, int16_t cy, uint16_t col)
{
    buf_fill_rect(buf, bw, bh, cx - 5, cy - 4, 10, 1, col);
    buf_fill_rect(buf, bw, bh, cx - 6, cy - 3, 1, 3, col);
    buf_put_pixel(buf, bw, bh, cx + 3, cy - 6, col);
    buf_put_pixel(buf, bw, bh, cx + 4, cy - 5, col);
    buf_put_pixel(buf, bw, bh, cx + 4, cy - 3, col);
    buf_put_pixel(buf, bw, bh, cx + 3, cy - 2, col);

    buf_fill_rect(buf, bw, bh, cx - 4, cy + 4, 10, 1, col);
    buf_fill_rect(buf, bw, bh, cx + 6, cy + 1, 1, 3, col);
    buf_put_pixel(buf, bw, bh, cx - 3, cy + 2, col);
    buf_put_pixel(buf, bw, bh, cx - 4, cy + 3, col);
    buf_put_pixel(buf, bw, bh, cx - 4, cy + 5, col);
    buf_put_pixel(buf, bw, bh, cx - 3, cy + 6, col);
}

static void render_bottom_dirty_rect(bool is_playing)
{
    buf_fill_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 0, 0, TFT_WIDTH, BOTTOM_BAR_H, COL_BG_CREAM);

    const int16_t cy = 19;

    /* 1. Shuffle Icon ben trai (cx = 30) */
    draw_shuffle_icon(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 30, cy, COL_TEXT_DARK);

    /* 2. Nut Previous tron (cx = 73, r = 12) + Icon |< */
    const int16_t prev_cx = 73;
    buf_fill_circle(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, prev_cx, cy, 12, COL_BTN_CIRCLE);
    buf_fill_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, prev_cx - 4, cy - 4, 1, 9, COL_TEXT_DARK);
    buf_draw_line(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, prev_cx - 2, cy,     prev_cx + 4, cy - 4, COL_TEXT_DARK);
    buf_draw_line(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, prev_cx - 2, cy,     prev_cx + 4, cy + 4, COL_TEXT_DARK);
    buf_draw_line(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, prev_cx + 4, cy - 4, prev_cx + 4, cy + 4, COL_TEXT_DARK);

    /* 3. Nut Play / Pause Tron Lon Terracotta (cx = 120, r = 17) */
    const int16_t play_cx = 120;
    buf_fill_circle(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, play_cx, cy, 18, COL_LABEL_RING);
    buf_fill_circle(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, play_cx, cy, 17, COL_LABEL_TERRA);

    if (!is_playing) {
        /* Khi PAUSED: Hien thi Tam giac Play (▶) trang can doi tuyet doi de nhan Resume */
        for (int16_t dx = 0; dx < 9; dx++) {
            int16_t half_h = (int16_t)((8 - dx) / 2);
            int16_t h = (int16_t)(half_h * 2 + 1);
            buf_fill_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H,
                          play_cx - 3 + dx, cy - half_h, 1, h, COL_PURE_WHITE);
        }
    } else {
        /* Khi PLAYING: Hien thi 2 thanh Pause (||) trang sac net tren nen tron Terracotta */
        buf_fill_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, play_cx - 5, cy - 5, 3, 11, COL_PURE_WHITE);
        buf_fill_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, play_cx + 2, cy - 5, 3, 11, COL_PURE_WHITE);
    }

    /* 4. Nut Next tron (cx = 167, r = 12) + Icon >| */
    const int16_t next_cx = 167;
    buf_fill_circle(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, next_cx, cy, 12, COL_BTN_CIRCLE);
    buf_draw_line(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, next_cx - 4, cy - 4, next_cx - 4, cy + 4, COL_TEXT_DARK);
    buf_draw_line(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, next_cx - 4, cy - 4, next_cx + 2, cy,     COL_TEXT_DARK);
    buf_draw_line(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, next_cx - 4, cy + 4, next_cx + 2, cy,     COL_TEXT_DARK);
    buf_fill_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, next_cx + 4, cy - 4, 1, 9, COL_TEXT_DARK);

    /* 5. Repeat Icon ben phai (cx = 210) */
    draw_repeat_icon(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 210, cy, COL_TEXT_DARK);

    lcd_draw_bitmap_dma_sync(0, BOTTOM_BAR_Y, TFT_WIDTH, BOTTOM_BAR_Y + BOTTOM_BAR_H, s_bar_buf);
}

/* =========================================================================
 * Gui tin hieu TaskNotify danh thuc ui_render_task tuc thi
 * ========================================================================= */
void ui_st7789_notify(uint32_t notify_bits)
{
    if (g_ui_task_handle != NULL) {
        xTaskNotify(g_ui_task_handle, notify_bits, eSetBits);
    }
}

/* =========================================================================
 * CORE 1 (Priority 4): ui_render_task
 * - Khi PLAYING: cap nhat ~30 FPS (timeout 33ms tren xTaskNotifyWait)
 * - Khi PAUSED:  giam tan suat refresh (timeout 500ms) de tiet kiem CPU,
 *                nhung thuc day TUC THI (< 1ms) khi nhan TaskNotify tu GPIO 0!
 * ========================================================================= */
static void ui_render_task(void *arg)
{
    (void)arg;
    char local_station[32];
    char local_title[128];
    char local_ip[20];

    radio_state_get_strings(local_station, sizeof(local_station),
                            local_title, sizeof(local_title));
    radio_state_get_ip(local_ip, sizeof(local_ip));

    int16_t  marquee_offset_px = 0;
    int64_t  marquee_pause_since_us = esp_timer_get_time();
    bool     marquee_in_pause = true;
    int16_t  song_px_width = 0;

    uint32_t elapsed_sec = 0;
    int64_t  last_sec_us = esp_timer_get_time();
    int64_t  vol_overlay_until_us = 0;

    /* Ve lan dau toan bo 5 Dirty-Rectangles */
    bool playing = atomic_load(&g_radio_state.is_playing);
    bool wifi_ok = atomic_load(&g_radio_state.wifi_connected);

    render_top_ip_dirty_rect(wifi_ok, local_ip);
    render_vinyl_dirty_rect(g_radio_state.disc_angle, playing);
    render_title_dirty_rect(local_station, local_title, 0, &song_px_width);
    render_slider_dirty_rect(elapsed_sec, g_radio_state.volume, false);
    render_bottom_dirty_rect(playing);

    for (;;) {
        playing = atomic_load(&g_radio_state.is_playing);
        bool need_marquee = (song_px_width > (TFT_WIDTH - 24));

        /* Khi dang phat: ~30 FPS (33ms). Khi Pause: 500ms (hoac 40ms neu dang cuon chu) */
        TickType_t wait_ticks = playing ? pdMS_TO_TICKS(33)
                                        : (need_marquee ? pdMS_TO_TICKS(40) : pdMS_TO_TICKS(500));

        uint32_t notify_bits = 0;
        (void)xTaskNotifyWait(0UL, ULONG_MAX, &notify_bits, wait_ticks);

        playing = atomic_load(&g_radio_state.is_playing);
        wifi_ok = atomic_load(&g_radio_state.wifi_connected);
        int64_t now_us = esp_timer_get_time();

        /* 1. Neu thay doi trang thai WiFi / IP -> cap nhat ngay IP o goc trai man hinh */
        if ((notify_bits & UI_NOTIFY_WIFI) != 0) {
            radio_state_get_ip(local_ip, sizeof(local_ip));
            render_top_ip_dirty_rect(wifi_ok, local_ip);
        }

        /* 2. Neu thay doi Station hoac Metadata -> doc lai chuoi an toan */
        if ((notify_bits & (UI_NOTIFY_STATION | UI_NOTIFY_METADATA)) != 0) {
            radio_state_get_strings(local_station, sizeof(local_station),
                                    local_title, sizeof(local_title));
            marquee_offset_px = 0;
            marquee_in_pause = true;
            marquee_pause_since_us = now_us;
            render_title_dirty_rect(local_station, local_title, 0, &song_px_width);
        }

        /* 3. Neu thay doi Play/Pause tu GPIO 0 hoac Web -> cap nhat nut tron duoi cung ngay */
        if ((notify_bits & UI_NOTIFY_PLAY_STATE) != 0) {
            render_bottom_dirty_rect(playing);
        }

        /* 4. Neu thay doi Volume -> hien thi VOL XX% tren thanh tien trinh trong 3 giay */
        if ((notify_bits & UI_NOTIFY_VOLUME) != 0) {
            vol_overlay_until_us = now_us + 3000000LL;
            render_slider_dirty_rect(elapsed_sec, g_radio_state.volume, true);
        }

        /* 5. Cap nhat bo dem giay tren thanh tien trinh khi dang phat */
        if (playing && (now_us - last_sec_us) >= 1000000LL) {
            last_sec_us = now_us;
            elapsed_sec++;
            bool show_vol = (now_us < vol_overlay_until_us);
            render_slider_dirty_rect(elapsed_sec, g_radio_state.volume, show_vol);
        }

        /* 6. Vung Dia than: CHI cap nhat khi is_playing == true hoac vua nhan Pause */
        if (playing) {
            g_radio_state.disc_angle = (uint16_t)((g_radio_state.disc_angle + 6U) % 360U);
            render_vinyl_dirty_rect(g_radio_state.disc_angle, true);
        } else if ((notify_bits & UI_NOTIFY_PLAY_STATE) != 0) {
            /* Khi vua chuyen sang PAUSE: ve 1 lan de dung dia ngay tai goc hien tai & nhac can kim */
            render_vinyl_dirty_rect(g_radio_state.disc_angle, false);
        }

        /* 7. Vung Marquee Song Title: Dich chuyen 1 pixel muot ma voi khoang nghi 2s dau cau */
        if (song_px_width > (TFT_WIDTH - 24)) {
            if (marquee_in_pause) {
                if ((now_us - marquee_pause_since_us) >= 2000000LL) {
                    marquee_in_pause = false;
                }
            } else {
                marquee_offset_px += 1;
                if (marquee_offset_px >= (song_px_width + 48)) {
                    marquee_offset_px = 0;
                    marquee_in_pause = true;
                    marquee_pause_since_us = now_us;
                }
                render_title_dirty_rect(local_station, local_title, marquee_offset_px, &song_px_width);
            }
        }
    }
}

/* =========================================================================
 * Khoi tao ST7789 240x240 qua driver goc esp_lcd (SPI DMA, Dirty-Rectangle)
 * ========================================================================= */
esp_err_t ui_st7789_init(void)
{
    s_dma_sem = xSemaphoreCreateBinaryStatic(&s_dma_sem_storage);
    configASSERT(s_dma_sem != NULL);

    /* 1. Cau hinh chan Backlight (GPIO 32), Reset (GPIO 4) & CS phu tro (GPIO 15) */
    const gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << PIN_TFT_BLK) |
                        (1ULL << PIN_TFT_RES) |
                        (1ULL << PIN_TFT_CS_COMPAT_2),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(PIN_TFT_BLK, 0);
    gpio_set_level(PIN_TFT_RES, 1);
    gpio_set_level(PIN_TFT_CS_COMPAT_2, 0);

    /* 2. Khoi tao bus SPI2 (SCL=18, SDA=23, Max transfer = 240*40*2 bytes) */
    const spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_TFT_SDA,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_TFT_SCL,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = TFT_WIDTH * BOTTOM_BAR_H * sizeof(uint16_t),
    };
    err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        printf("[UI_ST7789] ERROR: spi_bus_initialize failed: %s\n", esp_err_to_name(err));
        return err;
    }

    /* 3. Gan Panel IO SPI (CS=GPIO5 tuong thich ca CS=5 va CS-less, DC=2, 27MHz DMA) */
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num         = PIN_TFT_DC,
        .cs_gpio_num         = PIN_TFT_CS_HW,
        .pclk_hz             = TFT_SPI_CLOCK_HZ,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .spi_mode            = 0,
        .trans_queue_depth   = 4,
        .on_color_trans_done = on_lcd_color_trans_done,
        .user_ctx            = NULL,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io_handle);
    if (err != ESP_OK) {
        printf("[UI_ST7789] ERROR: esp_lcd_new_panel_io_spi failed: %s\n", esp_err_to_name(err));
        return err;
    }

    /* 4. Khoi tao Panel ST7789 (de reset_gpio_num = -1 de chu dong timing reset chuan) */
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle);
    if (err != ESP_OK) {
        printf("[UI_ST7789] ERROR: esp_lcd_new_panel_st7789 failed: %s\n", esp_err_to_name(err));
        return err;
    }

    /* Goi esp_lcd_panel_init truoc de khoi tao st7789->fb_bits_per_pixel = 16 */
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    /* Thuc thi chuoi Hardware Reset + Init da kiem chung tren phan cung ST7789 cua ban:
     *   RST LOW 50ms -> HIGH 120ms -> 0x01 (150ms) -> 0x11 (120ms)
     *   -> 0x36 (0x70) -> 0x3A (0x55) -> 0x21 (INVON) -> 0x29 (50ms)
     */
    gpio_set_level(PIN_TFT_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_TFT_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_lcd_panel_io_tx_param(s_io_handle, 0x01, NULL, 0); /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));

    esp_lcd_panel_io_tx_param(s_io_handle, 0x11, NULL, 0); /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));

    const uint8_t madctl_val = 0x70;
    esp_lcd_panel_io_tx_param(s_io_handle, 0x36, &madctl_val, 1); /* MADCTL = 0x70 */

    const uint8_t colmod_val = 0x55;
    esp_lcd_panel_io_tx_param(s_io_handle, 0x3A, &colmod_val, 1); /* COLMOD = 16-bit */

    /* BAT BUOC cho tam nen IPS ST7789: 0x21 (INVON) de 0x0000 hien thi DEN TUYEN & #F4F1EA hien thi KEM SANG */
    esp_lcd_panel_io_tx_param(s_io_handle, 0x21, NULL, 0); /* INVON */
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_lcd_panel_io_tx_param(s_io_handle, 0x29, NULL, 0); /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* To toan bo nen Kem Giay Am (#F4F1EA) theo tung bang 240x40 dong bo DMA va bat BLK=32 */
    for (int y = 0; y < TFT_HEIGHT; y += 40) {
        buf_fill_rect(s_bar_buf, TFT_WIDTH, 40, 0, 0, TFT_WIDTH, 40, COL_BG_CREAM);
        lcd_draw_bitmap_dma_sync(0, y, TFT_WIDTH, y + 40, s_bar_buf);
    }
    gpio_set_level(PIN_TFT_BLK, 1);

    /* 5. Tao ui_render_task tren Core 1 (Priority 4) */
    BaseType_t ok = xTaskCreatePinnedToCore(
        ui_render_task,
        "ui_render_task",
        STACK_SIZE_UI_RENDER,
        NULL,
        TASK_PRIO_UI_RENDER,
        &g_ui_task_handle,
        CORE_REALTIME_AUDIO_UI
    );
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    printf("[UI_ST7789] ST7789 240x240 Vinyl Player UI initialized via esp_lcd (DMA sync active)\n");
    return ESP_OK;
}
