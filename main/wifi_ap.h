#ifndef PAPER_E6_WIFI_AP_H
#define PAPER_E6_WIFI_AP_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

// Captive-portal AP channel and capacity. SSID/password come from config_store.
#define WIFI_AP_CHANNEL  6
#define WIFI_AP_MAX_CONN 1

// Bring up the configured Wi-Fi mode (AP, STA, or STA+AP). Empty AP or STA
// passwords mean open networks. Returns the AP esp_netif handle when AP is
// active, otherwise NULL.
esp_netif_t *wifi_ap_start(void);

// Runtime interface state after wifi_ap_start().
bool         wifi_ap_has_ap(void);
bool         wifi_ap_has_sta(void);
bool         wifi_ap_sta_connected(void);

// AP gateway IP in network byte order (for the DNS hijack reply).
uint32_t     wifi_ap_get_ip(void);

// AP station count plus one while STA is connected. Existing power/display
// policy treats this as "someone can reach the web UI".
int          wifi_ap_station_count(void);

#ifdef __cplusplus
}
#endif
#endif
