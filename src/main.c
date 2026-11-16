/*
 * main.c
 * Entry point chuan ESP-IDF v5.3+ (app_main) cho thiet bi ESP32 WiFi Internet Radio.
 * Khoi tao Static System State, StreamBuffer (48 KB), EventGroup va cac Task FreeRTOS
 * phan chia tren Core 0 (Network & Control) va Core 1 (Realtime Audio & UI).
 */
#include "radio_config.h"
#include "button_handler.h"
#include "audio_stream.h"
#include "ui_st7789.h"
#include "wifi_web.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "RADIO_MAIN";

/* =========================================================================
 * Danh sach kenh Internet Radio mac dinh luu trong Flash (Const)
 * ========================================================================= */
static const radio_station_preset_t kRadioPresets[] = {
    { "Coastal Letters", "http://ice1.somafm.com/seventies-128-mp3" },
    { "Lofi Groove",     "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "Lush Chill",      "http://ice1.somafm.com/lush-128-mp3" },
    { "Retro Synth",     "http://ice1.somafm.com/defcon-128-mp3" }
};
#define NUM_RADIO_PRESETS (sizeof(kRadioPresets) / sizeof(kRadioPresets[0]))

/* =========================================================================
 * Cap phat Tinh (Static Allocation) cho Toan bo Trang thai & Dong bo FreeRTOS
 * ========================================================================= */
radio_system_state_t    g_radio_state;
EventGroupHandle_t      g_radio_event_group = NULL;
StreamBufferHandle_t    g_audio_stream_buf  = NULL;

TaskHandle_t            g_ui_task_handle    = NULL;
TaskHandle_t            g_http_task_handle  = NULL;
TaskHandle_t            g_audio_task_handle = NULL;
TaskHandle_t            g_btn_task_handle   = NULL;

static StaticEventGroup_t s_radio_event_group_storage;

/* Khai bao bien phu tro de Linker uu tien app_main() thuan ESP-IDF tren PlatformIO */
bool         loopTaskWDTEnabled = false;
TaskHandle_t loopTaskHandle     = NULL;

/* =========================================================================
 * Cac ham truy cap & cap nhat Struct Trang thai He thong an toan luong
 * ========================================================================= */
void radio_state_set_station(const char *name, const char *url, uint8_t index)
{
    if (xSemaphoreTake(g_radio_state.mutex, portMAX_DELAY) == pdTRUE) {
        if (name != NULL) {
            strncpy(g_radio_state.station_name, name, sizeof(g_radio_state.station_name) - 1);
            g_radio_state.station_name[sizeof(g_radio_state.station_name) - 1] = '\0';
        }
        if (url != NULL) {
            strncpy(g_radio_state.stream_url, url, sizeof(g_radio_state.stream_url) - 1);
            g_radio_state.stream_url[sizeof(g_radio_state.stream_url) - 1] = '\0';
        }
        g_radio_state.station_index = index;
        xSemaphoreGive(g_radio_state.mutex);
    }
    ui_st7789_notify(UI_NOTIFY_STATION);
}

void radio_state_set_song_title(const char *title)
{
    if (title == NULL) {
        return;
    }
    if (xSemaphoreTake(g_radio_state.mutex, portMAX_DELAY) == pdTRUE) {
        strncpy(g_radio_state.song_title, title, sizeof(g_radio_state.song_title) - 1);
        g_radio_state.song_title[sizeof(g_radio_state.song_title) - 1] = '\0';
        xSemaphoreGive(g_radio_state.mutex);
    }
    ui_st7789_notify(UI_NOTIFY_METADATA);
}

void radio_state_get_strings(char *out_station, size_t station_len,
                             char *out_title, size_t title_len)
{
    if (xSemaphoreTake(g_radio_state.mutex, portMAX_DELAY) == pdTRUE) {
        if (out_station != NULL && station_len > 0) {
            strncpy(out_station, g_radio_state.station_name, station_len - 1);
            out_station[station_len - 1] = '\0';
        }
        if (out_title != NULL && title_len > 0) {
            strncpy(out_title, g_radio_state.song_title, title_len - 1);
            out_title[title_len - 1] = '\0';
        }
        xSemaphoreGive(g_radio_state.mutex);
    }
}

void radio_state_set_ip(const char *ip_str)
{
    if (xSemaphoreTake(g_radio_state.mutex, portMAX_DELAY) == pdTRUE) {
        if (ip_str != NULL) {
            strncpy(g_radio_state.sta_ip, ip_str, sizeof(g_radio_state.sta_ip) - 1);
            g_radio_state.sta_ip[sizeof(g_radio_state.sta_ip) - 1] = '\0';
        } else {
            g_radio_state.sta_ip[0] = '\0';
        }
        xSemaphoreGive(g_radio_state.mutex);
    }
    ui_st7789_notify(UI_NOTIFY_WIFI);
}

void radio_state_get_ip(char *out_ip, size_t ip_len)
{
    if (out_ip == NULL || ip_len == 0) {
        return;
    }
    if (xSemaphoreTake(g_radio_state.mutex, portMAX_DELAY) == pdTRUE) {
        strncpy(out_ip, g_radio_state.sta_ip, ip_len - 1);
        out_ip[ip_len - 1] = '\0';
        xSemaphoreGive(g_radio_state.mutex);
    }
}

void radio_state_set_volume(uint8_t vol_pct)
{
    if (vol_pct > 100U) {
        vol_pct = 100U;
    }
    g_radio_state.volume = vol_pct;
    ui_st7789_notify(UI_NOTIFY_VOLUME);
}

void radio_state_cycle_station(int delta)
{
    int idx = (int)g_radio_state.station_index + delta;
    if (idx < 0) {
        idx = (int)NUM_RADIO_PRESETS - 1;
    } else if (idx >= (int)NUM_RADIO_PRESETS) {
        idx = 0;
    }

    radio_state_set_station(kRadioPresets[idx].name, kRadioPresets[idx].url, (uint8_t)idx);
    radio_state_set_song_title("Connecting to station stream...");

    if (g_radio_event_group != NULL) {
        xEventGroupSetBits(g_radio_event_group, EVT_STATION_CHANGED);
    }
}

/* =========================================================================
 * ESP-IDF Entry Point: app_main(void)
 * ========================================================================= */
void app_main(void)
{
    /* Tat log loi noi bo cua esp-tls / HTTP_CLIENT khi server radio timeout */
    esp_log_level_set("esp-tls",        ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("HTTP_CLIENT",    ESP_LOG_NONE);

    printf("\n==========================================================\n");
    printf(" ESP32 WIFI WEB RADIO - PURE ESP-IDF v5.3+ FREERTOS ARCH  \n");
    printf("==========================================================\n");
    ESP_LOGI(TAG, "Booting ESP32 WiFi Web Radio...");

    /* 1. Khoi tao NVS Flash phuc vu WiFi & Luu cau hinh */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Khoi tao Struct Trang thai Tinh (g_radio_state) & Mutex Tinh */
    memset(&g_radio_state, 0, sizeof(g_radio_state));
    g_radio_state.mutex = xSemaphoreCreateMutexStatic(&g_radio_state.mutex_storage);
    configASSERT(g_radio_state.mutex != NULL);

    strncpy(g_radio_state.station_name, kRadioPresets[0].name, sizeof(g_radio_state.station_name) - 1);
    strncpy(g_radio_state.stream_url,   kRadioPresets[0].url,  sizeof(g_radio_state.stream_url) - 1);
    strncpy(g_radio_state.song_title,   "June Halberd - Slow River",
            sizeof(g_radio_state.song_title) - 1);

    g_radio_state.volume        = 75;
    g_radio_state.station_index = 0;
    g_radio_state.lcd_mode      = RADIO_LCD_MODE;
    g_radio_state.wifi_rssi     = -55;
    g_radio_state.disc_angle    = 0;

    /* Doc che do man hinh LED tu NVS (dong bo voi RADIO_LCD_MODE neu vua doi trong code) */
    {
        nvs_handle_t nvs;
        if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs) == ESP_OK) {
            uint8_t saved_def  = 0xFF;
            uint8_t saved_mode = RADIO_LCD_MODE;
            if (nvs_get_u8(nvs, "lcd_def", &saved_def) == ESP_OK &&
                saved_def == (uint8_t)RADIO_LCD_MODE &&
                nvs_get_u8(nvs, "lcd_mode", &saved_mode) == ESP_OK) {
                g_radio_state.lcd_mode = (saved_mode == LCD_MODE_ST7735_128X160)
                                         ? LCD_MODE_ST7735_128X160
                                         : LCD_MODE_ST7789_240X240;
            } else {
                nvs_set_u8(nvs, "lcd_def",  (uint8_t)RADIO_LCD_MODE);
                nvs_set_u8(nvs, "lcd_mode", (uint8_t)RADIO_LCD_MODE);
                nvs_commit(nvs);
            }
            nvs_close(nvs);
        }
    }

    atomic_init(&g_radio_state.is_playing,     true);
    atomic_init(&g_radio_state.is_buffering,   true);
    atomic_init(&g_radio_state.wifi_connected, false);
    atomic_init(&g_radio_state.is_video_mode,  false);

    /* 3. Khoi tao EventGroup Tinh & StreamBuffer (48 KB, Trigger 1024 Bytes) */
    g_radio_event_group = xEventGroupCreateStatic(&s_radio_event_group_storage);
    configASSERT(g_radio_event_group != NULL);

    g_audio_stream_buf = xStreamBufferCreate(STREAM_BUF_SIZE_BYTES, STREAM_BUF_TRIGGER_BYTES);
    configASSERT(g_audio_stream_buf != NULL);

    /* Dat co EVT_PLAY_REQUESTED ban dau de san sang phat ngay khi co WiFi */
    xEventGroupSetBits(g_radio_event_group, EVT_PLAY_REQUESTED);

    /* 4. Khoi tao UI ST7789 (Core 1, Priority 4, SPI 40MHz Dirty-Rectangle) */
    ESP_ERROR_CHECK(ui_st7789_init());

    /* 5. Khoi tao Audio I2S MAX98357A (Core 1 Prio 16) & HTTP Stream (Core 0 Prio 6) */
    ESP_ERROR_CHECK(audio_stream_init());

    /* 6. Khoi tao Ngat cung Nut nhan GPIO 0 (IRAM_ATTR, 200ms Debounce) */
    ESP_ERROR_CHECK(button_handler_init());

    /* 7. Khoi tao WiFi APSTA (WIFI_PS_NONE) & Web Server (Core 0, Priority 5) */
    ESP_ERROR_CHECK(wifi_web_init());

    printf("[RADIO_MAIN] All FreeRTOS tasks running! Free internal heap: %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("[RADIO_MAIN] Press BOOT (GPIO 0) to Toggle Play/Pause instantly.\n");
}
