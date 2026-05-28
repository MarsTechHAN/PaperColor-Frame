"""Decode the E6 controller's native OTP waveform (read via ROTP 0x92).

Confirmed model (this is NOT a .wbf file and NOT the register LUT format — it is
the waveform the controller plays directly, as the user identified):

  * Each byte packs 4 consecutive frames, 2 bits per frame (inkwave packed_state
    layout). MSB-first: frame levels = [(b>>6)&3, (b>>4)&3, (b>>2)&3, b&3].
  * Level codes 0..3 are per-frame voltage selections. Drive pulses are levels
    1 and 2 (opposite polarities); 0 and 3 are idle/hold (GND/keep). The exact
    code->voltage map is calibrated on hardware.
  * A target colour's full waveform is ~1200 frames (~300 bytes). At the panel's
    ~82 Hz frame rate (PLL 0x30=0x08, ~12.1 ms/frame) that is 14.5 s — matching
    the measured baseline DRF->BUSY of 14530 ms.
  * The long sustained runs near the end (0x55=[1,1,1,1], 0xAA=[2,2,2,2],
    0xFF=[3,3,3,3]) are the final drive/settle phase, not a delimiter.

Open/uncalibrated: exact code->voltage values; how the 300-byte records map to
(target colour x temperature); the true total OTP size (no clean wrap seen in
the 98304 bytes dumped, so it is >= that).
"""

FRAMES_PER_BYTE = 4


def decode_frames(data, msb_first=True):
    """bytes -> list of per-frame 2-bit level codes (0..3)."""
    out = []
    for b in data:
        if msb_first:
            out += [(b >> 6) & 3, (b >> 4) & 3, (b >> 2) & 3, b & 3]
        else:
            out += [b & 3, (b >> 2) & 3, (b >> 4) & 3, (b >> 6) & 3]
    return out


def rle(seq):
    runs = []
    cur, n = seq[0], 1
    for v in seq[1:]:
        if v == cur:
            n += 1
        else:
            runs.append((cur, n)); cur, n = v, 1
    runs.append((cur, n))
    return runs


def segment_phases(frames, hold_levels=(0, 3), min_hold=6):
    """Rough split into (active vs hold) segments to expose reset/activation/
    drive/settle structure. Returns list of (kind, start, length)."""
    segs = []
    i = 0
    n = len(frames)
    while i < n:
        j = i
        if frames[i] in hold_levels:
            while j < n and frames[j] in hold_levels:
                j += 1
            kind = "hold" if (j - i) >= min_hold else "gap"
        else:
            while j < n and frames[j] not in hold_levels:
                j += 1
            kind = "drive"
        segs.append((kind, i, j - i))
        i = j
    return segs


def dc_balance(frames, volts):
    """Net signed impulse given a code->volts map (dict 0..3 -> float)."""
    return sum(volts[v] for v in frames)


if __name__ == "__main__":
    import sys
    from collections import Counter
    d = open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/otp_full.bin", "rb").read()
    REC = 300
    rec = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    frames = decode_frames(d[rec * REC:(rec + 1) * REC])
    print(f"record {rec}: {len(frames)} frames")
    print("level histogram:", dict(sorted(Counter(frames).items())))
    # provisional voltage map: 0=GND 1=-V 2=+V 3=GND/keep
    volts = {0: 0.0, 1: -1.0, 2: +1.0, 3: 0.0}
    print(f"DC impulse (1=-1,2=+1,0/3=0): {dc_balance(frames, volts):+.0f}")
    segs = segment_phases(frames)
    big = [s for s in segs if s[2] >= 8]
    print(f"{len(segs)} segments; long(>=8) segments (kind@start xlen):")
    print("  " + " ".join(f"{k}@{st}x{ln}" for k, st, ln in big[:40]))
