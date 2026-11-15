/*
 * button_handler.c
 * Xu ly ngat cung GPIO 0 (IRAM_ATTR, Debounce 200ms bang esp_timer_get_time)
 * va Task dieu khien Toggle Play / Pause / Resume tuc thi (Zero-Blocking).
 * Chinh xac 1 lan nhan = 1 lan chuyen trang thai (khu triet de doi xung khi nha nut).
 */
#include "button_handler.h"
#include "radio_config.h"
#include "audio_stream.h"
#include "ui_st7789.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <stdio.h>
#include <limits.h>

static const char *TAG = "BTN_GPIO0";

/* Thoi diem ngat hop le cuoi cung (microseconds) phuc vu Debounce 200ms trong ISR */
static volatile int64_t s_last_isr_us = 0;

/* =========================================================================
 * ISR Ngat cung GPIO 0 (IRAM_ATTR)
 * - Kiem tra muc logic LOW (nhan xuong) + Debounce 200ms bang esp_timer_get_time()
 * - Ban tin hieu qua xTaskNotifyFromISR() (khong goi log/SPI/I2C trong ISR)
 * ========================================================================= */
static void IRAM_ATTR gpio0_isr_handler(void *arg)
{
    (void)arg;
    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_last_isr_us) < BTN_DEBOUNCE_US) {
        return;
    }
    if (gpio_get_level(PIN_BTN_PLAY_PAUSE) != 0) {
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
 * Thuc thi chuyen doi trang thai Play / Pause / Resume tuc thi
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
         * 3. Danh thuc UI de dung xoay dia than ngay tai goc hien tai & doi icon Play
         * 4. Bao hieu http_stream_task & audio_i2s_task tam dung ngay lap tuc
         * ----------------------------------------------------------------- */
        audio_hw_set_unmute(false); /* GPIO 33 = 0 ngay lap tuc */

        atomic_store(&g_radio_state.is_playing, false);
        atomic_store(&g_radio_state.is_buffering, false);

        xEventGroupClearBits(g_radio_event_group, EVT_PLAY_REQUESTED);
        xEventGroupSetBits(g_radio_event_group, EVT_STREAM_ABORT_REQ);

        ui_st7789_notify(UI_NOTIFY_PLAY_STATE);

        printf("[BTN_GPIO0] State -> PAUSED (GPIO33 SD=0 Muted instantly, Vinyl stopped at %u deg)\n",
               (unsigned)g_radio_state.disc_angle);
        ESP_LOGI(TAG, "State -> PAUSED");
    } else {
        /* -----------------------------------------------------------------
         * NHAN LAN 2 -> CHUYEN SANG PLAY / RESUME:
         * 1. Dat is_playing = true
         * 2. Xoa co EVT_STREAM_ABORT_REQ va bat co EVT_PLAY_REQUESTED
         * 3. Mo lai GPIO 33 (SD) = 1 trong audio_i2s_task va phat tiep tuc thi
         * 4. UI tiep tuc xoay dia than muot ma tu goc hien tai & doi icon Pause
         * ----------------------------------------------------------------- */
        atomic_store(&g_radio_state.is_playing, true);

        xEventGroupClearBits(g_radio_event_group, EVT_STREAM_ABORT_REQ);
        xEventGroupSetBits(g_radio_event_group, EVT_PLAY_REQUESTED);

        ui_st7789_notify(UI_NOTIFY_PLAY_STATE | UI_NOTIFY_BUFFERING);

        printf("[BTN_GPIO0] State -> PLAYING / RESUMED (GPIO33 SD=1 Active, Vinyl rotating)\n");
        ESP_LOGI(TAG, "State -> PLAYING");
    }
}

void button_toggle_play_pause(void)
{
    bool currently_playing = atomic_load(&g_radio_state.is_playing);
    button_set_playback_state(!currently_playing);
}

/* =========================================================================
 * Task xu ly su kien nut nhan GPIO 0 (Event-Driven qua xTaskNotifyWait)
 * - Xac nhan chan GPIO 0 thuc su o muc LOW (nhan xuong) va cho nha nut an toan
 * - Dam bao 100% khong bao gio bi tu dong nhay trang thai khi nha tay khoi nut BOOT!
 * ========================================================================= */
static void button_ctrl_task(void *arg)
{
    (void)arg;
    uint32_t notify_val = 0;

    for (;;) {
        /* Ngu hoan toan (Zero CPU) cho den khi ISR GPIO 0 danh thuc */
        if (xTaskNotifyWait(0UL, ULONG_MAX, &notify_val, portMAX_DELAY) == pdTRUE) {
            /* Cho 15ms de xung nhan on dinh va kiem tra GPIO 0 thuc su dang duoc nhan (LOW) */
            (void)xTaskNotifyWait(0UL, ULONG_MAX, NULL, pdMS_TO_TICKS(15));
            if (gpio_get_level(PIN_BTN_PLAY_PAUSE) != 0) {
                /* Bo qua neu chi la xung nhieu khi nha nut */
                continue;
            }

            /* Thuc thi Toggle Play / Pause ngay lap tuc khi nhan xuong */
            button_toggle_play_pause();

            /* Cho den khi nguoi dung nha tay khoi nut GPIO 0 (muc HIGH) */
            while (gpio_get_level(PIN_BTN_PLAY_PAUSE) == 0) {
                (void)xTaskNotifyWait(0UL, ULONG_MAX, NULL, pdMS_TO_TICKS(20));
            }

            /* Khu rung co hoc khi nha nut (50ms) va xoa sach moi xung ngat phu */
            (void)xTaskNotifyWait(0UL, ULONG_MAX, NULL, pdMS_TO_TICKS(50));
            s_last_isr_us = esp_timer_get_time();
            (void)xTaskNotifyStateClear(NULL);
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

    ESP_LOGI(TAG, "GPIO 0 ISR initialized (Pull-up, NEGEDGE, press+release bounce filter)");
    return ESP_OK;
}
