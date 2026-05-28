# EPD OTP / LUT Read-Back Probe — Design (Phase 1)

Date: 2026-05-28
Status: Approved for implementation
Scope: Phase 1 only (read-only exploration). Phase 2 (custom LUT / fast refresh) is a separate design.

## Goal

Determine whether the panel's controller can be **read** over SPI on the M5Stack
PaperColor board, and if so, **dump its factory waveform / LUT and register
state** to the serial console for offline analysis. The dump is the input to a
later phase that designs a shorter ("fast refresh") custom waveform.

Success = we can answer, with evidence on the bench:
1. Is there a working SPI read path to this controller? (yes / no, and which one)
2. If yes, what do the LUT / OTP bytes look like? (captured to a log file)

## Background

- **Board:** M5Stack PaperColor (C151), ESP32-S3.
- **Panel:** ED2208-DOA / EL040EF1, 400×600, E-Ink Spectra 6 (E6).
- **Controller:** printed part number is unreliable (E-Ink ecosystem parts are
  commonly relabeled). The command set in `main/epd_4in0e.c` matches the
  **UltraChip UC81xx / EK79xx family** (UC8159 / UC8179 / UC8151), which we use
  as the documented "Rosetta Stone":
  - `0x00` PSR (Panel Setting) — **bit5 `REG`**: 0 = waveform from OTP (current),
    1 = waveform from registers. Current value `{5F,69}` → REG=0.
  - `0x01` PWR, `0x02` POF, `0x03` PFS, `0x04` PON, `0x05/06/08` BTST,
    `0x07` DSLP, `0x10` DTM, `0x12` DRF, `0x30` PLL, `0x50` CDI, `0x60` TCON,
    `0x61` TRES (0190×0258 = 400×600), `0xAA` CMDH handshake.
  - In this family, register-mode LUTs load via `0x20` (LUTC/VCOM) and
    `0x21`–`0x2x` (per-transition phase tables). The 6-color LUT layout is **not
    publicly documented** — that is exactly what we want to recover.
- **Power coupling:** EPD (PYG0) and SD (PYG3) rails are gated by the M5PM1 PMIC
  and must both be on; the probe reuses the normal boot/PMIC/`epd_init()` path so
  this is already handled. SD CS is held deasserted during EPD reads.
- The current driver is **write-only** (`SPI_DEVICE_HALFDUPLEX`, no read
  transactions). A read path is therefore unproven and is the first unknown.

## Constraints (from the user)

- **Single panel.** No behavior-changing writes, no REG mode, in Phase 1.
- **No parameter sweeps** (no DRF/PLL/CDI scanning).
- **OTP dump first**, then study the dumped waveform for further leads.

## Design

### Module & gating

- New files: `main/epd_probe.c`, `main/epd_probe.h`.
- Gated by a compile-time Kconfig option `CONFIG_EPD_PROBE` (default **n**).
  When `n`, the binary is identical to current firmware. When `y`, boot runs the
  normal PMIC → SPI → `epd_init()` sequence (so the panel is powered and OTP is
  loaded into the controller's working LUT RAM), then branches into the probe
  instead of the photo display loop.
- This follows the project convention: new/experimental features default off
  until signed off.

### Read primitive (tried in order, both logged)

1. **Half-duplex read on MISO (GPIO14).** Send command byte with DC=0, then a
   half-duplex read transaction with `rxlength` on the existing EPD device
   handle. SD CS held high throughout.
2. **Bit-banged SIO read on the data line (GPIO13).** Send command, switch the
   data pin to input, manually clock SCLK and sample — covers the common case
   where the panel SDA is bidirectional with no separate SDO. The SPI peripheral
   is quiesced and the relevant GPIOs are driven manually for this path.

Each attempt dumps raw hex so we can see which (if either) yields coherent,
repeatable data vs. floating/noisy lines.

### Read sequence

1. Run `epd_init()` (REG stays 0 → OTP waveform loaded into LUT RAM). Do **not**
   trigger a full display refresh.
2. Issue candidate read commands and dump the returned bytes, labeled:
   - any chip status / revision read the family exposes,
   - the LUT register addresses (`0x20`–`0x2x`) in read direction,
   - a bulk read to capture the full LUT RAM region.
3. Emit a labeled hex dump over UART; capture via `idf.py monitor` to a log file.

### Output / analysis

- Structured per-command hex over UART, captured to a file under `host_test/` or
  `doc/` for offline analysis.
- Offline, identify phase tables, frame counts, and per-color sequences. That
  analysis is the input to the Phase 2 design (shorter custom LUT).

## Safety

- **Read-only.** No REG=1, no custom LUT writes, no abnormal-voltage drive.
- The only panel activity is the same init already run every boot.
- The probe must **fail fast and loud**: if neither read method returns coherent
  data, it logs an explicit "reads appear floating / unsupported" verdict so we
  learn the answer on the first flash without further risk.

## Out of scope (Phase 2, separate design + per-step sign-off)

- Setting PSR `REG=1` and loading a custom/guessed LUT.
- Any write that changes refresh voltages, phase counts, or timing.
- Designing and validating a faster waveform.

## Success criteria

- Probe builds behind `CONFIG_EPD_PROBE` without affecting the default build.
- On the live board, the probe produces a clear verdict on read feasibility.
- If reads work, a captured log file containing the controller's LUT/register
  bytes exists for analysis.
