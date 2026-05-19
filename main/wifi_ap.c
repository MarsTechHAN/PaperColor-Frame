// wifi_ap.c — Bring up an open SoftAP for the captive-portal UI.  Pure
// offline; no STA, no NAT, no upstream Internet.

#include "wifi_ap.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi_ap";
static esp_netif_t *s_ap_netif = NULL;
static uint32_t     s_ap_ip    = 0;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "station joined: " MACSTR " aid=%d", MAC2STR(e->mac), e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "station left: "  MACSTR " aid=%d", MAC2STR(e->mac), e->aid);
    }
}

esp_netif_t *wifi_ap_start(void)
{
    if (s_ap_netif) return s_ap_netif;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .channel        = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,
            .pmf_cfg        = { .required = false },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Read the AP-side IP that esp_netif assigned (default 192.168.4.1).
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(s_ap_netif, &ip);
    s_ap_ip = ip.ip.addr;
    ESP_LOGI(TAG, "SoftAP up: ssid=%s ip=" IPSTR " ch=%d",
             WIFI_AP_SSID, IP2STR(&ip.ip), WIFI_AP_CHANNEL);
    return s_ap_netif;
}

uint32_t wifi_ap_get_ip(void)
{
    return s_ap_ip;
}
