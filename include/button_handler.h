/*
 * button_handler.h
 * Xu ly ngat cung GPIO 0 (IRAM_ATTR, Debounce 200ms bang esp_timer_get_time)
 * va Task dieu khien Toggle Play / Pause tuc thi (Zero-Blocking).
 */
#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Khoi tao GPIO 0 (Pull-up noi, ngat canh xuong) va tao Task xu ly su kien nut nhan */
esp_err_t button_handler_init(void);

/* Ham cho phep Web UI hoac GPIO 0 chuyen doi trang thai Play/Pause tuc thi */
void button_set_playback_state(bool play);
void button_toggle_play_pause(void);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_HANDLER_H */
