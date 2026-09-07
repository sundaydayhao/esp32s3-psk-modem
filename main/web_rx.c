#include "web_rx.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_config.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "rx_history.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static rx_history_t s_history;
static web_rx_status_t s_status;
static QueueHandle_t s_controls;
static httpd_handle_t s_server;
static uint32_t s_session;
static const char *TAG = "web-rx";
extern const char page_start[] asm("_binary_index_html_start");
extern const char page_end[] asm("_binary_index_html_end");

void web_rx_publish(const uint8_t *data, size_t len)
{
    portENTER_CRITICAL(&s_lock);
    rx_history_append(&s_history, data, len);
    portEXIT_CRITICAL(&s_lock);
}

void web_rx_update_status(const web_rx_status_t *status)
{
    portENTER_CRITICAL(&s_lock);
    s_status = *status;
    portEXIT_CRITICAL(&s_lock);
}

bool web_rx_take_control(ui_event_t *event)
{
    return s_controls && xQueueReceive(s_controls, event, 0) == pdTRUE;
}

static void no_cache(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

static esp_err_t page_get(httpd_req_t *req)
{
    no_cache(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

static bool parse_uint(const char *s, uint64_t *value)
{
    if (!s[0]) return false;
    for (const char *p = s; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0;
    *value = strtoull(s, NULL, 10);
    return errno == 0;
}

static esp_err_t rx_get(httpd_req_t *req)
{
    char query[128], value[32];
    uint64_t requested = 0, session = 0, start, lost;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "since", value, sizeof(value)) != ESP_OK ||
        !parse_uint(value, &requested)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected decimal since and session");
    }
    if (httpd_query_key_value(query, "session", value, sizeof(value)) != ESP_OK ||
        !parse_uint(value, &session)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid session");
    }
    uint8_t data[512];
    portENTER_CRITICAL(&s_lock);
    if (session != s_session) requested = s_history.end > RX_HISTORY_CAPACITY ? s_history.end - RX_HISTORY_CAPACITY : 0;
    size_t count = rx_history_read(&s_history, requested, data, sizeof(data), &start, &lost);
    portEXIT_CRITICAL(&s_lock);
    char next_buf[24], lost_buf[24], session_buf[16];
    snprintf(next_buf, sizeof(next_buf), "%" PRIu64, start + count);
    snprintf(lost_buf, sizeof(lost_buf), "%" PRIu64, lost);
    snprintf(session_buf, sizeof(session_buf), "%" PRIu32, s_session);
    no_cache(req);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-RX-Next", next_buf);
    httpd_resp_set_hdr(req, "X-RX-Lost", lost_buf);
    httpd_resp_set_hdr(req, "X-RX-Session", session_buf);
    return httpd_resp_send(req, (const char *)data, count);
}

static esp_err_t status_get(httpd_req_t *req)
{
    web_rx_status_t s;
    uint64_t total;
    portENTER_CRITICAL(&s_lock);
    s = s_status;
    total = s_history.end;
    portEXIT_CRITICAL(&s_lock);
    bool psk = s.mode == MODEM_MODE_PSK63 || s.mode == MODEM_MODE_BPSK31;
    const psk_mode_info_t *info = psk_mode_info(s.psk_mode);
    float raw = psk ? info->baud * info->bits_per_symbol * info->carriers : 0;
    float net = raw * (info->fec ? (info->punctured ? 2.0f / 3.0f : 0.5f) : 1.0f);
    char json[1200];
    int n = snprintf(json, sizeof(json),
        "{\"mode\":\"%s\",\"experimental\":%s,\"tone_hz\":%d,\"baud\":%.2f,\"raw_bps\":%.2f,\"coded_payload_bps\":%.2f,"
        "\"rms\":%lu,\"clips\":%lu,\"sample_rate\":%lu,\"work_us\":%lu,\"budget_us\":%lu,"
        "\"peak_work_us\":%lu,\"overruns\":%lu,\"dma_overflows\":%lu,\"read_errors\":%lu,"
        "\"output_drops\":%lu,\"decode_drops\":%lu,\"lcd_drops\":%lu,\"lock_state\":%lu,\"received_bytes\":%" PRIu64 ","
        "\"internal_free\":%u,\"internal_min_free\":%u,\"session\":%lu,\"uptime_s\":%" PRId64 ",\"ssid\":\"%s\"}",
        psk ? info->name : modem_mode_name(s.mode), psk && psk_mode_is_experimental(s.psk_mode) ? "true" : "false",
        s.tone_hz, psk ? info->baud : 0, raw, net,
        (unsigned long)s.rms, (unsigned long)s.clips, (unsigned long)s.sample_rate,
        (unsigned long)s.work_us, (unsigned long)s.budget_us, (unsigned long)s.peak_work_us,
        (unsigned long)s.overruns, (unsigned long)s.dma_overflows, (unsigned long)s.read_errors,
        (unsigned long)s.output_drops, (unsigned long)s.decode_drops, (unsigned long)s.lcd_drops,
        (unsigned long)s.lock_state, total,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)s_session, esp_timer_get_time() / 1000000,
        BOARD_WIFI_SSID);
    if (n < 0 || n >= sizeof(json)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Status too large");
    no_cache(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n);
}

static esp_err_t modes_get(httpd_req_t *req)
{
    no_cache(req);
    httpd_resp_set_type(req, "application/json");
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, "[", 1), TAG, "modes");
    for (size_t i = 0; i < psk_mode_count(); ++i) {
        const psk_mode_info_t *m = psk_mode_info_at(i);
        char item[112];
        int n = snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"enabled\":%s,\"experimental\":%s}",
                         i ? "," : "", m->name, psk_mode_can_decode(m->id) ? "true" : "false",
                         psk_mode_is_experimental(m->id) ? "true" : "false");
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, item, n), TAG, "mode");
    }
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req,
        ", {\"name\":\"FT8\",\"enabled\":true},{\"name\":\"FT4\",\"enabled\":true},"
        "{\"name\":\"RTTY\",\"enabled\":true},{\"name\":\"CW\",\"enabled\":true},"
        "{\"name\":\"MFSK16\",\"enabled\":true}]", HTTPD_RESP_USE_STRLEN), TAG, "modes tail");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t control_post(httpd_req_t *req)
{
    /* Require this non-simple header: a web page on another origin cannot
     * silently reconfigure the receiver through a form POST. No CORS enabled. */
    char marker[12];
    if (httpd_req_get_hdr_value_str(req, "X-Modem-Control", marker, sizeof(marker)) != ESP_OK ||
        strcmp(marker, "1")) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Missing control header");
    char body[64];
    if (req->content_len == 0 || req->content_len >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid control length");
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    body[got] = 0;
    ui_event_t event = {.type = UI_EVENT_NONE};
    if (!strncmp(body, "mode=", 5)) {
        for (size_t i = 0; i < psk_mode_count(); ++i) {
            const psk_mode_info_t *m = psk_mode_info_at(i);
            if (!strcmp(body + 5, m->name) && psk_mode_can_decode(m->id)) {
                event.type = UI_EVENT_PSK_MODE_CHANGED;
                event.psk_mode = m->id;
                break;
            }
        }
        for (modem_mode_t m = MODEM_MODE_FT8; m < MODEM_MODE_COUNT; ++m) {
            if (!strcmp(body + 5, modem_mode_name(m))) {
                event.type = UI_EVENT_MODE_CHANGED;
                event.mode = m;
            }
        }
    } else if (!strncmp(body, "tone=", 5)) {
        uint64_t tone;
        if (parse_uint(body + 5, &tone) && tone >= 100 && tone <= 3500) {
            event.type = UI_EVENT_TONE_CHANGED;
            event.tone_hz = tone;
        }
    }
    if (event.type == UI_EVENT_NONE)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported mode or tone (100..3500 Hz)");
    if (xQueueSend(s_controls, &event, 0) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Control queue busy");
    }
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "queued");
}

esp_err_t web_rx_start(void)
{
    if (s_server) return ESP_OK;
    s_session = esp_random();
    s_status.mode = MODEM_MODE_PSK63;
    s_status.psk_mode = BOARD_DEFAULT_PSK_MODE;
    s_status.tone_hz = BOARD_DEFAULT_TONE_HZ;
    s_controls = xQueueCreate(4, sizeof(ui_event_t));
    if (!s_controls) return ESP_ERR_NO_MEM;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 0;
    cfg.task_priority = 3;
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 2;
    cfg.send_wait_timeout = 2;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        vQueueDelete(s_controls);
        s_controls = NULL;
        return err;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/rx", .method = HTTP_GET, .handler = rx_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/modes", .method = HTTP_GET, .handler = modes_get},
        {.uri = "/api/control", .method = HTTP_POST, .handler = control_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG, "register route");
    return ESP_OK;
}
