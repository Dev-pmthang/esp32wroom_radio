/*
 * wifi_web.c
 * Quan ly WiFi STA/AP (esp_wifi_set_ps(WIFI_PS_NONE) chong nhieu nguon)
 * va Embedded Web Server dieu khien Radio (esp_http_server) tren Core 0 (Priority 5).
 * Tu dong cap nhat dia chi IP len goc trai man hinh ST7789 ngay khi ket noi WiFi thanh cong.
 */
#include "wifi_web.h"
#include "radio_config.h"
#include "button_handler.h"
#include "ui_st7789.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "WIFI_WEB";

/* EventGroup noi bo cho trang thai ket noi WiFi */
#define WIFI_INTERNAL_STA_GOT_IP    (1U << 0)
#define WIFI_INTERNAL_STA_DISCONN   (1U << 1)

static EventGroupHandle_t s_wifi_evt_group = NULL;
static StaticEventGroup_t s_wifi_evt_storage;
static httpd_handle_t     s_http_server    = NULL;

/* Giao dien Web dieu khien + Cau hinh WiFi luu co dinh trong Flash (Zero RAM) */
static const char kWebIndexHtml[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 Vinyl Player</title>"
    "<style>"
    "body{background:#f4f1ea;color:#1c1c1c;font-family:sans-serif;text-align:center;padding:18px;}"
    ".card{max-width:380px;margin:auto;background:#ffffff;border:1px solid #d4cfc4;border-radius:16px;padding:20px;box-shadow:0 8px 24px rgba(0,0,0,0.06);}"
    "h2{color:#1c1c1c;margin:0 0 4px 0;}"
    ".sub{color:#847e76;font-size:14px;margin-bottom:14px;}"
    ".ip{color:#c24b38;font-size:12px;font-weight:bold;margin-bottom:10px;}"
    "button{background:#e4e0d7;color:#1c1c1c;border:0;border-radius:20px;padding:10px 18px;margin:6px;font-size:15px;cursor:pointer;}"
    "button.play{background:#c24b38;color:#ffffff;font-weight:bold;padding:12px 24px;}"
    "input[type=range]{width:85%;accent-color:#c24b38;}"
    "input[type=text],input[type=password]{width:82%;padding:8px;margin:4px 0;border:1px solid #d0cbc2;border-radius:8px;}"
    "</style></head><body><div class='card'>"
    "<div class='ip' id='ip'>IP: Connecting...</div>"
    "<h2 id='ti'>Slow River</h2>"
    "<div class='sub' id='st'>Coastal Letters</div>"
    "<div>"
    "<button onclick=\"act('/api/prev')\">&laquo; Prev</button>"
    "<button class='play' id='pb' onclick=\"act('/api/toggle')\">PAUSE</button>"
    "<button onclick=\"act('/api/next')\">Next &raquo;</button>"
    "</div><div style='margin-top:14px'>"
    "Volume: <span id='vv'>75</span>%<br>"
    "<input type='range' id='vr' min='0' max='100' value='75' onchange='setVol(this.value)'>"
    "</div><hr style='border:0;border-top:1px solid #e4e0d7;margin:16px 0'>"
    "<div style='font-size:13px;color:#847e76'>WiFi Configuration</div>"
    "<input type='text' id='ws' placeholder='WiFi SSID'><br>"
    "<input type='password' id='wp' placeholder='WiFi Password'><br>"
    "<button onclick='saveWifi()'>Save &amp; Connect WiFi</button>"
    "</div>"
    "<script>"
    "function act(u){fetch(u,{method:'POST'}).then(()=>poll());}"
    "function setVol(v){document.getElementById('vv').innerText=v;fetch('/api/volume?val='+v,{method:'POST'});}"
    "function saveWifi(){const s=encodeURIComponent(document.getElementById('ws').value);"
    "const p=encodeURIComponent(document.getElementById('wp').value);"
    "fetch('/api/wifi?ssid='+s+'&pass='+p,{method:'POST'}).then(()=>alert('Connecting to WiFi...'));}"
    "function poll(){fetch('/api/status').then(r=>r.json()).then(d=>{"
    "document.getElementById('st').innerText=d.station;"
    "document.getElementById('ti').innerText=d.title;"
    "document.getElementById('ip').innerText=d.ip?('WiFi IP: '+d.ip):'AP Mode: 192.168.4.1';"
    "document.getElementById('pb').innerText=d.playing?'PAUSE':'PLAY';"
    "document.getElementById('vv').innerText=d.volume;"
    "document.getElementById('vr').value=d.volume;"
    "}).catch(()=>{});}"
    "setInterval(poll,1200);poll();"
    "</script></body></html>";

/* Giải mã URL-encoded cơ bản tại chỗ (Zero Malloc) */
static void url_decode_inplace(char *str)
{
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* =========================================================================
 * Cac HTTP Handlers (Su dung buffer tinh tren stack, Zero Malloc)
 * ========================================================================= */
static esp_err_t http_get_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kWebIndexHtml, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_status_handler(httpd_req_t *req)
{
    char station[32];
    char title[128];
    char ip_str[20];
    char json_buf[360];

    radio_state_get_strings(station, sizeof(station), title, sizeof(title));
    radio_state_get_ip(ip_str, sizeof(ip_str));
    bool playing   = atomic_load(&g_radio_state.is_playing);
    bool buffering = atomic_load(&g_radio_state.is_buffering);

    int len = snprintf(
        json_buf, sizeof(json_buf),
        "{\"station\":\"%s\",\"title\":\"%s\",\"ip\":\"%s\",\"playing\":%s,\"buffering\":%s,\"volume\":%u,\"rssi\":%d}",
        station, title, ip_str,
        playing ? "true" : "false",
        buffering ? "true" : "false",
        (unsigned)g_radio_state.volume,
        (int)g_radio_state.wifi_rssi
    );

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json_buf, len);
}

static esp_err_t http_post_toggle_handler(httpd_req_t *req)
{
    button_toggle_play_pause();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t http_post_next_handler(httpd_req_t *req)
{
    radio_state_cycle_station(1);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t http_post_prev_handler(httpd_req_t *req)
{
    radio_state_cycle_station(-1);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t http_post_volume_handler(httpd_req_t *req)
{
    char query[32];
    char val_str[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "val", val_str, sizeof(val_str)) == ESP_OK) {
            int v = atoi(val_str);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            radio_state_set_volume((uint8_t)v);
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t http_post_wifi_handler(httpd_req_t *req)
{
    char query[128];
    char ssid[33] = {0};
    char pass[65] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "ssid", ssid, sizeof(ssid));
        (void)httpd_query_key_value(query, "pass", pass, sizeof(pass));
        url_decode_inplace(ssid);
        url_decode_inplace(pass);

        if (ssid[0] != '\0') {
            nvs_handle_t nvs;
            if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_set_str(nvs, "ssid", ssid);
                nvs_set_str(nvs, "pass", pass);
                nvs_commit(nvs);
                nvs_close(nvs);
            }
            wifi_config_t sta_cfg = { 0 };
            strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
            strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
            esp_wifi_disconnect();
            esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
            esp_wifi_connect();
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void start_web_server(void)
{
    if (s_http_server != NULL) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id          = CORE_NETWORK_CTRL;
    config.task_priority    = TASK_PRIO_WIFI_WEB;
    config.stack_size       = 4096;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;

    if (httpd_start(&s_http_server, &config) == ESP_OK) {
        static const httpd_uri_t u_root   = { .uri = "/",           .method = HTTP_GET,  .handler = http_get_root_handler };
        static const httpd_uri_t u_status = { .uri = "/api/status", .method = HTTP_GET,  .handler = http_get_status_handler };
        static const httpd_uri_t u_toggle = { .uri = "/api/toggle", .method = HTTP_POST, .handler = http_post_toggle_handler };
        static const httpd_uri_t u_next   = { .uri = "/api/next",   .method = HTTP_POST, .handler = http_post_next_handler };
        static const httpd_uri_t u_prev   = { .uri = "/api/prev",   .method = HTTP_POST, .handler = http_post_prev_handler };
        static const httpd_uri_t u_vol    = { .uri = "/api/volume", .method = HTTP_POST, .handler = http_post_volume_handler };
        static const httpd_uri_t u_wifi   = { .uri = "/api/wifi",   .method = HTTP_POST, .handler = http_post_wifi_handler };

        httpd_register_uri_handler(s_http_server, &u_root);
        httpd_register_uri_handler(s_http_server, &u_status);
        httpd_register_uri_handler(s_http_server, &u_toggle);
        httpd_register_uri_handler(s_http_server, &u_next);
        httpd_register_uri_handler(s_http_server, &u_prev);
        httpd_register_uri_handler(s_http_server, &u_vol);
        httpd_register_uri_handler(s_http_server, &u_wifi);

        ESP_LOGI(TAG, "esp_http_server started on port 80 (Core 0)");
    }
}

/* =========================================================================
 * Event Handler cho WiFi & IP (Zero-Polling)
 * ========================================================================= */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&g_radio_state.wifi_connected, false);
        xEventGroupClearBits(g_radio_event_group, EVT_WIFI_CONNECTED);
        xEventGroupSetBits(s_wifi_evt_group, WIFI_INTERNAL_STA_DISCONN);
        radio_state_set_ip("");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_buf[20];
        snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&event->ip_info.ip));

        printf("[WIFI_WEB] WiFi Connected! STA IP: %s\n", ip_buf);
        ESP_LOGI(TAG, "WiFi STA Connected! IP: %s", ip_buf);

        atomic_store(&g_radio_state.wifi_connected, true);
        radio_state_set_ip(ip_buf);

        xEventGroupSetBits(g_radio_event_group, EVT_WIFI_CONNECTED);
        xEventGroupSetBits(s_wifi_evt_group, WIFI_INTERNAL_STA_GOT_IP);
    }
}

/* =========================================================================
 * CORE 0 (Priority 5): wifi_and_web_task
 * - Quan ly ket noi WiFi STA/AP & esp_wifi_set_ps(WIFI_PS_NONE)
 * - Khoi chay esp_http_server va cap nhat RSSI qua xEventGroupWaitBits timeout
 * ========================================================================= */
static void wifi_and_web_task(void *arg)
{
    (void)arg;
    start_web_server();

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_evt_group,
            WIFI_INTERNAL_STA_DISCONN | WIFI_INTERNAL_STA_GOT_IP,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(5000)
        );

        if ((bits & WIFI_INTERNAL_STA_DISCONN) != 0) {
            esp_wifi_connect();
        }

        if (atomic_load(&g_radio_state.wifi_connected)) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                int8_t old_rssi = g_radio_state.wifi_rssi;
                if (ap_info.rssi != old_rssi) {
                    g_radio_state.wifi_rssi = ap_info.rssi;
                    ui_st7789_notify(UI_NOTIFY_WIFI);
                }
            }
        }
    }
}

esp_err_t wifi_web_init(void)
{
    s_wifi_evt_group = xEventGroupCreateStatic(&s_wifi_evt_storage);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* Doc SSID/Password luu trong NVS (ho tro ca namespace "wifi_cfg" va "wifi-radio" cu) */
    wifi_config_t sta_cfg = { 0 };
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs) == ESP_OK) {
        size_t s_len = sizeof(sta_cfg.sta.ssid);
        size_t p_len = sizeof(sta_cfg.sta.password);
        (void)nvs_get_str(nvs, "ssid", (char *)sta_cfg.sta.ssid, &s_len);
        (void)nvs_get_str(nvs, "pass", (char *)sta_cfg.sta.password, &p_len);
        nvs_close(nvs);
    }
    if (sta_cfg.sta.ssid[0] == '\0' && nvs_open("wifi-radio", NVS_READONLY, &nvs) == ESP_OK) {
        size_t s_len = sizeof(sta_cfg.sta.ssid);
        size_t p_len = sizeof(sta_cfg.sta.password);
        (void)nvs_get_str(nvs, "ssid", (char *)sta_cfg.sta.ssid, &s_len);
        (void)nvs_get_str(nvs, "pass", (char *)sta_cfg.sta.password, &p_len);
        nvs_close(nvs);
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "WIFI_RADIO",
            .ssid_len = 10,
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    if (sta_cfg.sta.ssid[0] != '\0') {
        printf("[WIFI_WEB] Connecting to saved WiFi SSID: %s\n", (char *)sta_cfg.sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* BAT BUOC: Tat WiFi Power Save de triet tieu nhieu xung nguon len MAX98357A */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    BaseType_t ok = xTaskCreatePinnedToCore(
        wifi_and_web_task,
        "wifi_and_web_task",
        STACK_SIZE_WIFI_WEB,
        NULL,
        TASK_PRIO_WIFI_WEB,
        NULL,
        CORE_NETWORK_CTRL
    );
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WiFi APSTA initialized (WIFI_PS_NONE active, SoftAP: WIFI_RADIO)");
    return ESP_OK;
}
