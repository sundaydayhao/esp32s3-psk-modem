#include "transport.h"
#include "board_config.h"
#include "web_rx.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "transport";

#define TRANSPORT_WIFI_UDP_PORT 7355

static transport_mode_t s_mode = TRANSPORT_MODE_USB_SERIAL;
static int s_netif_ready;
static int s_wifi_initialized;
static int s_wifi_ready;
static int s_udp_sock = -1;
static struct sockaddr_in s_udp_dest;
typedef struct {
    transport_mode_t mode;
    size_t len;
    uint8_t data[96];
} output_packet_t;
static QueueHandle_t s_output_queue;
static uint32_t s_output_drops;

uint32_t transport_output_drops(void)
{
    return s_output_drops; /* Read by the same audio task that increments it. */
}

static void output_task(void *arg)
{
    output_packet_t packet;
    while (true) {
        if (xQueueReceive(s_output_queue, &packet, portMAX_DELAY) != pdTRUE) continue;
        if (packet.mode == TRANSPORT_MODE_USB_SERIAL) {
            fwrite(packet.data, 1, packet.len, stdout);
            fflush(stdout);
        } else if (packet.mode == TRANSPORT_MODE_WIFI && s_udp_sock >= 0) {
            sendto(s_udp_sock, packet.data, packet.len, MSG_DONTWAIT,
                   (const struct sockaddr *)&s_udp_dest, sizeof(s_udp_dest));
        }
    }
}

const char *transport_mode_name(transport_mode_t mode)
{
    switch (mode) {
    case TRANSPORT_MODE_USB_SERIAL:
        return "USB";
    case TRANSPORT_MODE_WIFI:
        return "WiFi";
    case TRANSPORT_MODE_BT:
        return "BT";
    default:
        return "?";
    }
}

static esp_err_t ensure_wifi_started(void)
{
    if (s_wifi_ready) {
        return ESP_OK;
    }

    if (!s_netif_ready) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
            err = nvs_flash_init();
        }
        ESP_RETURN_ON_ERROR(err, TAG, "init nvs");
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
        err = esp_event_loop_create_default();
        if (err != ESP_ERR_INVALID_STATE) ESP_RETURN_ON_ERROR(err, TAG, "event loop");
        if (!esp_netif_create_default_wifi_ap()) return ESP_ERR_NO_MEM;
        s_netif_ready = 1;
    }

    if (!s_wifi_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "init wifi");

        wifi_config_t ap_config = {0};
        strlcpy((char *)ap_config.ap.ssid, BOARD_WIFI_SSID, sizeof(ap_config.ap.ssid));
        strlcpy((char *)ap_config.ap.password, BOARD_WIFI_PASSWORD, sizeof(ap_config.ap.password));
        ap_config.ap.ssid_len = strlen(BOARD_WIFI_SSID);
        ap_config.ap.channel = BOARD_WIFI_CHANNEL;
        ap_config.ap.max_connection = BOARD_WIFI_MAX_CLIENTS;
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set ap mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "set ap config");
        s_wifi_initialized = 1;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi");

    if (s_udp_sock < 0) {
        s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (s_udp_sock < 0) {
            return ESP_FAIL;
        }
        int broadcast = 1;
        setsockopt(s_udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    }
    memset(&s_udp_dest, 0, sizeof(s_udp_dest));
    s_udp_dest.sin_family = AF_INET;
    s_udp_dest.sin_port = htons(TRANSPORT_WIFI_UDP_PORT);
    s_udp_dest.sin_addr.s_addr = inet_addr("255.255.255.255");

    s_wifi_ready = 1;
    ESP_LOGI(TAG, "WiFi AP %s ready: http://192.168.4.1/ (UDP %d)", BOARD_WIFI_SSID, TRANSPORT_WIFI_UDP_PORT);
    return ESP_OK;
}

esp_err_t transport_init(transport_mode_t mode)
{
    ESP_RETURN_ON_ERROR(ensure_wifi_started(), TAG, "always-on AP");
    ESP_RETURN_ON_ERROR(web_rx_start(), TAG, "phone receiver");
    s_output_queue = xQueueCreate(16, sizeof(output_packet_t));
    if (!s_output_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(output_task, "psk_output", 3072, NULL, 2, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    return transport_set_mode(mode);
}

esp_err_t transport_set_mode(transport_mode_t mode)
{
    if ((unsigned)mode >= TRANSPORT_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mode == TRANSPORT_MODE_BT) {
        ESP_LOGW(TAG, "BT transport selected, but BLE serial backend is not configured");
    }

    s_mode = mode;
    ESP_LOGI(TAG, "transport=%s", transport_mode_name(mode));
    return ESP_OK;
}

transport_mode_t transport_get_mode(void)
{
    return s_mode;
}

void transport_send(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return;
    }

    /* The audio task never waits for a phone, USB host or network socket. */
    web_rx_publish(data, len);
    while (len && s_mode != TRANSPORT_MODE_BT) {
        output_packet_t packet = {.mode = s_mode};
        packet.len = len > sizeof(packet.data) ? sizeof(packet.data) : len;
        memcpy(packet.data, data, packet.len);
        if (xQueueSend(s_output_queue, &packet, 0) != pdTRUE) s_output_drops += packet.len;
        data += packet.len;
        len -= packet.len;
    }
}
