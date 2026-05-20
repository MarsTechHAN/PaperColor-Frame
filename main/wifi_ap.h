#ifndef PAPER_E6_WIFI_AP_H
#define PAPER_E6_WIFI_AP_H

#include <stdint.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

// Captive-portal AP channel and capacity. SSID/password come from config_store.
#define WIFI_AP_CHANNEL  6
#define WIFI_AP_MAX_CONN 1

// Bring up SoftAP on a fixed channel. Empty configured password means open AP.
// Returns the AP esp_netif handle so other modules can query the AP IP.
esp_netif_t *wifi_ap_start(void);

// AP gateway IP in network byte order (for the DNS hijack reply).
uint32_t     wifi_ap_get_ip(void);

// Number of currently associated stations.  The AP is configured for one.
int          wifi_ap_station_count(void);

#ifdef __cplusplus
}
#endif
#endif
