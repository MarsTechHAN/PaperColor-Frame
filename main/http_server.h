#ifndef PAPER_E6_HTTP_SERVER_H
#define PAPER_E6_HTTP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

// Start the embedded HTTP server with the captive-portal + photo-management
// route table.  Idempotent.  Returns 0 on success.
int http_server_start(void);

#ifdef __cplusplus
}
#endif
#endif
