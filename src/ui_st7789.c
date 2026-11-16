/*
 * ui_st7789.c
 * Giao dien "Vinyl Player - Now Playing" (Warm Retro Analog Aesthetics)
 * Ho tro 2 Mode Man hinh LED / TFT qua driver goc esp_lcd (SPI DMA):
 *   - MODE 0: ST7789 1.54" (240x240, 27MHz, CS=GPIO5/None, IPS INVON)
 *   - MODE 1: ST7735 1.8"  (128x160 hoac 160x128, 20MHz, CS=GPIO15, Offset 2,1)
 * Tinh nang:
 *   - Hien thi dia chi IP ngay goc trai man hinh sau khi ket noi WiFi
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
#include "esp32/rom/tjpgd.h"
#include <string.h>
#include <stdio.h>
#include <limits.h>

/* =========================================================================
 * Macro chuyen doi RGB888 sang RGB565 Big-Endian (chuan SPI ST7789/ST7735 DMA)
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

#define COL_TEXT_DARK        0x0000U                   /* #000000 Chu tieu de & icon den sac net */
#define COL_TEXT_MUTED       BE_RGB565(96,  90,  82)   /* #605A52 Chu phu xam dam ro net */
#define COL_SLIDER_TRACK     BE_RGB565(208, 203, 194)  /* #D0CBC2 Thanh tien trinh xam nhat */
#define COL_BTN_CIRCLE       BE_RGB565(228, 224, 215)  /* #E4E0D7 Nen tron nut Prev/Next */
#define COL_WIFI_ONLINE      BE_RGB565(46,  148, 74)   /* #2E944A Cham xanh ket noi WiFi */
#define COL_PURE_WHITE       0xFFFFU                   /* #FFFFFF Tam giac Play / Pause trang */

/* =========================================================================
 * Cau truc Layout Dirty-Rectangle tu dong thich ung ST7789 (240x240) & ST7735 (128x160)
 * ========================================================================= */
typedef struct {
    uint8_t  mode;
    int16_t  width;
    int16_t  height;
    int16_t  top_ip_y;
    int16_t  top_ip_h;
    int16_t  vinyl_x;
    int16_t  vinyl_y;
    int16_t  vinyl_w;
    int16_t  vinyl_h;
    int16_t  vinyl_strip_h;
    int16_t  vinyl_cx;
    int16_t  vinyl_cy;
    int16_t  vinyl_outer_r;
    int16_t  vinyl_label_r;
    int16_t  vinyl_hole_r;
    int16_t  shadow_y;
    int16_t  shadow_a;
    int16_t  shadow_b;
    int16_t  pivot_x;
    int16_t  pivot_y;
    int16_t  tip_play_x;
    int16_t  tip_play_y;
    int16_t  tip_pause_x;
    int16_t  tip_pause_y;
    int16_t  title_y;
    int16_t  title_h;
    uint8_t  title_scale;
    int16_t  slider_y;
    int16_t  slider_h;
    int16_t  bottom_y;
    int16_t  bottom_h;
} lcd_ui_layout_t;

static lcd_ui_layout_t s_ui;

static void configure_ui_layout(uint8_t mode)
{
    if (mode == LCD_MODE_ST7735_128X160) {
        s_ui.mode          = LCD_MODE_ST7735_128X160;
        s_ui.width         = LCD_H_RES;
        s_ui.height        = LCD_V_RES;

        if (LCD_H_RES <= LCD_V_RES) {
            /* ST7735 Portrait 128x160: 5 vung Dirty-Rectangle khop khit 160 dong */
            s_ui.top_ip_y      = 0;
            s_ui.top_ip_h      = 12;  /* Y: 0..11 */
            s_ui.vinyl_w       = 116;
            s_ui.vinyl_h       = 88;  /* Y: 12..99 (4 strip x 22 dong) */
            s_ui.vinyl_strip_h = 22;
            s_ui.vinyl_x       = (LCD_H_RES - 116) / 2; /* 6 */
            s_ui.vinyl_y       = 12;
            s_ui.vinyl_cx      = 58;  /* Tam X tren man hinh = 64 */
            s_ui.vinyl_cy      = 40;
            s_ui.vinyl_outer_r = 38;  /* Duong kinh dia = 76 px (to, ro net) */
            s_ui.vinyl_label_r = 14;  /* Duong kinh nhan = 28 px */
            s_ui.vinyl_hole_r  = 2;
            s_ui.shadow_y      = 82;
            s_ui.shadow_a      = 35;
            s_ui.shadow_b      = 3;
            s_ui.pivot_x       = 98;
            s_ui.pivot_y       = 6;
            s_ui.tip_play_x    = 72;
            s_ui.tip_play_y    = 33;
            s_ui.tip_pause_x   = 93;
            s_ui.tip_pause_y   = 37;
            s_ui.title_y       = 100; /* Y: 100..121 (22 px) */
            s_ui.title_h       = 22;
            s_ui.title_scale   = 1;
            s_ui.slider_y      = 122; /* Y: 122..136 (15 px) */
            s_ui.slider_h      = 15;
            s_ui.bottom_y      = 137; /* Y: 137..159 (23 px) */
            s_ui.bottom_h      = 23;
        } else {
            /* ST7735 Landscape 160x128: 5 vung Dirty-Rectangle khop khit 128 dong */
            s_ui.top_ip_y      = 0;
            s_ui.top_ip_h      = 10;  /* Y: 0..9 */
            s_ui.vinyl_w       = 96;
            s_ui.vinyl_h       = 64;  /* Y: 10..73 (4 strip x 16 dong) */
            s_ui.vinyl_strip_h = 16;
            s_ui.vinyl_x       = (LCD_H_RES - 96) / 2; /* 32 */
            s_ui.vinyl_y       = 10;
            s_ui.vinyl_cx      = 48;
            s_ui.vinyl_cy      = 29;
            s_ui.vinyl_outer_r = 26;
            s_ui.vinyl_label_r = 10;
            s_ui.vinyl_hole_r  = 2;
            s_ui.shadow_y      = 58;
            s_ui.shadow_a      = 25;
            s_ui.shadow_b      = 3;
            s_ui.pivot_x       = 78;
            s_ui.pivot_y       = 5;
            s_ui.tip_play_x    = 58;
            s_ui.tip_play_y    = 24;
            s_ui.tip_pause_x   = 74;
            s_ui.tip_pause_y   = 27;
            s_ui.title_y       = 74;  /* Y: 74..93 (20 px) */
            s_ui.title_h       = 20;
            s_ui.title_scale   = 1;
            s_ui.slider_y      = 94;  /* Y: 94..107 (14 px) */
            s_ui.slider_h      = 14;
            s_ui.bottom_y      = 108; /* Y: 108..127 (20 px) */
            s_ui.bottom_h      = 20;
        }
    } else {
        /* ST7789 240x240: Layout goc 5 vung Dirty-Rectangle khop khit 240 dong */
        s_ui.mode          = LCD_MODE_ST7789_240X240;
        s_ui.width         = 240;
        s_ui.height        = 240;
        s_ui.top_ip_y      = 0;
        s_ui.top_ip_h      = 16;  /* Y: 0..15 */
        s_ui.vinyl_w       = 148;
        s_ui.vinyl_h       = 132; /* Y: 16..147 (4 strip x 33 dong) */
        s_ui.vinyl_strip_h = 33;
        s_ui.vinyl_x       = 46;
        s_ui.vinyl_y       = 16;
        s_ui.vinyl_cx      = 74;
        s_ui.vinyl_cy      = 58;
        s_ui.vinyl_outer_r = 54;
        s_ui.vinyl_label_r = 21;
        s_ui.vinyl_hole_r  = 3;
        s_ui.shadow_y      = 120;
        s_ui.shadow_a      = 51;
        s_ui.shadow_b      = 5;
        s_ui.pivot_x       = 124;
        s_ui.pivot_y       = 9;
        s_ui.tip_play_x    = 90;
        s_ui.tip_play_y    = 46;
        s_ui.tip_pause_x   = 118;
        s_ui.tip_pause_y   = 52;
        s_ui.title_y       = 148; /* Y: 148..179 (32 px) */
        s_ui.title_h       = 32;
        s_ui.title_scale   = 2;
        s_ui.slider_y      = 180; /* Y: 180..199 (20 px) */
        s_ui.slider_h      = 20;
        s_ui.bottom_y      = 200; /* Y: 200..239 (40 px) */
        s_ui.bottom_h      = 40;
    }
}

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
 * Dung chung 1 bo dem DMA 19.2 KB cho tat ca cac vung Dirty-Rectangle
 * tren ca ST7789 (240x240) lan ST7735 (128x160 / 160x128)
 * ========================================================================= */
static DMA_ATTR uint16_t s_bar_buf[TFT_MAX_WIDTH * 40];               /* 240x40 = 19,200 B */
#define s_vinyl_strip_buf s_bar_buf

static esp_lcd_panel_io_handle_t s_io_handle    = NULL;
static esp_lcd_panel_handle_t    s_panel_handle = NULL;
static SemaphoreHandle_t         s_dma_sem      = NULL;
static StaticSemaphore_t         s_dma_sem_storage;
static SemaphoreHandle_t         s_lcd_mutex    = NULL;
static StaticSemaphore_t         s_lcd_mutex_storage;
static volatile int64_t          s_last_video_frame_us = 0;

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
 * 1. DIRTY-RECTANGLE: TOP-LEFT WIFI IP ADDRESS
 * Hien thi dia chi IP ngay goc trai man hinh sau khi da ket noi duoc WiFi
 * ========================================================================= */
static void render_top_ip_dirty_rect(bool wifi_ok, const char *ip_str)
{
    int16_t w = s_ui.width;
    int16_t h = s_ui.top_ip_h;
    buf_fill_rect(s_bar_buf, w, h, 0, 0, w, h, COL_BG_CREAM);

    if (wifi_ok && ip_str != NULL && ip_str[0] != '\0') {
        int16_t dot_y  = h / 2;
        int16_t text_y = (h - 7) / 2;
        if (text_y < 1) text_y = 1;
        buf_fill_circle(s_bar_buf, w, h, 6, dot_y, 2, COL_WIFI_ONLINE);
        buf_draw_text(s_bar_buf, w, h, ip_str, 11, text_y, 1, COL_TEXT_DARK);
    }

    lcd_draw_bitmap_dma_sync(0, s_ui.top_ip_y, w, s_ui.top_ip_y + h, s_bar_buf);
}

/* =========================================================================
 * 2. DIRTY-RECTANGLE: PITCH-BLACK VINYL + SHADOW + GOLD TONEARM
 * Tu dong co gian ty le chuan xac giua ST7789 (240x240) va ST7735 (128x160)
 * ========================================================================= */
static void render_vinyl_dirty_rect(uint16_t angle_deg, bool is_playing)
{
    uint16_t deg = angle_deg % 360U;
    int32_t cos_a = cos_lut[deg];
    int32_t sin_a = sin_lut[deg];

    const int16_t vw      = s_ui.vinyl_w;
    const int16_t strip_h = s_ui.vinyl_strip_h;
    const int16_t cx      = s_ui.vinyl_cx;
    const int16_t cy      = s_ui.vinyl_cy;
    const int16_t ro      = s_ui.vinyl_outer_r;
    const int16_t rl      = s_ui.vinyl_label_r;
    const int16_t rh      = s_ui.vinyl_hole_r;

    const int32_t r_vinyl_outer_sq = (int32_t)ro * ro;
    const int32_t r_vinyl_rim_sq   = (int32_t)(ro - 2) * (ro - 2);
    const int32_t r_label_outer_sq = (int32_t)rl * rl;
    const int32_t r_label_ring1_sq = (int32_t)(rl - 2) * (rl - 2);
    const int32_t r_label_ring2_sq = (int32_t)(rl - 3) * (rl - 3);
    const int32_t r_hole_sq        = (int32_t)rh * rh;

    /* Toa do bong do Ellipse nam ngay duoi dia than */
    const int16_t shadow_y  = s_ui.shadow_y;
    const int32_t shadow_a2 = (int32_t)s_ui.shadow_a * s_ui.shadow_a;
    const int32_t shadow_b2 = (int32_t)s_ui.shadow_b * s_ui.shadow_b;
    const int32_t shadow_core_lim = shadow_a2 * shadow_b2;
    const int32_t shadow_soft_lim = shadow_core_lim + (shadow_core_lim / 3);

    /* Toa do Can kim Vang Dong thang (Straight Gold Tonearm) o goc tren phai */
    const int16_t pivot_x = s_ui.pivot_x;
    const int16_t pivot_y = s_ui.pivot_y;
    const int16_t tip_x   = is_playing ? s_ui.tip_play_x : s_ui.tip_pause_x;
    const int16_t tip_y   = is_playing ? s_ui.tip_play_y : s_ui.tip_pause_y;
    const int16_t pivot_r = (s_ui.mode == LCD_MODE_ST7735_128X160) ? 3 : 5;

    /* Render theo 4 strip ngang qua SPI DMA */
    for (int16_t strip = 0; strip < 4; strip++) {
        int16_t y0 = strip * strip_h;

        for (int16_t sy = 0; sy < strip_h; sy++) {
            int16_t y   = y0 + sy;
            int32_t dy  = y - cy;
            int32_t dy2 = dy * dy;
            int32_t sdy = y - shadow_y;
            int32_t sdy_term = (sdy * sdy) * shadow_a2;
            uint16_t *line = &s_vinyl_strip_buf[sy * vw];

            for (int16_t x = 0; x < vw; x++) {
                int32_t dx = x - cx;
                int32_t d2 = dx * dx + dy2;

                if (d2 > r_vinyl_outer_sq) {
                    /* Ngoai dia than: Kiem tra vung bong do Ellipse phia duoi */
                    if (sdy >= -(s_ui.shadow_b + 2) && sdy <= (s_ui.shadow_b + 2)) {
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
                    line[x] = COL_VINYL_RIM;
                } else if (d2 <= r_hole_sq) {
                    line[x] = COL_SPINDLE_BLACK;
                } else if (d2 <= r_label_outer_sq) {
                    /* Nhan tam Terracotta (#C24B38) voi vong chi kem & vach xoay muot */
                    if (s_ui.mode == LCD_MODE_ST7735_128X160) {
                        int32_t rx_fp = (dx * cos_a + dy * sin_a) >> 10;
                        int32_t ry_fp = (-dx * sin_a + dy * cos_a) >> 10;
                        if (d2 > r_label_outer_sq - 28) {
                            line[x] = COL_LABEL_DARK;
                        } else if (d2 >= 10 * 10 - 10 && d2 <= 10 * 10 + 10) {
                            /* Vong tron lien mach R=10 + cung sang xoay theo dia (Zero pixel snow) */
                            line[x] = (rx_fp > 0 && ry_fp < 0) ? COL_PURE_WHITE : COL_LABEL_RING;
                        } else if (ry_fp >= -5 && ry_fp <= -4 && i32_abs(rx_fp) <= 6) {
                            line[x] = COL_LABEL_TEXT;
                        } else {
                            line[x] = COL_LABEL_TERRA;
                        }
                    } else if (d2 >= r_label_ring2_sq && d2 <= r_label_ring1_sq) {
                        line[x] = COL_LABEL_RING;
                    } else if (d2 > r_label_outer_sq - (rl * 2)) {
                        line[x] = COL_LABEL_DARK;
                    } else {
                        int32_t rx_fp = (dx * cos_a + dy * sin_a) >> 10;
                        int32_t ry_fp = (-dx * sin_a + dy * cos_a) >> 10;
                        int32_t abs_rx = i32_abs(rx_fp);
                        int16_t txt_lim = (rl > 15) ? 11 : 7;

                        if (ry_fp >= -6 && ry_fp <= -4 && abs_rx <= txt_lim) {
                            line[x] = (((rx_fp + 16) & 3) != 0) ? COL_LABEL_TEXT : COL_LABEL_TERRA;
                        } else if (ry_fp == 5 && abs_rx <= (txt_lim - 2) && ((rx_fp + 12) & 3) != 0) {
                            line[x] = COL_LABEL_RING;
                        } else {
                            line[x] = COL_LABEL_TERRA;
                        }
                    }
                } else {
                    /* Mat dia than DEN TUYEN (#000000) + cac vong ranh dong tam lien mach (R*R +- R) */
                    int32_t rx_fp = (dx * cos_a + dy * sin_a) >> 10;
                    int32_t ry_fp = (-dx * sin_a + dy * cos_a) >> 10;
                    int32_t abs_rx = i32_abs(rx_fp);
                    int32_t abs_ry = i32_abs(ry_fp);

                    bool in_spec_core = (abs_ry * 100 < abs_rx * 14);
                    bool in_spec_soft = (abs_ry * 100 < abs_rx * 28);

                    bool is_groove;
                    if (s_ui.mode == LCD_MODE_ST7735_128X160) {
                        is_groove = (d2 >= 20 * 20 - 20 && d2 <= 20 * 20 + 20) ||
                                    (d2 >= 26 * 26 - 26 && d2 <= 26 * 26 + 26) ||
                                    (d2 >= 32 * 32 - 32 && d2 <= 32 * 32 + 32);
                    } else {
                        is_groove = (d2 >= 25 * 25 - 25 && d2 <= 25 * 25 + 25) ||
                                    (d2 >= 30 * 30 - 30 && d2 <= 30 * 30 + 30) ||
                                    (d2 >= 35 * 35 - 35 && d2 <= 35 * 35 + 35) ||
                                    (d2 >= 40 * 40 - 40 && d2 <= 40 * 40 + 40) ||
                                    (d2 >= 45 * 45 - 45 && d2 <= 45 * 45 + 45) ||
                                    (d2 >= 49 * 49 - 49 && d2 <= 49 * 49 + 49);
                    }

                    if (is_groove) {
                        if (in_spec_core)      line[x] = COL_GROOVE_GLINT2;
                        else if (in_spec_soft) line[x] = COL_GROOVE_GLINT1;
                        else                   line[x] = COL_VINYL_GROOVE;
                    } else {
                        if (s_ui.mode != LCD_MODE_ST7735_128X160 && in_spec_core) {
                            line[x] = COL_VINYL_SHEEN;
                        } else {
                            line[x] = COL_VINYL_BLACK;
                        }
                    }
                }
            }
        }

        /* Ve Can kim Vang Dong thang (Straight Gold Tonearm) + Chot xoay tron */
        if (s_ui.mode == LCD_MODE_ST7735_128X160) {
            buf_draw_wide_line_strip(s_vinyl_strip_buf, vw, y0, strip_h,
                                     pivot_x + 2, pivot_y, tip_x + 2, tip_y, 0, COL_TONEARM_EDGE);
            buf_draw_wide_line_strip(s_vinyl_strip_buf, vw, y0, strip_h,
                                     pivot_x, pivot_y, tip_x, tip_y, 0, COL_TONEARM_GOLD);
            buf_draw_wide_line_strip(s_vinyl_strip_buf, vw, y0, strip_h,
                                     pivot_x + 1, pivot_y, tip_x + 1, tip_y, 0, COL_TONEARM_GOLD);
        } else {
            buf_draw_wide_line_strip(s_vinyl_strip_buf, vw, y0, strip_h,
                                     pivot_x, pivot_y, tip_x, tip_y, 1, COL_TONEARM_GOLD);
        }
        buf_draw_wide_line_strip(s_vinyl_strip_buf, vw, y0, strip_h,
                                 pivot_x, pivot_y, pivot_x, pivot_y, pivot_r, COL_TONEARM_EDGE);
        buf_draw_wide_line_strip(s_vinyl_strip_buf, vw, y0, strip_h,
                                 pivot_x, pivot_y, pivot_x, pivot_y, pivot_r - 1, COL_TONEARM_GOLD);

        lcd_draw_bitmap_dma_sync(
            s_ui.vinyl_x,
            s_ui.vinyl_y + y0,
            s_ui.vinyl_x + vw,
            s_ui.vinyl_y + y0 + strip_h,
            s_vinyl_strip_buf
        );
    }
}

/* =========================================================================
 * 3. DIRTY-RECTANGLE: SONG TITLE & SUBTITLE
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

    const int16_t w  = s_ui.width;
    const int16_t h  = s_ui.title_h;
    const uint8_t sc = s_ui.title_scale;

    buf_fill_rect(s_bar_buf, w, h, 0, 0, w, h, COL_BG_CREAM);

    int16_t tw = text_pixel_width(song_str, sc) + ((sc > 1) ? 1 : 0);
    if (out_song_width != NULL) {
        *out_song_width = tw;
    }

    int16_t margin_px     = (s_ui.mode == LCD_MODE_ST7735_128X160) ? 6 : 12;
    int16_t max_visible_w = w - (margin_px * 2);
    int16_t title_y       = 2;
    int16_t sub_y         = (sc == 2) ? 21 : (h - 9);

    if (tw <= max_visible_w) {
        int16_t tx = (w - tw) / 2;
        buf_draw_text(s_bar_buf, w, h, song_str, tx, title_y, sc, COL_TEXT_DARK);
        if (sc > 1) {
            buf_draw_text(s_bar_buf, w, h, song_str, tx + 1, title_y, sc, COL_TEXT_DARK);
        }
    } else {
        int16_t start_x = margin_px - marquee_offset_px;
        buf_draw_text(s_bar_buf, w, h, song_str, start_x, title_y, sc, COL_TEXT_DARK);
        if (sc > 1) {
            buf_draw_text(s_bar_buf, w, h, song_str, start_x + 1, title_y, sc, COL_TEXT_DARK);
        }

        const int16_t gap_px = 36;
        int16_t second_x = start_x + tw + gap_px;
        if (second_x < w - margin_px) {
            buf_draw_text(s_bar_buf, w, h, " . ", start_x + tw + 6, title_y, sc, COL_LABEL_TERRA);
            buf_draw_text(s_bar_buf, w, h, song_str, second_x, title_y, sc, COL_TEXT_DARK);
            if (sc > 1) {
                buf_draw_text(s_bar_buf, w, h, song_str, second_x + 1, title_y, sc, COL_TEXT_DARK);
            }
        }

        buf_fill_rect(s_bar_buf, w, h, 0, 0, margin_px - 1, 8 * sc, COL_BG_CREAM);
        buf_fill_rect(s_bar_buf, w, h, w - (margin_px - 1), 0, margin_px - 1, 8 * sc, COL_BG_CREAM);
    }

    int16_t sub_w = text_pixel_width(sub_str, 1);
    int16_t sub_x = (w - sub_w) / 2;
    if (sub_x < 4) sub_x = 4;
    buf_draw_text(s_bar_buf, w, h, sub_str, sub_x, sub_y, 1, COL_TEXT_MUTED);

    lcd_draw_bitmap_dma_sync(0, s_ui.title_y, w, s_ui.title_y + h, s_bar_buf);
}

/* =========================================================================
 * 4. DIRTY-RECTANGLE: MINIMALIST PROGRESS / VOLUME LINE
 * ========================================================================= */
static void render_slider_dirty_rect(uint32_t elapsed_sec, uint8_t volume_pct, bool show_vol_label)
{
    const int16_t w = s_ui.width;
    const int16_t h = s_ui.slider_h;
    buf_fill_rect(s_bar_buf, w, h, 0, 0, w, h, COL_BG_CREAM);

    const int16_t pad    = (s_ui.mode == LCD_MODE_ST7735_128X160) ? 8 : 18;
    const int16_t bar_x0 = pad;
    const int16_t bar_x1 = w - pad;
    const int16_t bar_w  = bar_x1 - bar_x0;
    const int16_t bar_y  = (s_ui.mode == LCD_MODE_ST7735_128X160) ? 3 : 5;
    const int16_t knob_r = (s_ui.mode == LCD_MODE_ST7735_128X160) ? 3 : 4;
    const int16_t txt_y  = h - 8;

    buf_fill_rect(s_bar_buf, w, h, bar_x0, bar_y, bar_w, 2, COL_SLIDER_TRACK);

    int16_t knob_offset;
    if (show_vol_label) {
        if (volume_pct > 100U) volume_pct = 100U;
        knob_offset = (int16_t)(((int32_t)bar_w * volume_pct) / 100);
    } else {
        uint32_t pos_sec = (elapsed_sec + 9U) % 218U;
        knob_offset = (int16_t)(((int32_t)bar_w * (int32_t)pos_sec) / 218);
        if (knob_offset < 6) knob_offset = 6;
    }

    int16_t knob_x = bar_x0 + knob_offset;
    buf_fill_rect(s_bar_buf, w, h, bar_x0, bar_y, knob_offset, 2, COL_LABEL_TERRA);
    buf_fill_circle(s_bar_buf, w, h, knob_x, bar_y, knob_r, COL_LABEL_TERRA);

    uint32_t disp_sec = (elapsed_sec + 9U) % 218U;
    char left_str[12];
    snprintf(left_str, sizeof(left_str), "%u:%02u", (unsigned)(disp_sec / 60U), (unsigned)(disp_sec % 60U));
    buf_draw_text(s_bar_buf, w, h, left_str, bar_x0, txt_y, 1, COL_TEXT_MUTED);

    char right_str[16];
    if (show_vol_label) {
        snprintf(right_str, sizeof(right_str), "VOL %u%%", (unsigned)volume_pct);
    } else {
        uint32_t rem_sec = 217U - disp_sec;
        snprintf(right_str, sizeof(right_str), "-%u:%02u", (unsigned)(rem_sec / 60U), (unsigned)(rem_sec % 60U));
    }
    int16_t rw = text_pixel_width(right_str, 1);
    buf_draw_text(s_bar_buf, w, h, right_str, bar_x1 - rw, txt_y, 1, COL_TEXT_MUTED);

    lcd_draw_bitmap_dma_sync(0, s_ui.slider_y, w, s_ui.slider_y + h, s_bar_buf);
}

/* =========================================================================
 * 5. DIRTY-RECTANGLE: BOTTOM TRANSPORT BAR
 * ========================================================================= */
static void draw_shuffle_icon(uint16_t *buf, int16_t bw, int16_t bh, int16_t cx, int16_t cy, uint16_t col)
{
    buf_fill_rect(buf, bw, bh, cx - 5, cy - 2, 3, 1, col);
    buf_draw_line(buf, bw, bh, cx - 2, cy - 2, cx + 2, cy + 2, col);
    buf_fill_rect(buf, bw, bh, cx + 2, cy + 2, 3, 1, col);

    buf_fill_rect(buf, bw, bh, cx - 5, cy + 2, 3, 1, col);
    buf_draw_line(buf, bw, bh, cx - 2, cy + 2, cx + 2, cy - 2, col);
    buf_fill_rect(buf, bw, bh, cx + 2, cy - 2, 3, 1, col);
}

static void draw_repeat_icon(uint16_t *buf, int16_t bw, int16_t bh, int16_t cx, int16_t cy, uint16_t col)
{
    buf_fill_rect(buf, bw, bh, cx - 4, cy - 3, 8, 1, col);
    buf_fill_rect(buf, bw, bh, cx - 5, cy - 2, 1, 3, col);
    buf_fill_rect(buf, bw, bh, cx - 3, cy + 3, 8, 1, col);
    buf_fill_rect(buf, bw, bh, cx + 5, cy,     1, 3, col);
}

static void render_bottom_dirty_rect(bool is_playing)
{
    const int16_t w  = s_ui.width;
    const int16_t h  = s_ui.bottom_h;
    const int16_t cy = h / 2;
    const bool    compact = (s_ui.mode == LCD_MODE_ST7735_128X160);

    buf_fill_rect(s_bar_buf, w, h, 0, 0, w, h, COL_BG_CREAM);

    const int16_t shuf_cx = (w * 12) / 100;
    const int16_t prev_cx = (w * 30) / 100;
    const int16_t play_cx = w / 2;
    const int16_t next_cx = (w * 70) / 100;
    const int16_t rept_cx = (w * 88) / 100;

    const int16_t side_r  = compact ? 7 : 12;
    const int16_t play_r  = compact ? 9 : 17;

    /* 1. Shuffle Icon */
    draw_shuffle_icon(s_bar_buf, w, h, shuf_cx, cy, COL_TEXT_DARK);

    /* 2. Nut Previous tron + Icon |< */
    buf_fill_circle(s_bar_buf, w, h, prev_cx, cy, side_r, COL_BTN_CIRCLE);
    buf_fill_rect(s_bar_buf, w, h, prev_cx - 3, cy - 3, 1, 7, COL_TEXT_DARK);
    buf_draw_line(s_bar_buf, w, h, prev_cx - 1, cy,     prev_cx + 3, cy - 3, COL_TEXT_DARK);
    buf_draw_line(s_bar_buf, w, h, prev_cx - 1, cy,     prev_cx + 3, cy + 3, COL_TEXT_DARK);
    buf_draw_line(s_bar_buf, w, h, prev_cx + 3, cy - 3, prev_cx + 3, cy + 3, COL_TEXT_DARK);

    /* 3. Nut Play / Pause Tron Lon Terracotta */
    buf_fill_circle(s_bar_buf, w, h, play_cx, cy, play_r + 1, COL_LABEL_RING);
    buf_fill_circle(s_bar_buf, w, h, play_cx, cy, play_r,     COL_LABEL_TERRA);

    if (!is_playing) {
        int16_t tri_w = compact ? 6 : 9;
        for (int16_t dx = 0; dx < tri_w; dx++) {
            int16_t half_h = (int16_t)((tri_w - 1 - dx) / 2);
            int16_t bar_h  = (int16_t)(half_h * 2 + 1);
            buf_fill_rect(s_bar_buf, w, h,
                          play_cx - (tri_w / 3) + dx, cy - half_h, 1, bar_h, COL_PURE_WHITE);
        }
    } else {
        int16_t bh = compact ? 7 : 11;
        int16_t bw = compact ? 2 : 3;
        buf_fill_rect(s_bar_buf, w, h, play_cx - bw - 1, cy - (bh / 2), bw, bh, COL_PURE_WHITE);
        buf_fill_rect(s_bar_buf, w, h, play_cx + 2,      cy - (bh / 2), bw, bh, COL_PURE_WHITE);
    }

    /* 4. Nut Next tron + Icon >| */
    buf_fill_circle(s_bar_buf, w, h, next_cx, cy, side_r, COL_BTN_CIRCLE);
    buf_draw_line(s_bar_buf, w, h, next_cx - 3, cy - 3, next_cx - 3, cy + 3, COL_TEXT_DARK);
    buf_draw_line(s_bar_buf, w, h, next_cx - 3, cy - 3, next_cx + 1, cy,     COL_TEXT_DARK);
    buf_draw_line(s_bar_buf, w, h, next_cx - 3, cy + 3, next_cx + 1, cy,     COL_TEXT_DARK);
    buf_fill_rect(s_bar_buf, w, h, next_cx + 3, cy - 3, 1, 7, COL_TEXT_DARK);

    /* 5. Repeat Icon */
    draw_repeat_icon(s_bar_buf, w, h, rept_cx, cy, COL_TEXT_DARK);

    lcd_draw_bitmap_dma_sync(0, s_ui.bottom_y, w, s_ui.bottom_y + h, s_bar_buf);
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

void ui_st7789_get_resolution(int16_t *out_w, int16_t *out_h)
{
    if (out_w != NULL) *out_w = s_ui.width;
    if (out_h != NULL) *out_h = s_ui.height;
}

uint16_t *ui_st7789_lock_video_buf(void)
{
    atomic_store(&g_radio_state.is_video_mode, true);
    s_last_video_frame_us = esp_timer_get_time();
    if (s_lcd_mutex != NULL && xSemaphoreTake(s_lcd_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        return s_bar_buf;
    }
    return NULL;
}

void ui_st7789_draw_video_strip(int16_t y_start, int16_t strip_h)
{
    lcd_draw_bitmap_dma_sync(0, y_start, s_ui.width, y_start + strip_h, s_bar_buf);
}

void ui_st7789_wait_video_dma(bool *io_dma_inflight)
{
    if (io_dma_inflight != NULL && *io_dma_inflight) {
        (void)xSemaphoreTake(s_dma_sem, pdMS_TO_TICKS(100));
        *io_dma_inflight = false;
    }
}

void ui_st7789_draw_video_strip_async(int16_t y_start, int16_t strip_h, const void *buf, bool *io_dma_inflight)
{
    ui_st7789_wait_video_dma(io_dma_inflight);
    if (esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y_start, s_ui.width, y_start + strip_h, buf) == ESP_OK) {
        if (io_dma_inflight != NULL) {
            *io_dma_inflight = true;
        }
    }
}

void ui_st7789_unlock_video_buf(void)
{
    if (s_lcd_mutex != NULL) {
        xSemaphoreGive(s_lcd_mutex);
    }
}

/* Triple-Buffer JPEG tinh (3 x 10 KB) giup Core 0 (WiFi/WS) va Core 1 (Decode/DMA) chay song song 100% */
#define VID_SLOT_COUNT     3
#define VID_SLOT_MAX_BYTES 10240

static uint8_t __attribute__((aligned(4))) s_vid_slots[VID_SLOT_COUNT][VID_SLOT_MAX_BYTES];
static size_t       s_vid_len[VID_SLOT_COUNT];
static int8_t       s_slot_decoding = -1;
static int8_t       s_slot_latest   = -1;
static int8_t       s_slot_writing  = -1;
static portMUX_TYPE s_vid_spinlock  = portMUX_INITIALIZER_UNLOCKED;

void ui_st7789_stop_video_mode(void)
{
    portENTER_CRITICAL(&s_vid_spinlock);
    s_slot_latest = -1;
    portEXIT_CRITICAL(&s_vid_spinlock);
    atomic_store(&g_radio_state.is_video_mode, false);
    ui_st7789_notify(UI_NOTIFY_VIDEO_EXIT);
}

/* =========================================================================
 * Giai ma JPEG bang ESP32 ROM (tjpgd.h) + Ping-Pong Async SPI DMA (Zero malloc)
 *   - s_bar_buf (19.2 KB) chia 2 nua: CPU giai ma strip N+1 vao nua B
 *     trong khi SPI DMA dang day strip N tu nua A ra man hinh (song song 100%)
 *   - strip_h = boi so cua 16 (khop MCU 16x16 cua JPEG 4:2:0)
 * ========================================================================= */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    int16_t        strip_y0;
    int16_t        strip_h;
    int16_t        max_x;
    int16_t        max_y;
    uint16_t      *bufs[2];
    uint8_t        cur;
    bool           dma_inflight;
} jpeg_stream_ctx_t;

static uint8_t __attribute__((aligned(4))) s_tjpgd_pool[3584];
static volatile uint32_t s_last_decode_us = 0;

void ui_st7789_enter_video_mode(void)
{
    portENTER_CRITICAL(&s_vid_spinlock);
    s_last_video_frame_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_vid_spinlock);
    atomic_store(&g_radio_state.is_video_mode, true);
    ui_st7789_notify(UI_NOTIFY_VIDEO_FRAME);
}

uint32_t ui_st7789_last_decode_ms(void)
{
    return (uint32_t)(s_last_decode_us / 1000U);
}

uint8_t *ui_st7789_video_acquire_slot(int8_t *out_slot, size_t max_len)
{
    if (out_slot == NULL || max_len > VID_SLOT_MAX_BYTES) {
        return NULL;
    }
    int8_t chosen = -1;
    portENTER_CRITICAL(&s_vid_spinlock);
    for (int8_t i = 0; i < VID_SLOT_COUNT; i++) {
        if (i != s_slot_decoding && i != s_slot_latest) {
            chosen = i;
            break;
        }
    }
    if (chosen >= 0) {
        s_slot_writing = chosen;
    }
    portEXIT_CRITICAL(&s_vid_spinlock);
    if (chosen < 0) {
        return NULL;
    }
    *out_slot = chosen;
    return s_vid_slots[chosen];
}

void ui_st7789_video_submit_slot(int8_t slot_idx, size_t len)
{
    if (slot_idx < 0 || slot_idx >= VID_SLOT_COUNT) {
        return;
    }
    if (len == 0 || len > VID_SLOT_MAX_BYTES) {
        portENTER_CRITICAL(&s_vid_spinlock);
        if (s_slot_writing == slot_idx) {
            s_slot_writing = -1;
        }
        portEXIT_CRITICAL(&s_vid_spinlock);
        return;
    }
    portENTER_CRITICAL(&s_vid_spinlock);
    s_vid_len[slot_idx] = len;
    s_slot_latest = slot_idx;
    if (s_slot_writing == slot_idx) {
        s_slot_writing = -1;
    }
    s_last_video_frame_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_vid_spinlock);

    atomic_store(&g_radio_state.is_video_mode, true);
    ui_st7789_notify(UI_NOTIFY_VIDEO_FRAME);
}

static UINT tjpgd_in_func(JDEC *jd, BYTE *buff, UINT ndata)
{
    jpeg_stream_ctx_t *ctx = (jpeg_stream_ctx_t *)jd->device;
    size_t rem = ctx->len - ctx->pos;
    UINT to_read = (ndata < rem) ? ndata : (UINT)rem;
    if (buff != NULL && to_read > 0) {
        memcpy(buff, ctx->data + ctx->pos, to_read);
    }
    ctx->pos += to_read;
    return to_read;
}

/* Day strip hien tai (nua cur) qua DMA bat dong bo roi doi sang nua con lai */
static void jpeg_flush_strip(jpeg_stream_ctx_t *ctx)
{
    int16_t flush_h = ctx->strip_h;
    if (ctx->strip_y0 + flush_h > ctx->max_y) {
        flush_h = ctx->max_y - ctx->strip_y0;
    }
    if (flush_h > 0) {
        /* Ham nay cho DMA cua nua kia xong (neu con dang chay) roi moi phat lenh moi */
        ui_st7789_draw_video_strip_async(ctx->strip_y0, flush_h, ctx->bufs[ctx->cur], &ctx->dma_inflight);
        ctx->cur ^= 1U;
    }
    ctx->strip_y0 += ctx->strip_h;
}

static UINT tjpgd_out_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_stream_ctx_t *ctx = (jpeg_stream_ctx_t *)jd->device;
    const int16_t top = (int16_t)rect->top;

    if (top >= ctx->max_y) {
        return 1;
    }
    while (top >= ctx->strip_y0 + ctx->strip_h) {
        jpeg_flush_strip(ctx);
    }

    const uint8_t *src = (const uint8_t *)bitmap;
    const int16_t left = (int16_t)rect->left;
    const int16_t bw   = (int16_t)(rect->right - rect->left + 1);
    const int16_t bh   = (int16_t)(rect->bottom - rect->top + 1);
    const int16_t w    = ctx->max_x;
    int16_t cols = bw;
    if (left + cols > w) {
        cols = w - left;
    }
    if (cols <= 0) {
        return 1;
    }

    uint16_t *strip_buf = ctx->bufs[ctx->cur];
    for (int16_t row = 0; row < bh; row++) {
        int16_t gy = top + row;
        if (gy >= ctx->max_y) break;
        int16_t ly = gy - ctx->strip_y0;
        if (ly < 0 || ly >= ctx->strip_h) continue;

        uint16_t *dst = &strip_buf[ly * w + left];
        const uint8_t *s = src + (row * bw * 3);
        for (int16_t col = 0; col < cols; col++, s += 3) {
            uint16_t raw = (uint16_t)(((uint16_t)(s[0] & 0xF8U) << 8) |
                                      ((uint16_t)(s[1] & 0xFCU) << 3) |
                                      ((uint16_t)s[2] >> 3));
            dst[col] = (uint16_t)((raw >> 8) | (raw << 8));
        }
    }
    return 1;
}

bool ui_st7789_draw_jpeg_frame(const uint8_t *jpg_data, size_t jpg_len)
{
    if (jpg_data == NULL || jpg_len < 16) {
        return false;
    }
    atomic_store(&g_radio_state.is_video_mode, true);
    s_last_video_frame_us = esp_timer_get_time();

    if (s_lcd_mutex == NULL || xSemaphoreTake(s_lcd_mutex, pdMS_TO_TICKS(150)) != pdTRUE) {
        return false;
    }

    int64_t t_start = esp_timer_get_time();

    /* Moi nua buffer = 4,800 px. strip_h = boi so 16: 128px -> 32 dong, 160/240px -> 16 dong */
    const int32_t half_px = (int32_t)(sizeof(s_bar_buf) / sizeof(s_bar_buf[0])) / 2;
    int16_t strip_h = (int16_t)((half_px / s_ui.width) & ~15);
    if (strip_h < 16) strip_h = 16;

    jpeg_stream_ctx_t ctx = {
        .data         = jpg_data,
        .len          = jpg_len,
        .pos          = 0,
        .strip_y0     = 0,
        .strip_h      = strip_h,
        .max_x        = s_ui.width,
        .max_y        = s_ui.height,
        .bufs         = { s_bar_buf, s_bar_buf + half_px },
        .cur          = 0,
        .dma_inflight = false
    };

    JDEC jdec;
    JRESULT res = jd_prepare(&jdec, tjpgd_in_func, s_tjpgd_pool, sizeof(s_tjpgd_pool), &ctx);
    if (res == JDR_OK) {
        if ((int16_t)jdec.height < ctx.max_y) {
            ctx.max_y = (int16_t)jdec.height;
        }
        res = jd_decomp(&jdec, tjpgd_out_func, 0);
        if (res == JDR_OK && ctx.strip_y0 < ctx.max_y) {
            jpeg_flush_strip(&ctx);
        }
    }

    /* Bat buoc cho DMA strip cuoi xong truoc khi tra s_bar_buf cho task khac */
    ui_st7789_wait_video_dma(&ctx.dma_inflight);
    s_last_decode_us = (uint32_t)(esp_timer_get_time() - t_start);

    xSemaphoreGive(s_lcd_mutex);
    return (res == JDR_OK);
}

/* =========================================================================
 * CORE 1 (Priority 4): ui_render_task
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

    bool playing = atomic_load(&g_radio_state.is_playing);
    bool wifi_ok = atomic_load(&g_radio_state.wifi_connected);

    if (xSemaphoreTake(s_lcd_mutex, portMAX_DELAY) == pdTRUE) {
        render_top_ip_dirty_rect(wifi_ok, local_ip);
        render_vinyl_dirty_rect(g_radio_state.disc_angle, playing);
        render_title_dirty_rect(local_station, local_title, 0, &song_px_width);
        render_slider_dirty_rect(elapsed_sec, g_radio_state.volume, false);
        render_bottom_dirty_rect(playing);
        xSemaphoreGive(s_lcd_mutex);
    }

    int16_t max_vis_w = s_ui.width - ((s_ui.mode == LCD_MODE_ST7735_128X160) ? 12 : 24);
    bool    was_in_video = false;

    for (;;) {
        playing = atomic_load(&g_radio_state.is_playing);
        bool in_video = atomic_load(&g_radio_state.is_video_mode);
        bool need_marquee = (song_px_width > max_vis_w);

        if (in_video != was_in_video) {
            vTaskPrioritySet(NULL, in_video ? 12 : TASK_PRIO_UI_RENDER);
            was_in_video = in_video;
        }

        TickType_t wait_ticks = in_video ? pdMS_TO_TICKS(250)
                                         : (playing ? pdMS_TO_TICKS(33)
                                                    : (need_marquee ? pdMS_TO_TICKS(40) : pdMS_TO_TICKS(500)));

        uint32_t notify_bits = 0;
        (void)xTaskNotifyWait(0UL, ULONG_MAX, &notify_bits, wait_ticks);

        playing  = atomic_load(&g_radio_state.is_playing);
        wifi_ok  = atomic_load(&g_radio_state.wifi_connected);
        in_video = atomic_load(&g_radio_state.is_video_mode);
        int64_t now_us = esp_timer_get_time();

        /* Neu dang o Video Mode: giai ma + Ping-Pong DMA tren Core 1 song song voi WiFi tren Core 0 */
        if (in_video) {
            if (!was_in_video) {
                vTaskPrioritySet(NULL, 12);
                was_in_video = true;
            }
            for (;;) {
                int8_t idx = -1;
                size_t len = 0;
                portENTER_CRITICAL(&s_vid_spinlock);
                if (s_slot_latest >= 0) {
                    idx = s_slot_latest;
                    s_slot_latest = -1;
                    s_slot_decoding = idx;
                    len = s_vid_len[idx];
                }
                portEXIT_CRITICAL(&s_vid_spinlock);

                if (idx < 0) {
                    break;
                }
                (void)ui_st7789_draw_jpeg_frame(s_vid_slots[idx], len);
                portENTER_CRITICAL(&s_vid_spinlock);
                s_slot_decoding = -1;
                portEXIT_CRITICAL(&s_vid_spinlock);
                now_us = esp_timer_get_time();
            }

            if ((now_us - s_last_video_frame_us) > 3500000LL) {
                atomic_store(&g_radio_state.is_video_mode, false);
                vTaskPrioritySet(NULL, TASK_PRIO_UI_RENDER);
                was_in_video = false;
                notify_bits |= UI_NOTIFY_VIDEO_EXIT;
            } else {
                continue;
            }
        } else if (was_in_video) {
            vTaskPrioritySet(NULL, TASK_PRIO_UI_RENDER);
            was_in_video = false;
        }

        if (xSemaphoreTake(s_lcd_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if ((notify_bits & UI_NOTIFY_VIDEO_EXIT) != 0) {
            radio_state_get_strings(local_station, sizeof(local_station),
                                    local_title, sizeof(local_title));
            radio_state_get_ip(local_ip, sizeof(local_ip));
            render_top_ip_dirty_rect(wifi_ok, local_ip);
            render_vinyl_dirty_rect(g_radio_state.disc_angle, playing);
            render_title_dirty_rect(local_station, local_title, marquee_offset_px, &song_px_width);
            render_slider_dirty_rect(elapsed_sec, g_radio_state.volume, false);
            render_bottom_dirty_rect(playing);
            xSemaphoreGive(s_lcd_mutex);
            continue;
        }

        if ((notify_bits & UI_NOTIFY_WIFI) != 0) {
            radio_state_get_ip(local_ip, sizeof(local_ip));
            render_top_ip_dirty_rect(wifi_ok, local_ip);
        }

        if ((notify_bits & (UI_NOTIFY_STATION | UI_NOTIFY_METADATA)) != 0) {
            radio_state_get_strings(local_station, sizeof(local_station),
                                    local_title, sizeof(local_title));
            marquee_offset_px = 0;
            marquee_in_pause = true;
            marquee_pause_since_us = now_us;
            render_title_dirty_rect(local_station, local_title, 0, &song_px_width);
        }

        if ((notify_bits & UI_NOTIFY_PLAY_STATE) != 0) {
            render_bottom_dirty_rect(playing);
        }

        if ((notify_bits & UI_NOTIFY_VOLUME) != 0) {
            vol_overlay_until_us = now_us + 3000000LL;
            render_slider_dirty_rect(elapsed_sec, g_radio_state.volume, true);
        }

        if (playing && (now_us - last_sec_us) >= 1000000LL) {
            last_sec_us = now_us;
            elapsed_sec++;
            bool show_vol = (now_us < vol_overlay_until_us);
            render_slider_dirty_rect(elapsed_sec, g_radio_state.volume, show_vol);
        }

        if (playing) {
            g_radio_state.disc_angle = (uint16_t)((g_radio_state.disc_angle + 6U) % 360U);
            render_vinyl_dirty_rect(g_radio_state.disc_angle, true);
        } else if ((notify_bits & UI_NOTIFY_PLAY_STATE) != 0) {
            render_vinyl_dirty_rect(g_radio_state.disc_angle, false);
        }

        if (song_px_width > max_vis_w) {
            if (marquee_in_pause) {
                if ((now_us - marquee_pause_since_us) >= 2000000LL) {
                    marquee_in_pause = false;
                }
            } else {
                marquee_offset_px += 1;
                if (marquee_offset_px >= (song_px_width + 36)) {
                    marquee_offset_px = 0;
                    marquee_in_pause = true;
                    marquee_pause_since_us = now_us;
                }
                render_title_dirty_rect(local_station, local_title, marquee_offset_px, &song_px_width);
            }
        }

        xSemaphoreGive(s_lcd_mutex);
    }
}

/* =========================================================================
 * Khoi tao Panel ST7735 (128x160 @ 16MHz, CS=15, Offset 0,0)
 * hoac ST7789 (240x240 @ 27MHz, CS=5/None, IPS INVON) theo g_radio_state.lcd_mode
 * ========================================================================= */
esp_err_t ui_st7789_init(void)
{
    uint8_t mode = g_radio_state.lcd_mode;
    configure_ui_layout(mode);

    s_dma_sem   = xSemaphoreCreateBinaryStatic(&s_dma_sem_storage);
    s_lcd_mutex = xSemaphoreCreateMutexStatic(&s_lcd_mutex_storage);
    configASSERT(s_dma_sem != NULL && s_lcd_mutex != NULL);

    /* 1. Cau hinh chan Backlight (GPIO 32), Reset (GPIO 4) va chan CS phu */
    gpio_num_t spi_cs_pin   = (mode == LCD_MODE_ST7735_128X160) ? PIN_NUM_CS    : PIN_TFT_CS_HW;
    gpio_num_t extra_cs_pin = (mode == LCD_MODE_ST7735_128X160) ? PIN_TFT_CS_HW : PIN_NUM_CS;
    uint32_t   spi_clk_hz   = (mode == LCD_MODE_ST7735_128X160) ? LCD_PIXEL_CLOCK_HZ : TFT_SPI_CLOCK_HZ;

    const gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << PIN_NUM_BK_LIGHT) |
                        (1ULL << PIN_NUM_RST) |
                        (1ULL << extra_cs_pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(PIN_NUM_BK_LIGHT, 0);
    gpio_set_level(PIN_NUM_RST, 1);
    gpio_set_level(extra_cs_pin, 0);

    /* 2. Khoi tao bus SPI2 (LCD_HOST: CLK=18, MOSI=23) */
    const spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_NUM_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_NUM_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = sizeof(s_bar_buf),
    };
    err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        printf("[UI_LCD] ERROR: spi_bus_initialize failed: %s\n", esp_err_to_name(err));
        return err;
    }

    /* 3. Gan Panel IO SPI (ST7735: CS=15 @ 20MHz | ST7789: CS=5 @ 27MHz) */
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num         = PIN_NUM_DC,
        .cs_gpio_num         = spi_cs_pin,
        .pclk_hz             = spi_clk_hz,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .spi_mode            = 0,
        .trans_queue_depth   = 4,
        .on_color_trans_done = on_lcd_color_trans_done,
        .user_ctx            = NULL,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_io_handle);
    if (err != ESP_OK) {
        printf("[UI_LCD] ERROR: esp_lcd_new_panel_io_spi failed: %s\n", esp_err_to_name(err));
        return err;
    }

    /* 4. Tao esp_lcd panel handle (MIPI-DCS CASET 0x2A / RASET 0x2B / RAMWR 0x2C) */
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle);
    if (err != ESP_OK) {
        printf("[UI_LCD] ERROR: esp_lcd_new_panel failed: %s\n", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    /* Hardware Reset chung */
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_lcd_panel_io_tx_param(s_io_handle, 0x01, NULL, 0); /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));

    esp_lcd_panel_io_tx_param(s_io_handle, 0x11, NULL, 0); /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(150));

    if (mode == LCD_MODE_ST7735_128X160) {
        /* ================= KHOI TAO CHONG NHIEU HAT CHO MAN HINH ST7735 (128x160) =================
         * - INVCTR (0xB4 = 0x07): Line Inversion thay vi Dot Inversion (khu nhieu hat caro)
         * - PWCTR / VMCTR / Gamma: On dinh dien ap VCOM & tang do tuong phan sac net
         */
        const uint8_t frmctr1[] = { 0x01, 0x2C, 0x2D };
        const uint8_t frmctr2[] = { 0x01, 0x2C, 0x2D };
        const uint8_t frmctr3[] = { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D };
        const uint8_t invctr[]  = { 0x07 };
        const uint8_t pwctr1[]  = { 0xA2, 0x02, 0x84 };
        const uint8_t pwctr2[]  = { 0xC5 };
        const uint8_t pwctr3[]  = { 0x0A, 0x00 };
        const uint8_t pwctr4[]  = { 0x8A, 0x2A };
        const uint8_t pwctr5[]  = { 0x8A, 0xEE };
        const uint8_t vmctr1[]  = { 0x0E };
        const uint8_t gmctrp1[] = { 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                                    0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10 };
        const uint8_t gmctrn1[] = { 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                                    0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10 };

        esp_lcd_panel_io_tx_param(s_io_handle, 0xB1, frmctr1, sizeof(frmctr1));
        esp_lcd_panel_io_tx_param(s_io_handle, 0xB2, frmctr2, sizeof(frmctr2));
        esp_lcd_panel_io_tx_param(s_io_handle, 0xB3, frmctr3, sizeof(frmctr3));
        esp_lcd_panel_io_tx_param(s_io_handle, 0xB4, invctr,  sizeof(invctr));
        esp_lcd_panel_io_tx_param(s_io_handle, 0xC0, pwctr1,  sizeof(pwctr1));
        esp_lcd_panel_io_tx_param(s_io_handle, 0xC1, pwctr2,  sizeof(pwctr2));
        esp_lcd_panel_io_tx_param(s_io_handle, 0xC2, pwctr3,  sizeof(pwctr3));
        esp_lcd_panel_io_tx_param(s_io_handle, 0xC3, pwctr4,  sizeof(pwctr4));
        esp_lcd_panel_io_tx_param(s_io_handle, 0xC4, pwctr5,  sizeof(pwctr5));
        esp_lcd_panel_io_tx_param(s_io_handle, 0xC5, vmctr1,  sizeof(vmctr1));

        esp_lcd_panel_io_tx_param(s_io_handle, 0x20, NULL, 0); /* INVOFF (ST7735 Normal Color) */

        const uint8_t madctl_st7735 = (LCD_H_RES <= LCD_V_RES) ? ST7735_MADCTL_PORTRAIT
                                                               : ST7735_MADCTL_LANDSCAPE;
        esp_lcd_panel_io_tx_param(s_io_handle, 0x36, &madctl_st7735, 1);

        const uint8_t colmod_st7735 = 0x05; /* 16-bit RGB565 */
        esp_lcd_panel_io_tx_param(s_io_handle, 0x3A, &colmod_st7735, 1);

        esp_lcd_panel_io_tx_param(s_io_handle, 0xE0, gmctrp1, sizeof(gmctrp1));
        esp_lcd_panel_io_tx_param(s_io_handle, 0xE1, gmctrn1, sizeof(gmctrn1));

        esp_lcd_panel_io_tx_param(s_io_handle, 0x13, NULL, 0); /* NORON */
        vTaskDelay(pdMS_TO_TICKS(10));

        esp_lcd_panel_io_tx_param(s_io_handle, 0x29, NULL, 0); /* DISPON */
        vTaskDelay(pdMS_TO_TICKS(50));

        /* Ap dung Offset bu vien (mac dinh 0, 0 de CASET 0..127 khong tran cot 127 gay xeo hinh) */
        int gap_x = (LCD_H_RES <= LCD_V_RES) ? LCD_OFFSET_X : LCD_OFFSET_Y;
        int gap_y = (LCD_H_RES <= LCD_V_RES) ? LCD_OFFSET_Y : LCD_OFFSET_X;
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel_handle, gap_x, gap_y));
    } else {
        /* ================= KHOI TAO RIENG CHO MAN HINH ST7789 (240x240) ================= */
        const uint8_t madctl_st7789 = 0x70;
        esp_lcd_panel_io_tx_param(s_io_handle, 0x36, &madctl_st7789, 1);

        const uint8_t colmod_st7789 = 0x55;
        esp_lcd_panel_io_tx_param(s_io_handle, 0x3A, &colmod_st7789, 1);

        /* IPS ST7789 can INVON (0x21) de 0x0000 hien thi DEN TUYEN */
        esp_lcd_panel_io_tx_param(s_io_handle, 0x21, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(10));

        esp_lcd_panel_io_tx_param(s_io_handle, 0x29, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(50));

        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel_handle, 0, 0));
    }

    /* To toan bo nen Kem Giay Am (#F4F1EA) va bat den nen BLK=32 */
    int16_t step_h = 20;
    for (int16_t y = 0; y < s_ui.height; y += step_h) {
        int16_t cur_h = (y + step_h <= s_ui.height) ? step_h : (s_ui.height - y);
        buf_fill_rect(s_bar_buf, s_ui.width, cur_h, 0, 0, s_ui.width, cur_h, COL_BG_CREAM);
        lcd_draw_bitmap_dma_sync(0, y, s_ui.width, y + cur_h, s_bar_buf);
    }
    gpio_set_level(PIN_NUM_BK_LIGHT, 1);

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

    printf("[UI_LCD] Mode %u (%s %dx%d @ %luMHz, CS=GPIO%d) initialized via esp_lcd DMA\n",
           (unsigned)mode,
           (mode == LCD_MODE_ST7735_128X160) ? "ST7735" : "ST7789",
           (int)s_ui.width, (int)s_ui.height,
           (unsigned long)(spi_clk_hz / 1000000UL),
           (int)spi_cs_pin);
    return ESP_OK;
}
