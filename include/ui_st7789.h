/*
 * ui_st7789.h
 * Giao dien Trinh phat nhac ST7789 240x240 su dung driver goc esp_lcd (40MHz SPI),
 * Dirty-Rectangle Direct Render va Bang luong giac Fixed-Point (>> 10) trong Flash.
 */
#ifndef UI_ST7789_H
#define UI_ST7789_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Khoi tao bus SPI2, ST7789/ST7735 esp_lcd panel va tao ui_render_task tren Core 1 */
esp_err_t ui_st7789_init(void);

/* Gui tin hieu TaskNotify danh thuc ui_render_task tuc thi (non-blocking) */
void ui_st7789_notify(uint32_t notify_bits);

/* API ho tro stream Video MP4 Ping-Pong Async DMA (Zero extra RAM, Zero TCP Window Stall) */
void      ui_st7789_get_resolution(int16_t *out_w, int16_t *out_h);
uint16_t *ui_st7789_lock_video_buf(void);
void      ui_st7789_draw_video_strip(int16_t y_start, int16_t strip_h);
void      ui_st7789_draw_video_strip_async(int16_t y_start, int16_t strip_h, const void *buf, bool *io_dma_inflight);
void      ui_st7789_wait_video_dma(bool *io_dma_inflight);
void      ui_st7789_unlock_video_buf(void);
void      ui_st7789_enter_video_mode(void);
void      ui_st7789_stop_video_mode(void);
uint32_t  ui_st7789_last_decode_ms(void);
bool      ui_st7789_draw_jpeg_frame(const uint8_t *jpg_data, size_t jpg_len);
uint8_t  *ui_st7789_video_acquire_slot(int8_t *out_slot, size_t max_len);
void      ui_st7789_video_submit_slot(int8_t slot_idx, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* UI_ST7789_H */
