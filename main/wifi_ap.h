#ifndef PAPER_E6_WIFI_AP_H
#define PAPER_E6_WIFI_AP_H

#include <stdint.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

// SSID broadcast by the captive-portal AP.
#define WIFI_AP_SSID     "PaperE6"
#define WIFI_AP_CHANNEL  6
#define WIFI_AP_MAX_CONN 4

// Bring up SoftAP with no password (open) on a fixed channel.  Returns the
// AP esp_netif handle so other modules (DNS hijack) can query the AP IP.
esp_netif_t *wifi_ap_start(void);

// AP gateway IP in network byte order (for the DNS hijack reply).
uint32_t     wifi_ap_get_ip(void);

#ifdef __cplusplus
}
#endif
#endif
