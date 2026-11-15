/*
 * ui_st7789.c
 * Giao dien Trinh phat nhac Co Dien (Pitch-Black Vinyl & Warm Vintage Hi-Fi)
 * tren man hinh ST7789 240x240 su dung driver goc esp_lcd (SPI 40MHz):
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

static const char *TAG = "UI_ST7789";

/* =========================================================================
 * Macro chuyen doi RGB888 sang RGB565 Big-Endian (chuẩn SPI ST7789 DMA)
 * Tinh toan hoan toan tai compile-time (Zero runtime byte-swap overhead)
 * ========================================================================= */
#define RAW_RGB565(r, g, b) \
    ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | (((uint16_t)(g) & 0xFCU) << 3) | (((uint16_t)(b) & 0xF8U) >> 3)))
#define BE_RGB565(r, g, b) \
    ((uint16_t)((RAW_RGB565(r, g, b) >> 8) | (RAW_RGB565(r, g, b) << 8)))

/* Bang mau Co Dien (Warm Vintage Hi-Fi & Pitch-Black Vinyl #000000) */
#define COL_BG_DARK          BE_RGB565(16,  13,  10)   /* #100D0A Go mun den am */
#define COL_TOPBAR_BG        BE_RGB565(26,  20,  15)   /* #1A140F Thanh tren nau co dien */
#define COL_CARD_BG          BE_RGB565(24,  19,  14)   /* #18130E Khung chu bai hat */
#define COL_BRASS_BORDER     BE_RGB565(115, 88,  48)   /* #735830 Vien dong co */
#define COL_BRASS_BRIGHT     BE_RGB565(205, 164, 92)   /* #CDA45C Vang dong sang */

#define COL_PLATTER_OUTER    BE_RGB565(180, 145, 85)   /* #B49155 Vien mam xoay dong thau */
#define COL_PLATTER_DARK     BE_RGB565(58,  48,  36)   /* #3A3024 Ranh mam xoay */
#define COL_STROBE_DOT       BE_RGB565(235, 205, 140)  /* #EBCD8C Cham Strobe co dien */

#define COL_VINYL_BLACK      0x0000U                   /* #000000 MAT DIA DEN TUYEN */
#define COL_VINYL_GROOVE     BE_RGB565(22,  20,  18)   /* #161412 Ranh dong tam tram */
#define COL_VINYL_TRACKGAP   BE_RGB565(34,  30,  25)   /* #221E19 Ranh chia bai */
#define COL_SHEEN_SOFT       BE_RGB565(20,  17,  13)   /* #14110D Anh den day toc nhe */
#define COL_SHEEN_CORE       BE_RGB565(36,  30,  22)   /* #241E16 Tia sang chinh */
#define COL_GROOVE_GLINT1    BE_RGB565(150, 128, 90)   /* #96805A Ranh bat sang vang dong */
#define COL_GROOVE_GLINT2    BE_RGB565(225, 198, 145)  /* #E1C691 Ranh bat sang kem vang */

#define COL_LABEL_RED        BE_RGB565(175, 48,  28)   /* #AF301C Do gach Crimson 1960s */
#define COL_LABEL_CREAM      BE_RGB565(232, 214, 175)  /* #E8D6AF Kem giay cu */
#define COL_LABEL_GOLD       BE_RGB565(218, 172, 84)   /* #DAAC54 Vong chi vang */
#define COL_SPINDLE_HOLE     BE_RGB565(242, 234, 216)  /* #F2EAD8 Lo truc giua */

#define COL_AMBER_GLOW       BE_RGB565(255, 178, 64)   /* #FFB240 Vang ho phach */
#define COL_CREAM_TEXT       BE_RGB565(250, 238, 212)  /* #FAEED4 Chu trang kem */
#define COL_MUTED_TEXT       BE_RGB565(138, 120, 96)   /* #8A7860 Chu xam nau */
#define COL_VINTAGE_GREEN    BE_RGB565(104, 204, 112)  /* #68CC70 Xanh den bao Play */
#define COL_BTN_SURFACE      BE_RGB565(34,  27,  20)   /* #221B14 Nen nut bam co dien */

/* =========================================================================
 * Kich thuoc cac vung Dirty-Rectangle tren man hinh 240x240
 * ========================================================================= */
#define TOP_BAR_Y            0
#define TOP_BAR_H            25

#define VINYL_W              136
#define VINYL_H              136
#define VINYL_STRIP_H        34      /* 136 / 4 = 34 dong moi strip DMA */
#define VINYL_SCREEN_X       52      /* (240 - 136) / 2 */
#define VINYL_SCREEN_Y       28      /* Y: 28..163 */
#define VINYL_OUTER_R        61
#define VINYL_LABEL_R        21
#define VINYL_HOLE_R         4

#define TITLE_BAR_Y          169
#define TITLE_BAR_H          26

#define BOTTOM_BAR_Y         198
#define BOTTOM_BAR_H         42

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
 * Font ASCII 5x7 luu trong Flash
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
static DMA_ATTR uint16_t s_bar_buf[TFT_WIDTH * BOTTOM_BAR_H];         /* 240x42 = 20,160 B */
static DMA_ATTR uint16_t s_vinyl_strip_buf[VINYL_W * VINYL_STRIP_H];  /* 136x34 =  9,248 B */

static esp_lcd_panel_io_handle_t s_io_handle    = NULL;
static esp_lcd_panel_handle_t    s_panel_handle = NULL;

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

static void buf_fill_round_rect(uint16_t *buf, int16_t bw, int16_t bh,
                                int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col)
{
    if (w < 4 || h < 4) {
        buf_fill_rect(buf, bw, bh, x, y, w, h, col);
        return;
    }
    buf_fill_rect(buf, bw, bh, x + 2, y, w - 4, 1, col);
    buf_fill_rect(buf, bw, bh, x + 1, y + 1, w - 2, h - 2, col);
    buf_fill_rect(buf, bw, bh, x + 2, y + h - 1, w - 4, 1, col);
}

static void buf_draw_round_rect(uint16_t *buf, int16_t bw, int16_t bh,
                                int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col)
{
    buf_fill_rect(buf, bw, bh, x + 2, y, w - 4, 1, col);
    buf_fill_rect(buf, bw, bh, x + 2, y + h - 1, w - 4, 1, col);
    buf_fill_rect(buf, bw, bh, x, y + 2, 1, h - 4, col);
    buf_fill_rect(buf, bw, bh, x + w - 1, y + 2, 1, h - 4, col);
    buf_put_pixel(buf, bw, bh, x + 1, y + 1, col);
    buf_put_pixel(buf, bw, bh, x + w - 2, y + 1, col);
    buf_put_pixel(buf, bw, bh, x + 1, y + h - 2, col);
    buf_put_pixel(buf, bw, bh, x + w - 2, y + h - 2, col);
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
 * 1. DIRTY-RECTANGLE: TOP BAR (Y: 0..24)
 * ========================================================================= */
static void render_top_bar_dirty_rect(const char *station_name, bool is_playing,
                                      bool wifi_ok, int8_t rssi)
{
    buf_fill_rect(s_bar_buf, TFT_WIDTH, TOP_BAR_H, 0, 0, TFT_WIDTH, TOP_BAR_H, COL_TOPBAR_BG);
    buf_fill_rect(s_bar_buf, TFT_WIDTH, TOP_BAR_H, 0, TOP_BAR_H - 1, TFT_WIDTH, 1, COL_BRASS_BORDER);

    /* Cot song WiFi 4 vach ben trai */
    int bars = 0;
    if (wifi_ok) {
        if (rssi >= -55)      bars = 4;
        else if (rssi >= -67) bars = 3;
        else if (rssi >= -78) bars = 2;
        else                  bars = 1;
    }
    for (int i = 0; i < 4; i++) {
        int16_t bh = (int16_t)(4 + i * 3);
        int16_t bx = (int16_t)(7 + i * 5);
        int16_t by = (int16_t)(19 - bh);
        uint16_t c = (i < bars) ? COL_AMBER_GLOW : COL_PLATTER_DARK;
        buf_fill_rect(s_bar_buf, TFT_WIDTH, TOP_BAR_H, bx, by, 3, bh, c);
    }

    /* Ten kenh Radio can giua mau Vang Ho Phach */
    int16_t tw = text_pixel_width(station_name, 1);
    int16_t tx = (TFT_WIDTH - tw) / 2;
    buf_draw_text(s_bar_buf, TFT_WIDTH, TOP_BAR_H, station_name, tx, 9, 1, COL_AMBER_GLOW);
    buf_draw_text(s_bar_buf, TFT_WIDTH, TOP_BAR_H, station_name, tx + 1, 9, 1, COL_AMBER_GLOW);

    /* Huy hieu trang thai Play/Pause goc phai */
    int16_t bx = TFT_WIDTH - 28;
    int16_t by = 5;
    buf_fill_round_rect(s_bar_buf, TFT_WIDTH, TOP_BAR_H, bx, by, 22, 15, COL_BTN_SURFACE);
    buf_draw_round_rect(s_bar_buf, TFT_WIDTH, TOP_BAR_H, bx, by, 22, 15, COL_BRASS_BORDER);

    if (is_playing) {
        /* Icon tam giac Play */
        for (int16_t dx = 0; dx < 7; dx++) {
            int16_t h = (int16_t)(7 - dx);
            buf_fill_rect(s_bar_buf, TFT_WIDTH, TOP_BAR_H,
                          bx + 8 + dx, by + 7 - (h / 2), 1, h, COL_VINTAGE_GREEN);
        }
    } else {
        /* Icon 2 vach Pause */
        buf_fill_rect(s_bar_buf, TFT_WIDTH, TOP_BAR_H, bx + 7,  by + 4, 3, 7, COL_AMBER_GLOW);
        buf_fill_rect(s_bar_buf, TFT_WIDTH, TOP_BAR_H, bx + 12, by + 4, 3, 7, COL_AMBER_GLOW);
    }

    /* Day Dirty-Rectangle Top Bar (240x25) qua SPI DMA */
    esp_lcd_panel_draw_bitmap(s_panel_handle, 0, TOP_BAR_Y, TFT_WIDTH, TOP_BAR_Y + TOP_BAR_H, s_bar_buf);
}

/* =========================================================================
 * 2. DIRTY-RECTANGLE: PITCH-BLACK VINYL RECORD (X: 52..187, Y: 28..163)
 * Su dung sin_lut[360] & cos_lut[360] Fixed-Point (>> 10) hoan toan khong float!
 * ========================================================================= */
static void render_vinyl_dirty_rect(uint16_t angle_deg, bool is_playing)
{
    uint16_t deg = angle_deg % 360U;
    int32_t cos_a = cos_lut[deg];
    int32_t sin_a = sin_lut[deg];

    const int16_t cx = VINYL_W / 2; /* 68 */
    const int16_t cy = VINYL_H / 2; /* 68 */

    const int32_t r_platter_outer_sq = (VINYL_OUTER_R + 2) * (VINYL_OUTER_R + 2); /* 63^2 */
    const int32_t r_platter_bevel_sq = VINYL_OUTER_R * VINYL_OUTER_R;             /* 61^2 */
    const int32_t r_vinyl_outer_sq   = (VINYL_OUTER_R - 2) * (VINYL_OUTER_R - 2); /* 59^2 */
    const int32_t r_label_outer_sq   = VINYL_LABEL_R * VINYL_LABEL_R;             /* 21^2 */
    const int32_t r_label_gold_sq    = (VINYL_LABEL_R - 2) * (VINYL_LABEL_R - 2); /* 19^2 */
    const int32_t r_label_inner_sq   = (VINYL_LABEL_R - 4) * (VINYL_LABEL_R - 4); /* 17^2 */
    const int32_t r_label_center_sq  = 10 * 10;                                   /* 10^2 */
    const int32_t r_collar_sq        = (VINYL_HOLE_R + 2) * (VINYL_HOLE_R + 2);   /*  6^2 */
    const int32_t r_hole_sq          = VINYL_HOLE_R * VINYL_HOLE_R;               /*  4^2 */

    /* Toa do vach kim chi huong tren nhan tam (Fixed-Point >> 10) */
    int16_t mx1 = cx + (int16_t)((cos_a * 6) >> 10);
    int16_t my1 = cy + (int16_t)((sin_a * 6) >> 10);
    int16_t mx2 = cx + (int16_t)((cos_a * (VINYL_LABEL_R - 3)) >> 10);
    int16_t my2 = cy + (int16_t)((sin_a * (VINYL_LABEL_R - 3)) >> 10);

    /* Toa do can doc dia than (Tonearm) */
    int16_t pivot_x = VINYL_W - 11;
    int16_t pivot_y = 13;
    int16_t elbow_x = is_playing ? (VINYL_W - 15) : (VINYL_W - 5);
    int16_t elbow_y = is_playing ? 46 : 44;
    int16_t head_x  = is_playing ? (cx + 36) : (VINYL_W - 7);
    int16_t head_y  = is_playing ? (cy + 20) : 68;

    /* Render 136x136 theo 4 strip 136x34 de tiet kiem SRAM tuyet doi */
    for (int16_t strip = 0; strip < 4; strip++) {
        int16_t y0 = strip * VINYL_STRIP_H;

        for (int16_t sy = 0; sy < VINYL_STRIP_H; sy++) {
            int16_t y   = y0 + sy;
            int32_t dy  = y - cy;
            int32_t dy2 = dy * dy;
            uint16_t *line = &s_vinyl_strip_buf[sy * VINYL_W];

            for (int16_t x = 0; x < VINYL_W; x++) {
                int32_t dx = x - cx;
                int32_t d2 = dx * dx + dy2;

                if (d2 > r_platter_outer_sq) {
                    line[x] = COL_BG_DARK;
                } else if (d2 > r_platter_bevel_sq) {
                    line[x] = COL_PLATTER_OUTER;
                } else if (d2 > r_vinyl_outer_sq) {
                    line[x] = COL_PLATTER_DARK;
                } else if (d2 <= r_hole_sq) {
                    line[x] = COL_SPINDLE_HOLE;
                } else if (d2 <= r_collar_sq) {
                    line[x] = COL_BRASS_BORDER;
                } else if (d2 <= r_label_outer_sq) {
                    int32_t ry_fp = (-dx * sin_a + dy * cos_a) >> 10;
                    if (d2 >= r_label_inner_sq && d2 <= r_label_gold_sq) {
                        line[x] = COL_LABEL_GOLD;
                    } else if (d2 <= r_label_center_sq) {
                        line[x] = (ry_fp < 0) ? COL_LABEL_CREAM : COL_LABEL_RED;
                    } else {
                        line[x] = COL_LABEL_RED;
                    }
                } else {
                    /* Mat dia than DEN TUYEN (#000000) + Ranh dong tam Fixed-Point */
                    int32_t rx_fp = (dx * cos_a + dy * sin_a) >> 10;
                    int32_t ry_fp = (-dx * sin_a + dy * cos_a) >> 10;
                    int32_t abs_rx = i32_abs(rx_fp);
                    int32_t abs_ry = i32_abs(ry_fp);

                    /* Quat phan sang hep (~14 do): abs_ry * 100 < abs_rx * 11 / 24 */
                    bool in_spec_core = (abs_ry * 100 < abs_rx * 11);
                    bool in_spec_soft = (abs_ry * 100 < abs_rx * 24);

                    bool is_track_gap = (d2 >= 40 * 40 && d2 <= 41 * 41);
                    bool is_groove    = (d2 >= 26 * 26 && d2 <= 27 * 27) ||
                                        (d2 >= 31 * 31 && d2 <= 32 * 32) ||
                                        (d2 >= 35 * 35 && d2 <= 36 * 36) ||
                                        (d2 >= 45 * 45 && d2 <= 46 * 46) ||
                                        (d2 >= 50 * 50 && d2 <= 51 * 51) ||
                                        (d2 >= 55 * 55 && d2 <= 56 * 56);

                    if (is_groove || is_track_gap) {
                        if (in_spec_core)      line[x] = COL_GROOVE_GLINT2;
                        else if (in_spec_soft) line[x] = COL_GROOVE_GLINT1;
                        else                   line[x] = is_track_gap ? COL_VINYL_TRACKGAP : COL_VINYL_GROOVE;
                    } else {
                        if (in_spec_core)      line[x] = COL_SHEEN_CORE;
                        else if (in_spec_soft) line[x] = COL_SHEEN_SOFT;
                        else                   line[x] = COL_VINYL_BLACK;
                    }
                }
            }
        }

        /* Ve 24 cham Strobe co dien quanh vien mam xoay bang bang sin_lut/cos_lut */
        for (uint16_t i = 0; i < 24; i++) {
            uint16_t a_deg = (deg + i * 15U) % 360U;
            int16_t sx = cx + (int16_t)((cos_lut[a_deg] * 61) >> 10);
            int16_t sy = cy + (int16_t)((sin_lut[a_deg] * 61) >> 10);
            buf_put_pixel(s_vinyl_strip_buf, VINYL_W, VINYL_STRIP_H, sx, sy - y0, COL_STROBE_DOT);
        }

        /* Ve vach chi huong tren nhan tam & can kim Tonearm */
        buf_draw_wide_line_strip(s_vinyl_strip_buf, VINYL_W, y0, VINYL_STRIP_H,
                                 mx1, my1, mx2, my2, 1, COL_LABEL_CREAM);
        buf_draw_wide_line_strip(s_vinyl_strip_buf, VINYL_W, y0, VINYL_STRIP_H,
                                 pivot_x, pivot_y, elbow_x, elbow_y, 1, COL_LABEL_CREAM);
        buf_draw_wide_line_strip(s_vinyl_strip_buf, VINYL_W, y0, VINYL_STRIP_H,
                                 elbow_x, elbow_y, head_x, head_y, 1, COL_LABEL_CREAM);
        buf_draw_wide_line_strip(s_vinyl_strip_buf, VINYL_W, y0, VINYL_STRIP_H,
                                 pivot_x, pivot_y, pivot_x, pivot_y, 5, COL_BRASS_BRIGHT);
        buf_draw_wide_line_strip(s_vinyl_strip_buf, VINYL_W, y0, VINYL_STRIP_H,
                                 head_x, head_y, head_x, head_y, 3,
                                 is_playing ? COL_AMBER_GLOW : COL_BRASS_BORDER);

        /* Day tung strip Dirty-Rectangle cua dia than qua esp_lcd_panel_draw_bitmap */
        esp_lcd_panel_draw_bitmap(
            s_panel_handle,
            VINYL_SCREEN_X,
            VINYL_SCREEN_Y + y0,
            VINYL_SCREEN_X + VINYL_W,
            VINYL_SCREEN_Y + y0 + VINYL_STRIP_H,
            s_vinyl_strip_buf
        );
    }
}

/* =========================================================================
 * 3. DIRTY-RECTANGLE: SONG TITLE MARQUEE (Y: 169..194)
 * Chi ve lai khi StreamTitle thay doi hoac dich chuyen 1px Marquee
 * ========================================================================= */
static void render_title_dirty_rect(const char *song_title, int16_t marquee_offset_px)
{
    buf_fill_rect(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, 0, 0, TFT_WIDTH, TITLE_BAR_H, COL_BG_DARK);
    buf_fill_round_rect(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, 4, 1, TFT_WIDTH - 8, TITLE_BAR_H - 2, COL_CARD_BG);
    buf_draw_round_rect(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, 4, 1, TFT_WIDTH - 8, TITLE_BAR_H - 2, COL_BRASS_BORDER);

    int16_t tw = text_pixel_width(song_title, 2);
    int16_t max_visible_w = TFT_WIDTH - 16; /* 224 px */
    int16_t text_y = (TITLE_BAR_H - 14) / 2;

    if (tw <= max_visible_w) {
        /* Ngan hon man hinh -> Can giua */
        int16_t start_x = (TFT_WIDTH - tw) / 2;
        buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, song_title, start_x, text_y, 2, COL_CREAM_TEXT);
    } else {
        /* Dai hon man hinh -> Chay chu ngang Marquee muot 1px */
        int16_t start_x = 10 - marquee_offset_px;
        buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, song_title, start_x, text_y, 2, COL_CREAM_TEXT);

        const int16_t gap_px = 48;
        int16_t second_x = start_x + tw + gap_px;
        if (second_x < TFT_WIDTH - 8) {
            buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, " * ", start_x + tw + 6, text_y, 2, COL_AMBER_GLOW);
            buf_draw_text(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, song_title, second_x, text_y, 2, COL_CREAM_TEXT);
        }

        /* Che 2 mep khung card de chu khong tran ra ngoai vien */
        buf_fill_rect(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, 0, 0, 5, TITLE_BAR_H, COL_BG_DARK);
        buf_fill_rect(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, TFT_WIDTH - 5, 0, 5, TITLE_BAR_H, COL_BG_DARK);
        buf_fill_rect(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, 4, 3, 1, TITLE_BAR_H - 6, COL_BRASS_BORDER);
        buf_fill_rect(s_bar_buf, TFT_WIDTH, TITLE_BAR_H, TFT_WIDTH - 5, 3, 1, TITLE_BAR_H - 6, COL_BRASS_BORDER);
    }

    esp_lcd_panel_draw_bitmap(s_panel_handle, 0, TITLE_BAR_Y, TFT_WIDTH, TITLE_BAR_Y + TITLE_BAR_H, s_bar_buf);
}

/* =========================================================================
 * 4. DIRTY-RECTANGLE: BOTTOM CONTROLS & VOLUME BAR (Y: 198..239)
 * Cap nhat tuc thi khi is_playing hoac volume thay doi tu GPIO 0 / Web UI
 * ========================================================================= */
static void render_bottom_dirty_rect(bool is_playing, bool is_buffering, uint8_t volume_pct)
{
    buf_fill_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 0, 0, TFT_WIDTH, BOTTOM_BAR_H, COL_BG_DARK);

    /* Nut Previous (|<<) */
    buf_fill_round_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 38, 1, 44, 20, COL_BTN_SURFACE);
    buf_draw_round_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 38, 1, 44, 20, COL_BRASS_BORDER);
    buf_draw_text(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, "|<<", 51, 8, 1, COL_CREAM_TEXT);

    /* Nut Trung tam Play / Pause */
    uint16_t play_bg = is_playing ? COL_AMBER_GLOW : COL_LABEL_RED;
    uint16_t play_fg = is_playing ? COL_BG_DARK    : COL_CREAM_TEXT;
    buf_fill_round_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 92, 0, 56, 22, play_bg);
    buf_draw_round_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 92, 0, 56, 22, COL_BRASS_BRIGHT);

    if (is_buffering && is_playing) {
        buf_draw_text(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, "BUF..", 106, 8, 1, play_fg);
    } else if (is_playing) {
        buf_fill_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 115, 6, 3, 10, play_fg);
        buf_fill_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 122, 6, 3, 10, play_fg);
    } else {
        for (int16_t dx = 0; dx < 9; dx++) {
            int16_t h = (int16_t)(11 - dx);
            buf_fill_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H,
                          116 + dx, 11 - (h / 2), 1, h, play_fg);
        }
    }

    /* Nut Next (>>|) */
    buf_fill_round_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 158, 1, 44, 20, COL_BTN_SURFACE);
    buf_draw_round_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, 158, 1, 44, 20, COL_BRASS_BORDER);
    buf_draw_text(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, ">>|", 171, 8, 1, COL_CREAM_TEXT);

    /* Thanh Am luong bo goc (Rounded Progress Bar) + % */
    int16_t vol_y = 27;
    buf_draw_text(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, "VOL", 6, vol_y + 2, 1, COL_AMBER_GLOW);

    int16_t bar_x = 30;
    int16_t bar_w = 156;
    int16_t bar_h = 10;
    buf_fill_round_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, bar_x, vol_y, bar_w, bar_h, COL_CARD_BG);
    buf_draw_round_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, bar_x, vol_y, bar_w, bar_h, COL_BRASS_BORDER);

    if (volume_pct > 100) volume_pct = 100;
    int16_t fill_w = (int16_t)(((int32_t)(bar_w - 4) * volume_pct) / 100);
    if (fill_w > 0) {
        uint16_t fill_col = (volume_pct > 85) ? COL_LABEL_RED : COL_AMBER_GLOW;
        buf_fill_rect(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, bar_x + 2, vol_y + 2, fill_w, bar_h - 4, fill_col);
    }

    char pct_str[8];
    snprintf(pct_str, sizeof(pct_str), "%3u%%", (unsigned)volume_pct);
    buf_draw_text(s_bar_buf, TFT_WIDTH, BOTTOM_BAR_H, pct_str, TFT_WIDTH - 30, vol_y + 2, 1, COL_CREAM_TEXT);

    esp_lcd_panel_draw_bitmap(s_panel_handle, 0, BOTTOM_BAR_Y, TFT_WIDTH, BOTTOM_BAR_Y + BOTTOM_BAR_H, s_bar_buf);
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

    radio_state_get_strings(local_station, sizeof(local_station),
                            local_title, sizeof(local_title));

    int16_t marquee_offset_px = 0;
    int64_t marquee_pause_since_us = esp_timer_get_time();
    bool    marquee_in_pause = true;

    /* Ve lan dau toan bo 4 Dirty-Rectangles */
    bool playing   = atomic_load(&g_radio_state.is_playing);
    bool buffering = atomic_load(&g_radio_state.is_buffering);
    bool wifi_ok   = atomic_load(&g_radio_state.wifi_connected);

    render_top_bar_dirty_rect(local_station, playing, wifi_ok, g_radio_state.wifi_rssi);
    render_vinyl_dirty_rect(g_radio_state.disc_angle, playing);
    render_title_dirty_rect(local_title, 0);
    render_bottom_dirty_rect(playing, buffering, g_radio_state.volume);

    for (;;) {
        playing = atomic_load(&g_radio_state.is_playing);
        int16_t title_w = text_pixel_width(local_title, 2);
        bool need_marquee = (title_w > (TFT_WIDTH - 16));

        /* Khi dang phat: ~30 FPS (33ms). Khi Pause: 500ms (hoac 40ms neu dang cuon chu) */
        TickType_t wait_ticks = playing ? pdMS_TO_TICKS(33)
                                        : (need_marquee ? pdMS_TO_TICKS(40) : pdMS_TO_TICKS(500));

        uint32_t notify_bits = 0;
        (void)xTaskNotifyWait(0UL, ULONG_MAX, &notify_bits, wait_ticks);

        playing   = atomic_load(&g_radio_state.is_playing);
        buffering = atomic_load(&g_radio_state.is_buffering);
        wifi_ok   = atomic_load(&g_radio_state.wifi_connected);

        /* 1. Neu thay doi Station hoac Metadata -> doc lai chuoi an toan */
        if ((notify_bits & (UI_NOTIFY_STATION | UI_NOTIFY_METADATA)) != 0) {
            radio_state_get_strings(local_station, sizeof(local_station),
                                    local_title, sizeof(local_title));
        }

        if ((notify_bits & UI_NOTIFY_METADATA) != 0) {
            marquee_offset_px = 0;
            marquee_in_pause = true;
            marquee_pause_since_us = esp_timer_get_time();
            render_title_dirty_rect(local_title, 0);
        }

        /* 2. Neu thay doi trang thai Play/Pause, Station hoac WiFi -> cap nhat Top Bar */
        if ((notify_bits & (UI_NOTIFY_PLAY_STATE | UI_NOTIFY_STATION | UI_NOTIFY_WIFI)) != 0) {
            render_top_bar_dirty_rect(local_station, playing, wifi_ok, g_radio_state.wifi_rssi);
        }

        /* 3. Neu thay doi Play/Pause, Buffering hoac Volume -> cap nhat Bottom Bar ngay */
        if ((notify_bits & (UI_NOTIFY_PLAY_STATE | UI_NOTIFY_BUFFERING | UI_NOTIFY_VOLUME)) != 0) {
            render_bottom_dirty_rect(playing, buffering, g_radio_state.volume);
        }

        /* 4. Vung Dia than: CHI cap nhat khi is_playing == true hoac vua nhan Pause */
        if (playing) {
            g_radio_state.disc_angle = (uint16_t)((g_radio_state.disc_angle + 6U) % 360U);
            render_vinyl_dirty_rect(g_radio_state.disc_angle, true);
        } else if ((notify_bits & UI_NOTIFY_PLAY_STATE) != 0) {
            /* Khi vua chuyen sang PAUSE: ve 1 lan de dung dia ngay tai goc hien tai & nhac can kim */
            render_vinyl_dirty_rect(g_radio_state.disc_angle, false);
        }

        /* 5. Vung Marquee Song Title: Dich chuyen 1 pixel muot ma voi khoang nghi 2s dau cau */
        title_w = text_pixel_width(local_title, 2);
        if (title_w > (TFT_WIDTH - 16)) {
            int64_t now_us = esp_timer_get_time();
            if (marquee_in_pause) {
                if ((now_us - marquee_pause_since_us) >= 2000000LL) {
                    marquee_in_pause = false;
                }
            } else {
                marquee_offset_px += 1; /* Dich chuyen 1 pixel moi buoc */
                if (marquee_offset_px >= (title_w + 48)) {
                    marquee_offset_px = 0;
                    marquee_in_pause = true;
                    marquee_pause_since_us = now_us;
                }
                render_title_dirty_rect(local_title, marquee_offset_px);
            }
        }
    }
}

/* =========================================================================
 * Khoi tao ST7789 240x240 qua driver goc esp_lcd (SPI 40MHz, CS-less)
 * ========================================================================= */
esp_err_t ui_st7789_init(void)
{
    /* 1. Cau hinh chan Backlight (GPIO 32) & chan CS phu tro (GPIO 5, 15 keo LOW) */
    const gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << PIN_TFT_BLK) |
                        (1ULL << PIN_TFT_CS_COMPAT_1) |
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
    gpio_set_level(PIN_TFT_CS_COMPAT_1, 0);
    gpio_set_level(PIN_TFT_CS_COMPAT_2, 0);

    /* 2. Khoi tao bus SPI2 (SCL=18, SDA=23, Max transfer = 240*42*2 bytes) */
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
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. Gan Panel IO SPI (CS=-1 khong CS, DC=2, Xung nhip 40MHz) */
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = PIN_TFT_DC,
        .cs_gpio_num       = PIN_TFT_CS_NONE,
        .pclk_hz           = TFT_SPI_CLOCK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. Khoi tao Panel ST7789 (RES=4, RGB565) */
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_TFT_RES,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    /* Cau hinh MADCTL=0x70, COLMOD=0x55, INVOFF=0x20, DISPON=0x29 tuong thich module ST7789 */
    const uint8_t madctl_val = 0x70;
    const uint8_t colmod_val = 0x55;
    esp_lcd_panel_io_tx_param(s_io_handle, 0x36, &madctl_val, 1);
    esp_lcd_panel_io_tx_param(s_io_handle, 0x3A, &colmod_val, 1);
    esp_lcd_panel_io_tx_param(s_io_handle, 0x20, NULL, 0);
    esp_lcd_panel_io_tx_param(s_io_handle, 0x29, NULL, 0);

    /* Xoa nen ban dau theo tung bang 240x40 va bat den nen BLK=32 */
    for (int y = 0; y < TFT_HEIGHT; y += 40) {
        buf_fill_rect(s_bar_buf, TFT_WIDTH, 40, 0, 0, TFT_WIDTH, 40, COL_BG_DARK);
        if (y + 40 > 196 && y <= 196) {
            buf_fill_rect(s_bar_buf, TFT_WIDTH, 40, 10, 196 - y, TFT_WIDTH - 20, 1, COL_BRASS_BORDER);
        }
        esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y, TFT_WIDTH, y + 40, s_bar_buf);
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

    ESP_LOGI(TAG, "ST7789 240x240 initialized via esp_lcd (40MHz SPI, Dirty-Rectangle DMA)");
    return ESP_OK;
}
