#ifndef PAPER_E6_WELCOME_OVERLAY_H
#define PAPER_E6_WELCOME_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Paint the per-device Wi-Fi SSID and password into the empty underline strips
// reserved in the welcome card. The SSID underline is identical across zh/en
// art, but the "Password" / "密码" label widths differ, so the password slot
// rectangle is language-dependent. `lang` is 0 for zh, 1 for en.
//
// Coordinates are absolute pixel offsets into the 400x600 panel framebuffer
// (4bpp packed, EPD ink codes). See welcome_overlay.c for the per-language
// slot rectangles.
//
// `ssid` and `password` are NUL-terminated ASCII; characters outside the
// supported 5x7 glyph set render as blanks.
void welcome_overlay_draw(uint8_t *fb,
                          int lang,
                          const char *ssid,
                          const char *password);

#ifdef __cplusplus
}
#endif
#endif
