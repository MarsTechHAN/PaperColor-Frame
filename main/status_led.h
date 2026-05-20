#ifndef PAPER_E6_STATUS_LED_H
#define PAPER_E6_STATUS_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the ESP32 RMT channel used for the two top NeoPixels.
int status_led_init(void);

// Set both NeoPixels to one colour.  Brightness is a 0..255 scalar applied to
// the requested colour; brightness 0 or RGB 0 powers RGB_EN back down.
int status_led_show(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);

// Clear the LEDs and disable their PMIC RGB_EN rail.
void status_led_off(void);

#ifdef __cplusplus
}
#endif
#endif
