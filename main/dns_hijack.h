#ifndef PAPER_E6_DNS_HIJACK_H
#define PAPER_E6_DNS_HIJACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start a tiny DNS server on UDP/53 that resolves every A-record query to
// `ap_ip` (network byte order, same as wifi_ap_get_ip()).  This is what
// triggers iOS / Android captive-portal detection.
int dns_hijack_start(uint32_t ap_ip);

#ifdef __cplusplus
}
#endif
#endif
