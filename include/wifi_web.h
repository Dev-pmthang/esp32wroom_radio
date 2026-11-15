/*
 * wifi_web.h
 * Quan ly ket noi WiFi STA/AP (esp_wifi_set_ps(WIFI_PS_NONE)) va
 * Web Server dieu khien Radio (esp_http_server) tren Core 0 (Priority 5).
 */
#ifndef WIFI_WEB_H
#define WIFI_WEB_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Khoi tao WiFi STA/AP, tat Power Save va khoi chay wifi_and_web_task tren Core 0 */
esp_err_t wifi_web_init(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_WEB_H */
