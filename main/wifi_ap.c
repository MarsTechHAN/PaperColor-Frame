// wifi_ap.c — Bring up AP/STA networking for the local PaperColor UI.

#include "wifi_ap.h"
#include "config_store.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi_ap";
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static uint32_t     s_ap_ip = 0;
static uint32_t     s_sta_ip = 0;
static volatile int s_ap_station_count = 0;
static volatile bool s_sta_connected = false;
static bool s_has_ap = false;
static bool s_has_sta = false;
static bool s_wifi_started = false;
static TaskHandle_t s_mdns_task = NULL;

static int dns_name_matches_papercolor(const uint8_t *packet, int len, int *question_end)
{
    int off = 12;
    const char *labels[] = {"papercolor", "local"};
    for (int li = 0; li < 2; li++) {
        if (off >= len) return 0;
        int lab_len = packet[off++];
        int want_len = (int)strlen(labels[li]);
        if (lab_len != want_len || off + lab_len > len) return 0;
        for (int i = 0; i < lab_len; i++) {
            char c = (char)packet[off + i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != labels[li][i]) return 0;
        }
        off += lab_len;
    }
    if (off >= len || packet[off++] != 0) return 0;
    if (off + 4 > len) return 0;
    uint16_t qtype = ((uint16_t)packet[off] << 8) | packet[off + 1];
    uint16_t qclass = ((uint16_t)packet[off + 2] << 8) | packet[off + 3];
    if (qtype != 1 && qtype != 255) return 0;
    if ((qclass & 0x7FFF) != 1) return 0;
    *question_end = off + 4;
    return 1;
}

static int build_mdns_response(const uint8_t *query, int qlen, uint8_t *resp, int resp_sz, uint32_t ip)
{
    int qend = 0;
    if (!dns_name_matches_papercolor(query, qlen, &qend)) return 0;
    int question_len = qend - 12;
    int need = 12 + question_len + 16;
    if (need > resp_sz) return 0;
    memset(resp, 0, need);
    resp[2] = 0x84; // response + authoritative answer
    resp[5] = 0x01; // one question
    resp[7] = 0x01; // one answer
    memcpy(resp + 12, query + 12, question_len);
    int off = 12 + question_len;
    resp[off++] = 0xC0; resp[off++] = 0x0C; // name pointer to question
    resp[off++] = 0x00; resp[off++] = 0x01; // A
    resp[off++] = 0x80; resp[off++] = 0x01; // IN + cache flush
    resp[off++] = 0x00; resp[off++] = 0x00;
    resp[off++] = 0x00; resp[off++] = 0x78; // 120s TTL
    resp[off++] = 0x00; resp[off++] = 0x04;
    memcpy(resp + off, &ip, 4);
    off += 4;
    return off;
}

static void mdns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "mDNS socket failed");
        s_mdns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(5353);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof bind_addr) != 0) {
        ESP_LOGW(TAG, "mDNS bind failed");
        close(sock);
        s_mdns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct ip_mreq mreq = {0};
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    mreq.imr_interface.s_addr = s_sta_ip;
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq);

    ESP_LOGI(TAG, "mDNS ready: http://papercolor.local/");
    uint8_t buf[512];
    uint8_t resp[128];
    for (;;) {
        struct sockaddr_in src = {0};
        socklen_t slen = sizeof src;
        int n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&src, &slen);
        if (n <= 0) continue;
        if (!s_sta_connected || s_sta_ip == 0) continue;
        int rn = build_mdns_response(buf, n, resp, sizeof resp, s_sta_ip);
        if (rn <= 0) continue;
        struct sockaddr_in dst = {0};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(5353);
        dst.sin_addr.s_addr = inet_addr("224.0.0.251");
        sendto(sock, resp, rn, 0, (struct sockaddr *)&dst, sizeof dst);
    }
}

static void start_mdns_once(void)
{
    if (s_mdns_task) return;
    BaseType_t ok = xTaskCreate(mdns_task, "paper_mdns", 4096, NULL,
                                tskIDLE_PRIORITY + 2, &s_mdns_task);
    if (ok != pdPASS) {
        s_mdns_task = NULL;
        ESP_LOGW(TAG, "mDNS task start failed");
    }
}

static void reconnect_sta(void)
{
    if (!s_has_sta) return;
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "STA reconnect failed to start: %s", esp_err_to_name(e));
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        if (s_ap_station_count < WIFI_AP_MAX_CONN) s_ap_station_count++;
        ESP_LOGI(TAG, "station joined: " MACSTR " aid=%d", MAC2STR(e->mac), e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        if (s_ap_station_count > 0) s_ap_station_count--;
        ESP_LOGI(TAG, "station left: " MACSTR " aid=%d", MAC2STR(e->mac), e->aid);
    } else if (id == WIFI_EVENT_STA_START) {
        reconnect_sta();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        s_sta_connected = false;
        s_sta_ip = 0;
        ESP_LOGW(TAG, "STA disconnected, reason=%d; reconnecting", e ? e->reason : -1);
        reconnect_sta();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    s_sta_ip = event->ip_info.ip.addr;
    s_sta_connected = true;
    ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    start_mdns_once();
}

static esp_err_t ensure_netif_event_loop(void)
{
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    return ESP_OK;
}

esp_netif_t *wifi_ap_start(void)
{
    if (s_wifi_started) return s_ap_netif;

    ESP_ERROR_CHECK(ensure_netif_event_loop());

    config_wifi_mode_t cfg_mode = config_get_wifi_mode();
    const char *sta_ssid = config_get_wifi_sta_ssid();
    bool want_ap = cfg_mode != CONFIG_WIFI_MODE_STA;
    bool want_sta = cfg_mode != CONFIG_WIFI_MODE_AP;
    if (want_sta && (!sta_ssid || !sta_ssid[0])) {
        ESP_LOGW(TAG, "STA mode requested without SSID; keeping AP available");
        want_sta = false;
        want_ap = true;
    }
    if (!want_ap && !want_sta) want_ap = true;

    s_has_ap = want_ap;
    s_has_sta = want_sta;

    if (want_ap) s_ap_netif = esp_netif_create_default_wifi_ap();
    if (want_sta) s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    if (want_sta) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));
    }

    wifi_mode_t wifi_mode = WIFI_MODE_AP;
    if (want_ap && want_sta) wifi_mode = WIFI_MODE_APSTA;
    else if (want_sta) wifi_mode = WIFI_MODE_STA;
    ESP_ERROR_CHECK(esp_wifi_set_mode(wifi_mode));

    if (want_ap) {
        const char *ssid = config_get_wifi_ap_ssid();
        const char *password = config_get_wifi_ap_password();
        bool has_password = password && password[0];
        wifi_config_t ap_cfg = {
            .ap = {
                .channel        = WIFI_AP_CHANNEL,
                .max_connection = WIFI_AP_MAX_CONN,
                .pmf_cfg        = { .required = false },
            },
        };
        snprintf((char *)ap_cfg.ap.ssid, sizeof ap_cfg.ap.ssid, "%s", ssid);
        ap_cfg.ap.ssid_len = strlen((const char *)ap_cfg.ap.ssid);
        if (has_password) {
            snprintf((char *)ap_cfg.ap.password, sizeof ap_cfg.ap.password, "%s", password);
            ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    }

    if (want_sta) {
        const char *password = config_get_wifi_sta_password();
        wifi_config_t sta_cfg = {0};
        snprintf((char *)sta_cfg.sta.ssid, sizeof sta_cfg.sta.ssid, "%s", sta_ssid);
        if (password && password[0]) {
            snprintf((char *)sta_cfg.sta.password, sizeof sta_cfg.sta.password, "%s", password);
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;

    if (want_ap) {
        esp_netif_ip_info_t ip = {0};
        esp_netif_get_ip_info(s_ap_netif, &ip);
        s_ap_ip = ip.ip.addr;
        ESP_LOGI(TAG, "SoftAP up: ssid=%s security=%s ip=" IPSTR " ch=%d",
                 config_get_wifi_ap_ssid(),
                 config_get_wifi_ap_password()[0] ? "wpa2" : "open",
                 IP2STR(&ip.ip), WIFI_AP_CHANNEL);
    }
    if (want_sta) {
        ESP_LOGI(TAG, "STA connecting to SSID '%s'; mDNS will be papercolor.local",
                 sta_ssid);
    }
    return s_ap_netif;
}

bool wifi_ap_has_ap(void)
{
    return s_has_ap;
}

bool wifi_ap_has_sta(void)
{
    return s_has_sta;
}

bool wifi_ap_sta_connected(void)
{
    return s_sta_connected;
}

uint32_t wifi_ap_get_ip(void)
{
    return s_ap_ip;
}

int wifi_ap_station_count(void)
{
    return s_ap_station_count + (s_sta_connected ? 1 : 0);
}
