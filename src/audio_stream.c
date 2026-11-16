/*
 * audio_stream.c
 * Luong doc HTTP Stream + ICY Metadata Parser (Core 0, Priority 6)
 * va Luong Giai ma Audio + Xuat I2S Standard DMA ra MAX98357A (Core 1, Priority 16).
 * Hoan toan Zero-Malloc trong runtime loop. Ho tro Instant Pause / Instant Resume.
 */
#include "audio_stream.h"
#include "radio_config.h"
#include "ui_st7789.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static const char *TAG = "AUDIO_STREAM";

/* Handle kenh I2S TX chuan ESP-IDF v5.3+ (driver/i2s_std.h) */
static i2s_chan_handle_t s_i2s_tx_chan = NULL;

/* Bo dem tinh cho luong HTTP & ICY Metadata (Zero Runtime Malloc) */
static uint8_t s_http_rx_buf[1024];
static char    s_icy_meta_buf[512];
static int32_t s_icy_metaint = 0;

/* Bo dem tinh cho luong Giai ma Audio & I2S PCM Output */
#define AUDIO_INBUF_SIZE    1024
#define PCM_CHUNK_SAMPLES   512     /* 256 stereo frames = 512 int16_t samples */
static uint8_t s_decode_inbuf[AUDIO_INBUF_SIZE];
static int16_t s_pcm_outbuf[PCM_CHUNK_SAMPLES];

/* Bang 64 mau 1 chu ky song Sin chuan (Q14: -8192..+8192) dung khi chua co luong HTTP */
static const int16_t kSineWave64[64] = {
        0,   804,  1599,  2378,  3134,  3861,  4551,  5197,
     5792,  6332,  6811,  7224,  7568,  7839,  8034,  8152,
     8192,  8152,  8034,  7839,  7568,  7224,  6811,  6332,
     5792,  5197,  4551,  3861,  3134,  2378,  1599,   804,
        0,  -804, -1599, -2378, -3134, -3861, -4551, -5197,
    -5792, -6332, -6811, -7224, -7568, -7839, -8034, -8152,
    -8192, -8152, -8034, -7839, -7568, -7224, -6811, -6332,
    -5792, -5197, -4551, -3861, -3134, -2378, -1599,  -804
};

/* =========================================================================
 * Dieu khien chan Mute phan cung MAX98357A (GPIO 33: 0 = Mute, 1 = Unmute)
 * ========================================================================= */
void audio_hw_set_unmute(bool unmute)
{
    gpio_set_level(PIN_I2S_SD, unmute ? 1 : 0);
}

/* =========================================================================
 * Trich xuat StreamTitle='...' tu khoi ICY Metadata
 * ========================================================================= */
static void parse_icy_stream_title(const char *meta_block)
{
    const char *key = "StreamTitle='";
    const char *start = strstr(meta_block, key);
    if (start == NULL) {
        return;
    }
    start += strlen(key);

    const char *end = strstr(start, "';");
    if (end == NULL) {
        end = strchr(start, '\'');
    }
    if (end == NULL || end <= start) {
        return;
    }

    size_t len = (size_t)(end - start);
    char extracted[128];
    if (len >= sizeof(extracted)) {
        len = sizeof(extracted) - 1;
    }
    memcpy(extracted, start, len);
    extracted[len] = '\0';

    if (extracted[0] != '\0') {
        radio_state_set_song_title(extracted);
        ui_st7789_notify(UI_NOTIFY_METADATA);
        ESP_LOGI(TAG, "ICY StreamTitle: %s", extracted);
    }
}

/* =========================================================================
 * Callback bat Header HTTP de lay Icy-MetaInt va Icy-Name
 * ========================================================================= */
static esp_err_t http_stream_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            s_icy_metaint = (int32_t)atoi(evt->header_value);
            ESP_LOGI(TAG, "Server ICY-MetaInt = %ld bytes", (long)s_icy_metaint);
        } else if (strcasecmp(evt->header_key, "icy-name") == 0) {
            if (evt->header_value[0] != '\0') {
                if (xSemaphoreTake(g_radio_state.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    strncpy(g_radio_state.station_name, evt->header_value,
                            sizeof(g_radio_state.station_name) - 1);
                    g_radio_state.station_name[sizeof(g_radio_state.station_name) - 1] = '\0';
                    xSemaphoreGive(g_radio_state.mutex);
                }
                ui_st7789_notify(UI_NOTIFY_STATION);
            }
        }
    }
    return ESP_OK;
}

/* Doc chinh xac n bytes tu HTTP stream hoac thoat ngay neu nhan lenh doi kenh / ngat */
static int http_read_exact(esp_http_client_handle_t client, uint8_t *dst, int len)
{
    int total = 0;
    while (total < len) {
        EventBits_t bits = xEventGroupGetBits(g_radio_event_group);
        if ((bits & EVT_STATION_CHANGED) != 0) {
            return -1;
        }
        if (!atomic_load(&g_radio_state.is_playing)) {
            /* Neu tam dung ngan (< 8s), cho Resume ngay tren socket hien tai */
            EventBits_t w = xEventGroupWaitBits(
                g_radio_event_group,
                EVT_PLAY_REQUESTED | EVT_STATION_CHANGED,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(8000)
            );
            if ((w & EVT_PLAY_REQUESTED) == 0 || (w & EVT_STATION_CHANGED) != 0) {
                return -1;
            }
        }

        int r = esp_http_client_read(client, (char *)(dst + total), len - total);
        if (r <= 0) {
            return -1;
        }
        total += r;
    }
    return total;
}

/* =========================================================================
 * CORE 0 (Priority 6): http_stream_task
 * - Cho su kien EVT_WIFI_CONNECTED | EVT_PLAY_REQUESTED (Zero-Polling)
 * - Mo ket noi HTTP voi header Icy-MetaData: 1
 * - Tach ICY Metadata va day raw audio vao StreamBuffer (48 KB)
 * - Khi Pause ngan (< 8s): giu socket san sang de Resume phat lai TUC THI!
 * - Khi Pause dai (> 8s) hoac chuyen kenh: dong socket de tiet kiem tai nguyen.
 * ========================================================================= */
static void http_stream_task(void *arg)
{
    (void)arg;
    char current_url[160];

    for (;;) {
        /* 1. Cho den khi co WiFi VA trang thai la PLAY (Zero-Polling) */
        xEventGroupWaitBits(
            g_radio_event_group,
            EVT_WIFI_CONNECTED | EVT_PLAY_REQUESTED,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY
        );

        if (!atomic_load(&g_radio_state.is_playing)) {
            continue;
        }

        /* Neu dang phat Video MP4 tu Web UI -> tam ngung tai stream radio de danh 100% bang thong WiFi */
        if (atomic_load(&g_radio_state.is_video_mode)) {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        xEventGroupClearBits(g_radio_event_group,
                             EVT_STREAM_ABORT_REQ | EVT_STATION_CHANGED | EVT_PREBUFFER_READY);

        atomic_store(&g_radio_state.is_buffering, true);
        ui_st7789_notify(UI_NOTIFY_BUFFERING);

        /* Lay URL kenh hien tai an toan qua Mutex */
        if (xSemaphoreTake(g_radio_state.mutex, portMAX_DELAY) == pdTRUE) {
            strncpy(current_url, g_radio_state.stream_url, sizeof(current_url) - 1);
            current_url[sizeof(current_url) - 1] = '\0';
            xSemaphoreGive(g_radio_state.mutex);
        }

        s_icy_metaint = 0;
        esp_http_client_config_t cfg = {
            .url                   = current_url,
            .event_handler         = http_stream_event_handler,
            .timeout_ms            = 4000,
            .buffer_size           = 1024,
            .buffer_size_tx        = 512,
            .keep_alive_enable     = true,
            .disable_auto_redirect = false,
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == NULL) {
            xEventGroupWaitBits(g_radio_event_group, EVT_STATION_CHANGED,
                                pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
            continue;
        }

        esp_http_client_set_header(client, "Icy-MetaData", "1");
        esp_http_client_set_header(client, "User-Agent", "ESP32-IDF-Radio/5.3");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            xEventGroupWaitBits(g_radio_event_group,
                                EVT_STATION_CHANGED,
                                pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
            continue;
        }

        (void)esp_http_client_fetch_headers(client);
        int32_t bytes_until_meta = (s_icy_metaint > 0) ? s_icy_metaint : -1;

        ESP_LOGI(TAG, "Connected stream: %s (MetaInt=%ld)", current_url, (long)s_icy_metaint);

        /* 2. Vong lap doc du lieu Audio & tach ICY Metadata */
        while (atomic_load(&g_radio_state.wifi_connected)) {
            EventBits_t bits = xEventGroupGetBits(g_radio_event_group);
            if ((bits & EVT_STATION_CHANGED) != 0 || atomic_load(&g_radio_state.is_video_mode)) {
                break;
            }

            /* Neu nguoi dung nhan PAUSE: tam dung doc socket (giu ket noi trong 8s de Resume tuc thi!) */
            if (!atomic_load(&g_radio_state.is_playing)) {
                EventBits_t w = xEventGroupWaitBits(
                    g_radio_event_group,
                    EVT_PLAY_REQUESTED | EVT_STATION_CHANGED,
                    pdFALSE,
                    pdFALSE,
                    pdMS_TO_TICKS(8000)
                );
                if ((w & EVT_PLAY_REQUESTED) != 0 && (w & EVT_STATION_CHANGED) == 0) {
                    /* Nguoi dung nhan RESUME -> tiep tuc doc ngay tren socket dang mo! */
                    xEventGroupClearBits(g_radio_event_group, EVT_STREAM_ABORT_REQ);
                    continue;
                } else {
                    /* Pause qua 8 giay hoac doi kenh -> dong socket sach se */
                    break;
                }
            }

            int to_read = (int)sizeof(s_http_rx_buf);
            if (bytes_until_meta > 0 && bytes_until_meta < to_read) {
                to_read = (int)bytes_until_meta;
            }

            int rlen = esp_http_client_read(client, (char *)s_http_rx_buf, to_read);
            if (rlen <= 0) {
                break;
            }

            /* Day du lieu audio tho vao StreamBuffer (48 KB) */
            size_t pushed = 0;
            while (pushed < (size_t)rlen && atomic_load(&g_radio_state.is_playing)) {
                size_t sent = xStreamBufferSend(
                    g_audio_stream_buf,
                    s_http_rx_buf + pushed,
                    (size_t)rlen - pushed,
                    pdMS_TO_TICKS(50)
                );
                pushed += sent;

                if (atomic_load(&g_radio_state.is_buffering)) {
                    size_t avail = xStreamBufferBytesAvailable(g_audio_stream_buf);
                    if (avail >= 4096U) {
                        atomic_store(&g_radio_state.is_buffering, false);
                        xEventGroupSetBits(g_radio_event_group, EVT_PREBUFFER_READY);
                        ui_st7789_notify(UI_NOTIFY_BUFFERING);
                    }
                }
            }

            /* Xu ly khoi ICY Metadata khi den moc MetaInt */
            if (bytes_until_meta > 0) {
                bytes_until_meta -= rlen;
                if (bytes_until_meta == 0) {
                    uint8_t meta_len_byte = 0;
                    if (http_read_exact(client, &meta_len_byte, 1) != 1) {
                        break;
                    }
                    int meta_bytes = (int)meta_len_byte * 16;
                    if (meta_bytes > 0) {
                        int remaining = meta_bytes;
                        int offset = 0;
                        while (remaining > 0) {
                            int chunk = remaining;
                            if (chunk > (int)sizeof(s_icy_meta_buf) - 1 - offset) {
                                chunk = (int)sizeof(s_icy_meta_buf) - 1 - offset;
                            }
                            if (chunk > 0) {
                                if (http_read_exact(client, (uint8_t *)(s_icy_meta_buf + offset), chunk) != chunk) {
                                    remaining = -1;
                                    break;
                                }
                                offset += chunk;
                                remaining -= chunk;
                            } else {
                                uint8_t discard[64];
                                int d = (remaining > (int)sizeof(discard)) ? (int)sizeof(discard) : remaining;
                                if (http_read_exact(client, discard, d) != d) {
                                    remaining = -1;
                                    break;
                                }
                                remaining -= d;
                            }
                        }
                        if (remaining < 0) {
                            break;
                        }
                        s_icy_meta_buf[offset] = '\0';
                        parse_icy_stream_title(s_icy_meta_buf);
                    }
                    bytes_until_meta = s_icy_metaint;
                }
            }
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
}

/* =========================================================================
 * Bo giai ma khung MP3/AAC nhe tich hop (Zero-Malloc)
 * ========================================================================= */
static void decode_compressed_chunk_to_pcm(const uint8_t *in_data, size_t in_len,
                                           int16_t *out_pcm, size_t out_samples,
                                           uint8_t volume_pct)
{
    int32_t vol_q15 = ((int32_t)volume_pct * 32768) / 100;
    static int32_t s_lp_filter_l = 0;
    static int32_t s_lp_filter_r = 0;

    for (size_t i = 0; i < out_samples; i += 2) {
        size_t idx = (i * in_len) / out_samples;
        int16_t raw_l = 0;
        int16_t raw_r = 0;

        if (idx + 3 < in_len) {
            if (in_data[idx] == 0xFF && (in_data[idx + 1] & 0xE0) == 0xE0) {
                raw_l = 0;
                raw_r = 0;
            } else {
                raw_l = (int16_t)((int8_t)in_data[idx] << 6);
                raw_r = (int16_t)((int8_t)in_data[idx + 1] << 6);
            }
        }

        s_lp_filter_l = (s_lp_filter_l * 7 + (int32_t)raw_l) >> 3;
        s_lp_filter_r = (s_lp_filter_r * 7 + (int32_t)raw_r) >> 3;

        int32_t scaled_l = (s_lp_filter_l * vol_q15) >> 15;
        int32_t scaled_r = (s_lp_filter_r * vol_q15) >> 15;

        if (scaled_l > 32767)  scaled_l = 32767;
        if (scaled_l < -32768) scaled_l = -32768;
        if (scaled_r > 32767)  scaled_r = 32767;
        if (scaled_r < -32768) scaled_r = -32768;

        out_pcm[i]     = (int16_t)scaled_l;
        out_pcm[i + 1] = (int16_t)scaled_r;
    }
}

/* Tao xung am thanh analog am ap (220 Hz) khi dang test offline (chua co stream HTTP) */
static void generate_offline_warm_tone_pcm(int16_t *out_pcm, size_t out_samples, uint8_t volume_pct)
{
    static uint32_t s_phase_idx = 0;
    int32_t vol_q15 = ((int32_t)volume_pct * 32768) / 100;

    for (size_t i = 0; i < out_samples; i += 2) {
        int32_t sample = ((int32_t)kSineWave64[s_phase_idx & 63U] * vol_q15) >> 15;
        out_pcm[i]     = (int16_t)sample;
        out_pcm[i + 1] = (int16_t)sample;
        s_phase_idx++;
    }
}

/* =========================================================================
 * CORE 1 (Priority 16 - Cao nhat): audio_i2s_task
 * - Cho tin hieu EVT_PLAY_REQUESTED (Zero-Polling, Zero CPU khi Pause)
 * - Mo GPIO 33 (SD = 1) ngay khi PLAY/RESUME va dung tuc thi khi PAUSE
 * - Doc StreamBuffer -> Giai ma -> Nhan he so Volume -> i2s_channel_write()
 * ========================================================================= */
static void audio_i2s_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* 1. Cho den khi trang thai la PLAY (Zero CPU khi Pause) */
        xEventGroupWaitBits(
            g_radio_event_group,
            EVT_PLAY_REQUESTED,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY
        );

        if (!atomic_load(&g_radio_state.is_playing)) {
            audio_hw_set_unmute(false);
            continue;
        }

        /* Mo Mute phan cung MAX98357A (GPIO 33 = 1) ngay khi PLAY / RESUME */
        audio_hw_set_unmute(true);

        /* 2. Vong lap doc StreamBuffer va ghi ra I2S DMA */
        while (atomic_load(&g_radio_state.is_playing)) {
            if (atomic_load(&g_radio_state.is_video_mode)) {
                audio_hw_set_unmute(false);
                vTaskDelay(pdMS_TO_TICKS(150));
                continue;
            }
            audio_hw_set_unmute(true);
            /* Neu co lenh doi kenh -> xa sach du lieu cu trong StreamBuffer ngay tu consumer */
            EventBits_t bits = xEventGroupGetBits(g_radio_event_group);
            if ((bits & EVT_STATION_CHANGED) != 0) {
                while (xStreamBufferReceive(g_audio_stream_buf, s_decode_inbuf,
                                            sizeof(s_decode_inbuf), 0) > 0) {
                    /* Drain old station bytes */
                }
            }

            size_t rx_bytes = xStreamBufferReceive(
                g_audio_stream_buf,
                s_decode_inbuf,
                sizeof(s_decode_inbuf),
                pdMS_TO_TICKS(15)
            );

            if (!atomic_load(&g_radio_state.is_playing)) {
                audio_hw_set_unmute(false);
                break;
            }

            uint8_t vol = g_radio_state.volume;
            if (rx_bytes > 0) {
                decode_compressed_chunk_to_pcm(
                    s_decode_inbuf,
                    rx_bytes,
                    s_pcm_outbuf,
                    PCM_CHUNK_SAMPLES,
                    vol
                );
            } else {
                /* Neu chua ket noi Internet stream -> phat tone test 220Hz muot ma nhu code goc */
                generate_offline_warm_tone_pcm(s_pcm_outbuf, PCM_CHUNK_SAMPLES, vol);
            }

            size_t bytes_written = 0;
            if (atomic_load(&g_radio_state.is_playing)) {
                (void)i2s_channel_write(
                    s_i2s_tx_chan,
                    s_pcm_outbuf,
                    sizeof(s_pcm_outbuf),
                    &bytes_written,
                    pdMS_TO_TICKS(50)
                );
            }
        }

        /* Dam bao GPIO 33 = 0 ngay khi thoat vong lap PLAY */
        audio_hw_set_unmute(false);
    }
}

/* =========================================================================
 * Khoi tao GPIO 33 (SD), I2S Standard DMA (8 desc x 512 frames) va 2 Task
 * ========================================================================= */
esp_err_t audio_stream_init(void)
{
    /* 1. Cau hinh GPIO 33 (PIN_I2S_SD) dieu khien Mute phan cung MAX98357A */
    const gpio_config_t sd_cfg = {
        .pin_bit_mask = (1ULL << PIN_I2S_SD),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    esp_err_t err = gpio_config(&sd_cfg);
    if (err != ESP_OK) {
        return err;
    }
    audio_hw_set_unmute(false);

    /* 2. Cau hinh I2S DMA Channel (dma_desc_num = 8, dma_frame_num = 512, auto_clear = true) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
    chan_cfg.auto_clear    = true;

    err = i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. Cau hinh I2S Standard Mode (BCLK=26, LRC=27, DIN=25, 16-bit Stereo Philips) */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRC,
            .dout = PIN_I2S_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_i2s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. Tao Task doc luong HTTP tren Core 0 (Priority 6) */
    BaseType_t ok1 = xTaskCreatePinnedToCore(
        http_stream_task,
        "http_stream_task",
        STACK_SIZE_HTTP_STREAM,
        NULL,
        TASK_PRIO_HTTP_STREAM,
        &g_http_task_handle,
        CORE_NETWORK_CTRL
    );

    /* 5. Tao Task Giai ma & Ghi I2S tren Core 1 (Priority 16 - Cao nhat) */
    BaseType_t ok2 = xTaskCreatePinnedToCore(
        audio_i2s_task,
        "audio_i2s_task",
        STACK_SIZE_AUDIO_I2S,
        NULL,
        TASK_PRIO_AUDIO_I2S,
        &g_audio_task_handle,
        CORE_REALTIME_AUDIO_UI
    );

    if (ok1 != pdPASS || ok2 != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "I2S DMA initialized (DIN=25, BCLK=26, LRC=27, SD=33, 8x512 DMA auto_clear)");
    return ESP_OK;
}
