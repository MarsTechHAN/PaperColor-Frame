// dns_hijack.c — minimal DNS A/AAAA server.  Every query is answered with
// our AP IP (A records) or NODATA (AAAA / others), so iOS Captive Network
// Assistant and Android's connectivity-check land on us.
//
// Protocol cheat-sheet (RFC 1035 §4.1):
//   id(2) flags(2) qd(2) an(2) ns(2) ar(2)  <— 12-byte header
//   QName  : labels separated by length-prefixes, terminated by 0x00
//   QType  : 2 bytes (A=1, AAAA=28)
//   QClass : 2 bytes (IN=1)
//
// Response we emit (only when QType=A, QClass=IN):
//   <copied query as-is, flags |= 0x8180>
//   <RR: name pointer 0xC00C, type 1, class 1, TTL 60, rdlen 4, IP>
//
// For other QTypes we still set QR=1 + answer=0 (NODATA) so the client gives
// up gracefully rather than retrying forever.

#include "dns_hijack.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "dns_hijack";

#define DNS_PORT          53
#define DNS_BUF           512
#define DNS_QTYPE_A       1
#define DNS_QCLASS_IN     1

typedef struct {
    int      sock;
    uint32_t ap_ip;
} dns_ctx_t;

static int parse_qname(const uint8_t *pkt, int pkt_len, int off, int *out_end)
{
    while (off < pkt_len) {
        uint8_t l = pkt[off];
        if (l == 0) { *out_end = off + 1; return 0; }
        if ((l & 0xC0) == 0xC0) {     // compression pointer (shouldn't happen in a query)
            *out_end = off + 2;
            return 0;
        }
        off += l + 1;
    }
    return -1;
}

static void dns_task(void *arg)
{
    dns_ctx_t *ctx = (dns_ctx_t *)arg;
    uint8_t buf[DNS_BUF];
    uint8_t reply[DNS_BUF];

    for (;;) {
        struct sockaddr_in src;
        socklen_t srclen = sizeof src;
        int n = recvfrom(ctx->sock, buf, sizeof buf, 0,
                         (struct sockaddr *)&src, &srclen);
        if (n < 12) continue;

        uint16_t id      = (buf[0] << 8) | buf[1];
        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if (qdcount == 0) continue;

        int qname_end;
        if (parse_qname(buf, n, 12, &qname_end) != 0) continue;
        if (qname_end + 4 > n) continue;
        uint16_t qtype  = (buf[qname_end] << 8)     | buf[qname_end+1];
        uint16_t qclass = (buf[qname_end+2] << 8)   | buf[qname_end+3];

        // Build the response.
        int qsection_len = qname_end + 4 - 12;
        // Bound the response: 12-byte header + copied question + 16-byte A RR
        // must fit reply[]. A crafted long-QNAME query (up to n=512) would
        // otherwise overflow this stack buffer. Drop anything that won't fit.
        if (12 + qsection_len + 16 > (int)sizeof reply) continue;
        int reply_len = 0;
        // header
        reply[reply_len++] = id >> 8;       reply[reply_len++] = id & 0xff;
        reply[reply_len++] = 0x81;          reply[reply_len++] = 0x80;  // QR=1, RD=1, RA=1
        reply[reply_len++] = 0; reply[reply_len++] = 1;                  // QDCOUNT=1
        bool answer = (qtype == DNS_QTYPE_A && qclass == DNS_QCLASS_IN);
        reply[reply_len++] = 0; reply[reply_len++] = answer ? 1 : 0;     // ANCOUNT
        reply[reply_len++] = 0; reply[reply_len++] = 0;                  // NSCOUNT
        reply[reply_len++] = 0; reply[reply_len++] = 0;                  // ARCOUNT
        // copy original question section
        memcpy(reply + reply_len, buf + 12, qsection_len);
        reply_len += qsection_len;

        if (answer) {
            // RR: name pointer 0xC00C → "the QNAME at offset 12"
            reply[reply_len++] = 0xc0; reply[reply_len++] = 0x0c;
            reply[reply_len++] = 0; reply[reply_len++] = 1;              // TYPE=A
            reply[reply_len++] = 0; reply[reply_len++] = 1;              // CLASS=IN
            reply[reply_len++] = 0; reply[reply_len++] = 0;              // TTL
            reply[reply_len++] = 0; reply[reply_len++] = 60;
            reply[reply_len++] = 0; reply[reply_len++] = 4;              // RDLENGTH
            uint32_t ip = ctx->ap_ip;
            reply[reply_len++] = (ip >>  0) & 0xff;
            reply[reply_len++] = (ip >>  8) & 0xff;
            reply[reply_len++] = (ip >> 16) & 0xff;
            reply[reply_len++] = (ip >> 24) & 0xff;
        }

        sendto(ctx->sock, reply, reply_len, 0,
               (struct sockaddr *)&src, srclen);
    }
}

int dns_hijack_start(uint32_t ap_ip)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGE(TAG, "socket: %d", errno);
        return -1;
    }
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(DNS_PORT),
    };
    if (bind(s, (struct sockaddr *)&bind_addr, sizeof bind_addr) != 0) {
        ESP_LOGE(TAG, "bind :53: %d", errno);
        close(s);
        return -1;
    }

    dns_ctx_t *ctx = calloc(1, sizeof *ctx);
    ctx->sock  = s;
    ctx->ap_ip = ap_ip;

    BaseType_t ok = xTaskCreate(dns_task, "dns_hijack", 4096, ctx,
                                tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        close(s);
        free(ctx);
        return -1;
    }
    ESP_LOGI(TAG, "DNS hijack up on :53, answering A → %u.%u.%u.%u",
             (unsigned)((ap_ip      ) & 0xff),
             (unsigned)((ap_ip >>  8) & 0xff),
             (unsigned)((ap_ip >> 16) & 0xff),
             (unsigned)((ap_ip >> 24) & 0xff));
    return 0;
}
