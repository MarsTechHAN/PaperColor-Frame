# EPD OTP / LUT Read-Back Probe — Implementation Plan (Phase 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a gated, read-only firmware probe that powers the panel, runs the
existing init, then attempts to read the controller's LUT/register state over SPI
(two physical paths) and dumps the bytes to UART — so we can learn whether the
factory waveform is extractable on this board.

**Architecture:** A self-contained `epd_probe` module, compiled only when
`CONFIG_EPD_PROBE=y`. When enabled, `app_main` runs the probe instead of the
normal photo-frame stack. The probe powers both PMIC rails (EPD+SD coupling),
inits the bus+panel via `epd_init(false)`, then tries (A) a half-duplex read on
MISO/GPIO14 and (B) a bit-banged SIO read on the MOSI/GPIO13 data line, dumping
hex per command and printing a "data seen vs floating" verdict.

**Tech Stack:** ESP-IDF (`driver/spi_master`, `driver/gpio`, `esp_rom_gpio`),
ESP32-S3, UC81xx-family color EPD controller (relabeled).

**Testing approach (read this):** This module talks to real hardware over SPI;
there is no meaningful host unit test (the existing `host_test/` harness covers
pipeline math only). Verification per task is therefore: (1) `idf.py build`
compiles, and (2) flashing to the live board and reading `idf.py monitor` output.
Each on-device task states the exact serial lines to look for.

**Environment / how to build + flash:**
- Activate IDF once per shell: `. $HOME/esp/esp-idf/export.sh`
- Find the serial port: `ls /dev/cu.usb* 2>/dev/null` (e.g. `/dev/cu.usbmodem*` or `/dev/cu.wchusbserial*`).
- Build: `idf.py build`
- Flash + watch: `idf.py -p <PORT> flash monitor` (exit monitor with `Ctrl-]`).
- Enable the probe non-interactively: append `CONFIG_EPD_PROBE=y` to `sdkconfig`
  then build (IDF reconciles), or toggle it in `idf.py menuconfig` under
  "Paper E6 Experimental". Remove the line / set `n` to return to normal firmware.

---

### Task 1: Scaffold the gated probe (compiles to nothing when off)

**Files:**
- Create: `main/Kconfig.projbuild`
- Create: `main/epd_probe.h`
- Create: `main/epd_probe.c`
- Modify: `main/CMakeLists.txt` (add `epd_probe.c` to `SRCS`)
- Modify: `main/main.c` (branch into probe when `CONFIG_EPD_PROBE`)

- [ ] **Step 1: Add the Kconfig option (default n)**

Create `main/Kconfig.projbuild`:

```kconfig
menu "Paper E6 Experimental"

config EPD_PROBE
    bool "Enable read-only EPD controller probe (Phase 1 reverse engineering)"
    default n
    help
      When enabled, app_main runs a read-only SPI probe of the panel
      controller and dumps LUT/register reads over UART instead of starting
      the normal photo-frame stack. Bench reverse engineering only.
      Leave OFF for normal firmware.

endmenu
```

- [ ] **Step 2: Declare the probe API**

Create `main/epd_probe.h`:

```c
#ifndef PAPER_E6_EPD_PROBE_H
#define PAPER_E6_EPD_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

// Read-only Phase-1 probe. Self-contained: powers the PMIC rails, inits the
// SPI bus + panel, attempts controller read-back two ways, and dumps the
// results over UART. Never returns control to the normal app stack.
void epd_probe_run(void);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 3: Create the probe source as an empty translation unit when off**

Create `main/epd_probe.c` (body filled in later tasks; guarded so the default
build contains no probe code):

```c
// epd_probe.c — Phase 1 read-only controller probe. Active only when
// CONFIG_EPD_PROBE=y; otherwise this file compiles to nothing.
#include "sdkconfig.h"

#if CONFIG_EPD_PROBE

#include "epd_probe.h"
#include "esp_log.h"

static const char *TAG = "epd_probe";

void epd_probe_run(void)
{
    ESP_LOGW(TAG, "=== EPD PROBE (read-only) START ===");
    ESP_LOGW(TAG, "scaffold only — read paths added in later tasks");
    ESP_LOGW(TAG, "=== EPD PROBE DONE ===");
}

#endif // CONFIG_EPD_PROBE
```

- [ ] **Step 4: Register the source**

In `main/CMakeLists.txt`, add `"epd_probe.c"` to the `SRCS` list (right after
`"epd_4in0e.c"`):

```cmake
        "main.c"
        "epd_4in0e.c"
        "epd_probe.c"
        "sd_storage.c"
```

- [ ] **Step 5: Branch into the probe from app_main**

In `main/main.c`, add the include near the other module includes (after
`#include "power_manager.h"`):

```c
#include "power_manager.h"
#if CONFIG_EPD_PROBE
#include "epd_probe.h"
#endif
```

Then, immediately after the successful `pm1_init()` block (after the closing
`}` of the `if (pm1_init() != 0)` check, around line 86), insert:

```c
#if CONFIG_EPD_PROBE
    // Reverse-engineering probe: replaces the normal stack. Self-powers rails
    // and inits the bus/panel internally, so it must run before SD mount.
    epd_probe_run();
    return;
#endif
```

- [ ] **Step 6: Verify the default (probe OFF) build still compiles**

Run:
```bash
. $HOME/esp/esp-idf/export.sh && idf.py build
```
Expected: build succeeds. Because `CONFIG_EPD_PROBE` is unset, `epd_probe.c` is
an empty TU and `main.c` is unchanged in behavior.

- [ ] **Step 7: Verify the probe-ON build compiles and the branch fires**

Run:
```bash
grep -q '^CONFIG_EPD_PROBE=y' sdkconfig || echo 'CONFIG_EPD_PROBE=y' >> sdkconfig
idf.py build
```
Expected: build succeeds.

Flash + monitor (replace `<PORT>`):
```bash
idf.py -p <PORT> flash monitor
```
Expected serial output: the three `epd_probe` lines (`=== EPD PROBE ... START ===`,
`scaffold only ...`, `=== EPD PROBE DONE ===`), and the normal photo-frame logs
(`loop_display`, `wifi`, etc.) do **not** appear afterward.

- [ ] **Step 8: Commit**

```bash
git add main/Kconfig.projbuild main/epd_probe.h main/epd_probe.c main/CMakeLists.txt main/main.c
git commit -m "feat(probe): scaffold gated read-only EPD controller probe"
```

---

### Task 2: Panel bring-up + Attempt A (MISO half-duplex read)

**Files:**
- Modify: `main/epd_probe.c`

This task fills in the real probe body: power both rails (honoring the EPD/SD
rail coupling), init the bus+panel via `epd_init(false)`, then read a list of
candidate commands over the MISO line and dump the bytes.

- [ ] **Step 1: Replace the probe body with bring-up + MISO read**

Rewrite `main/epd_probe.c` (inside the existing `#if CONFIG_EPD_PROBE` guard):

```c
// epd_probe.c — Phase 1 read-only controller probe. Active only when
// CONFIG_EPD_PROBE=y; otherwise this file compiles to nothing.
#include "sdkconfig.h"

#if CONFIG_EPD_PROBE

#include "epd_probe.h"
#include "board_pins.h"
#include "epd_4in0e.h"
#include "pm1.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "epd_probe";

// Candidate read commands. 0x71 (Get Status) is the strongest "is the read
// path alive?" test on the UC81xx family; 0x20-0x25 are the LUT register
// addresses we hope to read back. These are hypotheses — the dump decides.
static const uint8_t k_cmds[]    = { 0x71, 0x70, 0x2E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25 };
// Short reads for status-like commands, longer for LUT-like commands.
static size_t read_len_for(uint8_t cmd) { return (cmd == 0x71 || cmd == 0x70) ? 4 : 64; }

static void hexdump(const char *via, uint8_t cmd, const uint8_t *buf, size_t n)
{
    printf("PROBE %s cmd=0x%02x [%u]:", via, cmd, (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        if ((i % 16) == 0) printf("\n  %04x: ", (unsigned)i);
        printf("%02x ", buf[i]);
    }
    printf("\n");
}

// "Alive" heuristic: not all identical, or a non-rail constant. All-0x00 or
// all-0xFF means the line was almost certainly floating/undriven.
static bool looks_alive(const uint8_t *b, size_t n)
{
    bool all_same = true;
    for (size_t i = 1; i < n; i++) if (b[i] != b[0]) { all_same = false; break; }
    if (!all_same) return true;
    return (b[0] != 0x00 && b[0] != 0xFF);
}

// ---- Attempt A: half-duplex read on MISO (GPIO14) -------------------------
// Command phase (DC=0) holds CS active into the read phase (DC=1) via
// CS_KEEP_ACTIVE + an explicit bus acquire, matching how multi-phase EPD
// reads expect CS to stay low across cmd+data.
static esp_err_t read_miso(uint8_t cmd, uint8_t *rx, size_t n)
{
    spi_device_handle_t dev = (spi_device_handle_t)epd_spi_handle();
    if (!dev) return ESP_ERR_INVALID_STATE;

    esp_err_t err = spi_device_acquire_bus(dev, portMAX_DELAY);
    if (err != ESP_OK) return err;

    spi_transaction_t tc = {
        .length     = 8,
        .tx_buffer  = &cmd,
        .user       = (void *)0,                 // DC=0 (command)
        .flags      = SPI_TRANS_CS_KEEP_ACTIVE,
    };
    err = spi_device_polling_transmit(dev, &tc);
    if (err == ESP_OK) {
        memset(rx, 0, n);
        spi_transaction_t tr = {
            .rxlength  = n * 8,
            .rx_buffer = rx,
            .user      = (void *)1,              // DC=1 (read data)
        };
        err = spi_device_polling_transmit(dev, &tr);
    }
    spi_device_release_bus(dev);
    return err;
}

void epd_probe_run(void)
{
    ESP_LOGW(TAG, "=== EPD PROBE (read-only) START ===");

    // Honor the EPD(PYG0)/SD(PYG3) rail coupling: both rails on before any
    // shared-bus traffic. Then init the bus + panel ourselves (false = not yet
    // inited), leaving REG=0 so the OTP waveform path is the active one.
    (void)pm1_set_sd_power(true);
    epd_prepare_power_off();
    if (pm1_set_epd_power(true) != 0) {
        ESP_LOGE(TAG, "EPD rail enable failed");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    if (epd_init(false) != 0) {
        ESP_LOGE(TAG, "epd_init failed");
        return;
    }
    ESP_LOGI(TAG, "panel inited (REG=0); beginning read attempts");

    uint8_t rx[64];

    ESP_LOGW(TAG, "--- Attempt A: MISO (GPIO%d) half-duplex read ---", EPD_PIN_MISO);
    bool a_alive = false;
    for (size_t i = 0; i < sizeof k_cmds; i++) {
        size_t n = read_len_for(k_cmds[i]);
        esp_err_t err = read_miso(k_cmds[i], rx, n);
        if (err == ESP_OK) {
            hexdump("MISO", k_cmds[i], rx, n);
            if (looks_alive(rx, n)) a_alive = true;
        } else {
            ESP_LOGE(TAG, "MISO read cmd 0x%02x failed: %s",
                     k_cmds[i], esp_err_to_name(err));
        }
    }
    ESP_LOGW(TAG, "Attempt A verdict: %s",
             a_alive ? "DATA SEEN (inspect dump)" : "looks floating/unsupported");

    ESP_LOGW(TAG, "=== EPD PROBE DONE ===");
}

#endif // CONFIG_EPD_PROBE
```

- [ ] **Step 2: Build**

Run:
```bash
. $HOME/esp/esp-idf/export.sh && idf.py build
```
Expected: build succeeds (probe still enabled from Task 1 Step 7).

- [ ] **Step 3: Flash, observe, and capture the dump**

Run (replace `<PORT>`):
```bash
idf.py -p <PORT> flash monitor | tee host_test/probe_dump_miso.log
```
Expected: `panel inited (REG=0)` then nine `PROBE MISO cmd=0x..` blocks, then an
`Attempt A verdict:` line. Read the verdict:
- "DATA SEEN" → a read path exists on MISO; inspect the bytes (esp. `cmd=0x71`).
- "looks floating/unsupported" → MISO is likely not driven; proceed to Task 3.

(Exit monitor with `Ctrl-]`.)

- [ ] **Step 4: Commit**

```bash
git add main/epd_probe.c
git commit -m "feat(probe): panel bring-up + MISO half-duplex read attempt"
```

---

### Task 3: Attempt B (bit-banged SIO read on the MOSI data line)

**Files:**
- Modify: `main/epd_probe.c`

For the common case where the panel has no separate SDO and instead drives the
read on the bidirectional MOSI/SDA line, bit-bang the read directly. This
detaches MOSI/SCLK/CS/DC from the SPI peripheral and drives them by hand. The
probe is terminal, so we never restore the peripheral routing.

- [ ] **Step 1: Add the bit-bang include**

In `main/epd_probe.c`, add to the include block (only `esp_rom_sys.h` is needed;
pins are reclaimed with `gpio_reset_pin()` from the already-included
`driver/gpio.h`):

```c
#include "esp_rom_sys.h"         // esp_rom_delay_us
```

- [ ] **Step 2: Add the bit-bang read implementation**

Insert before `epd_probe_run()`:

```c
// ---- Attempt B: bit-banged SIO read on the MOSI data line (GPIO13) --------
// SPI mode 0 (CPOL=0, CPHA=0): clock idles low, master samples on the rising
// edge. ~1us per half-period keeps us well inside the controller's read
// timing while staying fast enough for 64-byte dumps.
#define BB_DELAY_US 1

static void bb_begin(void)
{
    // Reclaim the pads from the SPI peripheral so gpio_set_level drives them.
    gpio_reset_pin(EPD_PIN_SCLK);
    gpio_reset_pin(EPD_PIN_MOSI);
    gpio_reset_pin(EPD_PIN_CS);
    gpio_reset_pin(EPD_PIN_DC);
    gpio_set_direction(EPD_PIN_SCLK, GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_PIN_CS,   GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_PIN_DC,   GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_PIN_MOSI, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(EPD_PIN_SCLK, 0);
    gpio_set_level(EPD_PIN_CS, 1);   // idle: CS high
}

static void bb_write_byte(uint8_t b)
{
    gpio_set_direction(EPD_PIN_MOSI, GPIO_MODE_OUTPUT);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(EPD_PIN_MOSI, (b >> i) & 1);
        gpio_set_level(EPD_PIN_SCLK, 1); esp_rom_delay_us(BB_DELAY_US);
        gpio_set_level(EPD_PIN_SCLK, 0); esp_rom_delay_us(BB_DELAY_US);
    }
}

static uint8_t bb_read_byte(void)
{
    uint8_t v = 0;
    gpio_set_direction(EPD_PIN_MOSI, GPIO_MODE_INPUT);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(EPD_PIN_SCLK, 1); esp_rom_delay_us(BB_DELAY_US);
        v = (uint8_t)((v << 1) | (gpio_get_level(EPD_PIN_MOSI) & 1));
        gpio_set_level(EPD_PIN_SCLK, 0); esp_rom_delay_us(BB_DELAY_US);
    }
    return v;
}

static void read_bitbang(uint8_t cmd, uint8_t *rx, size_t n)
{
    gpio_set_level(EPD_PIN_CS, 0);
    gpio_set_level(EPD_PIN_DC, 0);          // command phase
    bb_write_byte(cmd);
    gpio_set_level(EPD_PIN_DC, 1);          // read phase
    for (size_t i = 0; i < n; i++) rx[i] = bb_read_byte();
    gpio_set_level(EPD_PIN_CS, 1);
}
```

- [ ] **Step 3: Run Attempt B after Attempt A in `epd_probe_run()`**

Just before the final `ESP_LOGW(TAG, "=== EPD PROBE DONE ===");`, insert:

```c
    ESP_LOGW(TAG, "--- Attempt B: bit-bang SIO on MOSI (GPIO%d) ---", EPD_PIN_MOSI);
    bb_begin();   // NOTE: detaches MOSI/SCLK/CS/DC from the SPI peripheral; the
                  // SPI driver and SD are unusable after this point (probe is terminal).
    bool b_alive = false;
    for (size_t i = 0; i < sizeof k_cmds; i++) {
        size_t n = read_len_for(k_cmds[i]);
        read_bitbang(k_cmds[i], rx, n);
        hexdump("SIO", k_cmds[i], rx, n);
        if (looks_alive(rx, n)) b_alive = true;
    }
    ESP_LOGW(TAG, "Attempt B verdict: %s",
             b_alive ? "DATA SEEN (inspect dump)" : "looks floating/unsupported");
```

- [ ] **Step 4: Build**

Run:
```bash
. $HOME/esp/esp-idf/export.sh && idf.py build
```
Expected: build succeeds.

- [ ] **Step 5: Flash, observe, and capture the dump**

Run (replace `<PORT>`):
```bash
idf.py -p <PORT> flash monitor | tee host_test/probe_dump_full.log
```
Expected: the Attempt A blocks, then `--- Attempt B ...`, nine `PROBE SIO cmd=0x..`
blocks, and an `Attempt B verdict:` line. Compare the two attempts:
- If either attempt shows "DATA SEEN", inspect those bytes — that is the read
  path and the first glimpse of the controller's register/LUT contents.
- If both say "looks floating/unsupported", reads are not exposed on this board
  via either physical path; record that result for the Phase 1 conclusion.

- [ ] **Step 6: Commit**

```bash
git add main/epd_probe.c
git commit -m "feat(probe): bit-banged SIO read fallback on MOSI data line"
```

---

### Task 4: Record findings and restore normal firmware

**Files:**
- Create: `doc/probe_findings_phase1.md`
- Modify: `sdkconfig` (turn the probe back off)

- [ ] **Step 1: Write a short findings note**

Create `doc/probe_findings_phase1.md` capturing, in your own words from the
captured logs: which read path (if any) returned coherent data, what `cmd=0x71`
returned, what the `0x20`-`0x25` reads looked like, and the verdict for each
attempt. Paste the most informative hex blocks. If reads are unsupported, state
that plainly and note the candidate next steps (logic analyzer capture, or
revisiting whether a different command unlocks read mode).

- [ ] **Step 2: Disable the probe so the default build is normal firmware again**

Run:
```bash
sed -i '' '/^CONFIG_EPD_PROBE=y$/d' sdkconfig
. $HOME/esp/esp-idf/export.sh && idf.py build
```
Expected: build succeeds and is the normal photo-frame firmware (no probe branch).

- [ ] **Step 3: Commit**

```bash
git add doc/probe_findings_phase1.md
git commit -m "docs(probe): Phase 1 read-back findings"
```

---

## Notes for Phase 2 (NOT in this plan)

Phase 2 (writing a custom/shorter LUT via PSR `REG=1`) is a **separate design**
requiring per-step sign-off because of the single-panel brick risk. Do not start
it from this plan. The input to Phase 2 is the captured dump from Task 3/4.
