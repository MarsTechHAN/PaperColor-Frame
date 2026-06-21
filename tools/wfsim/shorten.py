"""Derive a SHORT, DC-balanced register-format waveform from a decoded OTP record.

Approach (keeps it "based on the given waveform", not a guess):
  1. decode an OTP record -> 1200 per-frame 2-bit levels
  2. subsample by S (preserves overall shape, shortens time, reduces run count)
  3. RLE -> phases (level, frames); translate 2-bit -> register 3-bit level codes
  4. pack into the UC8159c register colour-LUT format (<=20 states x 8 phases)
  5. FORCE exact DC balance by appending a correction phase so sum(V*frames)=0
  6. emit a C header for the firmware to write via 0x20-0x29 (REG mode)

Provisional 2-bit -> voltage (calibrate on HW): 0,3 = idle(GND); 1 = VSL(-);
2 = VSH(+). Level 3 is the biggest unknown (most common) -> treated as idle for
now; the DC-balance correction makes the net safe regardless.
"""
import sys
from otp_decode import decode_frames, rle

# register 3-bit level codes
GND, VSH, VSL, VSH_LV, VSL_LV = 0b000, 0b001, 0b010, 0b011, 0b100
VOLTS = {GND: 0.0, VSH: +15.0, VSL: -15.0, VSH_LV: +5.0, VSL_LV: -5.0}
MAP2TO3 = {0: GND, 1: VSL, 2: VSH, 3: GND}   # provisional

NUM_STATES, NUM_PHASES = 20, 8
MAX_PHASES = NUM_STATES * NUM_PHASES


def shorten(frames, S):
    sub = frames[::S]
    runs = rle(sub)                              # (level2, count)
    phases = [(MAP2TO3[lv], cnt) for lv, cnt in runs]
    return phases


def dc_impulse(phases):
    return sum(VOLTS[lv] * n for lv, n in phases)


def balance(phases):
    """Append a single correction phase (minority polarity) to zero the impulse."""
    imp = dc_impulse(phases)
    if abs(imp) < 1e-6:
        return phases
    # correct with VSH or VSL at 15 V
    if imp > 0:
        n = round(imp / 15.0); lv = VSL
    else:
        n = round(-imp / 15.0); lv = VSH
    if n > 0:
        phases = phases + [(lv, n)]
    return phases


def pack_color_lut(phases):
    """phases -> 260-byte colour LUT (20 states x 13 bytes)."""
    assert len(phases) <= MAX_PHASES, f"{len(phases)} phases > {MAX_PHASES}"
    out = bytearray()
    for s in range(NUM_STATES):
        chunk = phases[s * NUM_PHASES:(s + 1) * NUM_PHASES]
        levels = [lv for lv, _ in chunk] + [GND] * (NUM_PHASES - len(chunk))
        fr = [n for _, n in chunk] + [0] * (NUM_PHASES - len(chunk))
        repeat = 1 if any(fr) else 0
        out.append(repeat)
        for k in range(0, 8, 2):                 # 3-bit levels, 2 phases/byte
            out.append(((levels[k] & 7) << 4) | (levels[k + 1] & 7))
        out += bytes(min(f, 255) for f in fr)
    assert len(out) == 260
    return bytes(out)


def pack_vcom_lut(phases):
    """VCOM LUT mirroring the phase frame structure at VCM_DC (2-bit code 0b10)."""
    out = bytearray()
    for s in range(NUM_STATES):
        chunk = phases[s * NUM_PHASES:(s + 1) * NUM_PHASES]
        fr = [n for _, n in chunk] + [0] * (NUM_PHASES - len(chunk))
        repeat = 1 if any(fr) else 0
        out.append(repeat)
        out += bytes([0x00, 0x00])               # all phases = 0b00 = VCM_DC
        out += bytes(min(f, 255) for f in fr)
    assert len(out) == 220
    return bytes(out)


if __name__ == "__main__":
    d = open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/otp_full.bin", "rb").read()
    rec = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    S = int(sys.argv[3]) if len(sys.argv) > 3 else 12

    frames = decode_frames(d[rec * 300:(rec + 1) * 300])
    phases = shorten(frames, S)
    print(f"record {rec}, subsample S={S}: {len(frames)} frames -> {len(phases)} phases")
    if len(phases) > MAX_PHASES:
        print(f"  TOO MANY phases ({len(phases)} > {MAX_PHASES}); increase S")
        sys.exit(1)
    print(f"  DC impulse before balance: {dc_impulse(phases):+.0f}")
    phases = balance(phases)
    tot = sum(n for _, n in phases)
    print(f"  after balance: {len(phases)} phases, DC={dc_impulse(phases):+.0f}, "
          f"total {tot} frames (~{tot*12.1/1000:.1f}s @82Hz)")
    cl = pack_color_lut(phases)
    vc = pack_vcom_lut(phases)
    print(f"  colour LUT {len(cl)}B, VCOM LUT {len(vc)}B")
    # emit C header
    def carr(name, b):
        s = f"static const uint8_t {name}[{len(b)}] = {{\n"
        for i in range(0, len(b), 16):
            s += "  " + ",".join(f"0x{x:02x}" for x in b[i:i+16]) + ",\n"
        return s + "};\n"
    with open("/tmp/short_wf.h", "w") as f:
        f.write(f"// short waveform from OTP record {rec}, S={S}, {tot} frames\n")
        f.write(carr("SHORT_VCOM_LUT", vc))
        f.write(carr("SHORT_COLOR_LUT", cl))
    print("  wrote /tmp/short_wf.h")
