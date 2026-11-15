/*
 * ui_st7789.h
 * Giao dien Trinh phat nhac ST7789 240x240 su dung driver goc esp_lcd (40MHz SPI),
 * Dirty-Rectangle Direct Render va Bang luong giac Fixed-Point (>> 10) trong Flash.
 */
#ifndef UI_ST7789_H
#define UI_ST7789_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Khoi tao bus SPI2 (40MHz), ST7789 esp_lcd panel va tao ui_render_task tren Core 1 */
esp_err_t ui_st7789_init(void);

/* Gui tin hieu TaskNotify danh thuc ui_render_task tuc thi (non-blocking) */
void ui_st7789_notify(uint32_t notify_bits);

#ifdef __cplusplus
}
#endif

#endif /* UI_ST7789_H */
