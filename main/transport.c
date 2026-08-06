#include "transport.h"

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

static const char *TAG = "transport";

#define TRANSPORT_WIFI_SSID "PSK-MODEM"
#define TRANSPORT_WIFI_CHANNEL 6
#define TRANSPORT_WIFI_UDP_PORT 7355

static transport_mode_t s_mode = TRANSPORT_MODE_USB_SERIAL;
static int s_netif_ready;
static int s_wifi_initialized;
static int s_wifi_ready;
static int s_udp_sock = -1;
static struct sockaddr_in s_udp_dest;

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
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_loop_create_default());
        esp_netif_create_default_wifi_ap();
        s_netif_ready = 1;
    }

    if (!s_wifi_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "init wifi");

        wifi_config_t ap_config = {0};
        strlcpy((char *)ap_config.ap.ssid, TRANSPORT_WIFI_SSID, sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen(TRANSPORT_WIFI_SSID);
        ap_config.ap.channel = TRANSPORT_WIFI_CHANNEL;
        ap_config.ap.max_connection = 1;
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
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
    ESP_LOGI(TAG, "WiFi AP %s started, UDP broadcast port %d", TRANSPORT_WIFI_SSID, TRANSPORT_WIFI_UDP_PORT);
    return ESP_OK;
}

esp_err_t transport_init(transport_mode_t mode)
{
    return transport_set_mode(mode);
}

esp_err_t transport_set_mode(transport_mode_t mode)
{
    if (mode >= TRANSPORT_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mode == TRANSPORT_MODE_WIFI) {
        ESP_RETURN_ON_ERROR(ensure_wifi_started(), TAG, "enable wifi transport");
    } else if (s_wifi_ready) {
        esp_wifi_stop();
        s_wifi_ready = 0;
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

    switch (s_mode) {
    case TRANSPORT_MODE_USB_SERIAL:
        fwrite(data, 1, len, stdout);
        fflush(stdout);
        break;
    case TRANSPORT_MODE_WIFI:
        if (s_udp_sock >= 0) {
            sendto(s_udp_sock, data, len, 0, (const struct sockaddr *)&s_udp_dest, sizeof(s_udp_dest));
        }
        break;
    case TRANSPORT_MODE_BT:
    default:
        break;
    }
}
