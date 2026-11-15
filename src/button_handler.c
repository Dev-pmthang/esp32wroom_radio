/*
 * button_handler.c
 * Xu ly ngat cung GPIO 0 (IRAM_ATTR, Debounce 200ms bang esp_timer_get_time)
 * va Task dieu khien Toggle Play / Pause tuc thi (Zero-Blocking).
 */
#include "button_handler.h"
#include "radio_config.h"
#include "audio_stream.h"
#include "ui_st7789.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <limits.h>

static const char *TAG = "BTN_GPIO0";

/* Thoi diem ngat hop le cuoi cung (microseconds) phuc vu Debounce 200ms trong ISR */
static volatile int64_t s_last_isr_us = 0;

/* =========================================================================
 * ISR Ngat cung GPIO 0 (IRAM_ATTR)
 * - Debounce 200ms bang esp_timer_get_time() ngay trong ISR
 * - Ban tin hieu qua xTaskNotifyFromISR() (khong goi log/SPI/I2C trong ISR)
 * ========================================================================= */
static void IRAM_ATTR gpio0_isr_handler(void *arg)
{
    (void)arg;
    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_last_isr_us) < BTN_DEBOUNCE_US) {
        return;
    }
    s_last_isr_us = now_us;

    BaseType_t hp_task_woken = pdFALSE;
    if (g_btn_task_handle != NULL) {
        xTaskNotifyFromISR(g_btn_task_handle, 1UL, eSetBits, &hp_task_woken);
        if (hp_task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

/* =========================================================================
 * Thuc thi chuyen doi trang thai Play / Pause tuc thi
 * ========================================================================= */
void button_set_playback_state(bool play)
{
    bool current = atomic_load(&g_radio_state.is_playing);
    if (current == play) {
        return;
    }

    if (!play) {
        /* -----------------------------------------------------------------
         * NHAN LAN 1 -> CHUYEN SANG PAUSE:
         * 1. Keo ngay GPIO 33 (SD) = 0 (Mute phan cung tuc thi, khong cho xa DMA)
         * 2. Dat is_playing = false
         * 3. Danh thuc UI de dung xoay dia than ngay tai goc hien tai & doi icon
         * 4. Bao hieu http_stream_task ngat luong HTTP va xa sach StreamBuffer
         * ----------------------------------------------------------------- */
        audio_hw_set_unmute(false); /* GPIO 33 = 0 ngay lap tuc */

        atomic_store(&g_radio_state.is_playing, false);
        atomic_store(&g_radio_state.is_buffering, false);

        xEventGroupClearBits(g_radio_event_group, EVT_PLAY_REQUESTED | EVT_PREBUFFER_READY);
        xEventGroupSetBits(g_radio_event_group, EVT_STREAM_ABORT_REQ);

        ui_st7789_notify(UI_NOTIFY_PLAY_STATE);

        ESP_LOGI(TAG, "State -> PAUSED (GPIO33 SD=0 Muted instantly, Stream stopped)");
    } else {
        /* -----------------------------------------------------------------
         * NHAN LAN 2 -> CHUYEN SANG PLAY:
         * 1. Dat is_playing = true, is_buffering = true
         * 2. Kich hoat lai HTTP stream & nap Pre-buffer (>= 50%)
         * 3. Khi Pre-buffer day, audio_i2s_task mo lai GPIO 33 (SD) = 1
         * 4. UI tiep tuc xoay dia than muot ma tu goc hien tai
         * ----------------------------------------------------------------- */
        atomic_store(&g_radio_state.is_playing, true);
        atomic_store(&g_radio_state.is_buffering, true);

        xEventGroupClearBits(g_radio_event_group, EVT_STREAM_ABORT_REQ | EVT_PREBUFFER_READY);
        xEventGroupSetBits(g_radio_event_group, EVT_PLAY_REQUESTED);

        ui_st7789_notify(UI_NOTIFY_PLAY_STATE | UI_NOTIFY_BUFFERING);

        ESP_LOGI(TAG, "State -> PLAYING (Re-activating HTTP Stream & Pre-buffer)");
    }
}

void button_toggle_play_pause(void)
{
    bool currently_playing = atomic_load(&g_radio_state.is_playing);
    button_set_playback_state(!currently_playing);
}

/* =========================================================================
 * Task xu ly su kien nut nhan GPIO 0 (Event-Driven qua xTaskNotifyWait)
 * Hoan toan Zero-Blocking / Zero-Polling khi khong co thao tac nhan nut.
 * ========================================================================= */
static void button_ctrl_task(void *arg)
{
    (void)arg;
    uint32_t notify_val = 0;

    for (;;) {
        if (xTaskNotifyWait(0UL, ULONG_MAX, &notify_val, portMAX_DELAY) == pdTRUE) {
            if (notify_val != 0UL) {
                button_toggle_play_pause();
            }
        }
    }
}

esp_err_t button_handler_init(void)
{
    /* 1. Cau hinh GPIO 0: Input, Pull-up noi, Ngat canh xuong (Falling Edge) */
    const gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << PIN_BTN_PLAY_PAUSE),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE
    };
    esp_err_t err = gpio_config(&btn_cfg);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_pull_mode(PIN_BTN_PLAY_PAUSE, GPIO_PULLUP_ONLY);

    /* 2. Tao Task xu ly nut nhan tren Core 0 (Priority 10) */
    BaseType_t ok = xTaskCreatePinnedToCore(
        button_ctrl_task,
        "button_ctrl_task",
        STACK_SIZE_BTN_CTRL,
        NULL,
        TASK_PRIO_BTN_CTRL,
        &g_btn_task_handle,
        CORE_NETWORK_CTRL
    );
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* 3. Cai dat dich vu ISR GPIO va gan handler IRAM_ATTR cho GPIO 0 */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(PIN_BTN_PLAY_PAUSE, gpio0_isr_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "GPIO 0 ISR initialized (Pull-up, NEGEDGE, 200ms IRAM debounce)");
    return ESP_OK;
}
