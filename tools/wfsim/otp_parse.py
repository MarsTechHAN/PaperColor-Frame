"""Complete structural parse of the E6 panel OTP dump (otp_el040ef1_60000.bin,
the 60000-byte true period of the EL040EF1 / Spectra-6 controller OTP).

Layout discovered (zero-assumption, 2026-05-28):

  [0x0000 .. 0x752F]  HALF 1  (30000 B) : 2-bit/frame WAVEFORM LIBRARY
  [0x7530 .. 0xEA5F]  HALF 2  (30000 B) : high-entropy per-entry data (opaque)

Half 1 is a sequence of refresh waveforms. Each waveform slot =
    [drive content: 0/100/200 B] [16 B zero pad] [84 B activation block]
A *full* slot is 300 B = 1200 frames; at the panel's ~82 Hz (PLL=0x08,
~12.1 ms/frame) that is 14.6 s = the measured single OTP refresh time.

Frame encoding: 2 bits/frame, MSB-first, 4 frames/byte
    level 0,3 = holds / one drive rail   level 1 = -V drive   level 2 = +V drive
The 84-byte activation block is byte-identical in every slot:
    L0x3  -Vx67  +Vx66  L3x67  -Vx67  +Vx66   (336 frames, DC-balanced).

Usage:
    python3 otp_parse.py [path] [--frames N]   # also decode slot N to frames
"""
import sys
import numpy as np

args = [x for x in sys.argv[1:] if not x.startswith("--")]
path = args[0] if args else "../../../WAVEFORM/otp_el040ef1_60000.bin"
dump_slot = None
if "--frames" in sys.argv:
    dump_slot = int(sys.argv[sys.argv.index("--frames") + 1])

a = np.frombuffer(open(path, "rb").read(), dtype=np.uint8)
N = len(a)
HALF = N // 2
H1, H2 = a[:HALF], a[HALF:]
DELIM_SET = {0x01, 0x55, 0x5a, 0xaa, 0xff, 0xfd}


def entropy(b):
    c = np.bincount(b, minlength=256).astype(float)
    p = c[c > 0] / len(b)
    return float(-(p * np.log2(p)).sum())


def frames(buf):
    if isinstance(buf, (bytes, bytearray)):
        buf = np.frombuffer(buf, np.uint8)
    buf = np.asarray(buf, np.uint8)
    f = np.empty(len(buf) * 4, np.uint8)
    for s in range(4):
        f[s::4] = (buf >> (6 - 2 * s)) & 3
    return f


def rle(levels, maxn=40):
    out, i = [], 0
    levels = list(levels)
    while i < len(levels) and len(out) < maxn:
        j = i
        while j < len(levels) and levels[j] == levels[i]:
            j += 1
        out.append(f"{levels[i]}x{j - i}")
        i = j
    return " ".join(out) + (" …" if i < len(levels) else "")


print(f"file={path}  total={N} B")
print(f"HALF1 [0..{HALF}) ent={entropy(H1):.2f}   HALF2 [{HALF}..{N}) ent={entropy(H2):.2f}")

# --- segment half 1 on the activation block ----------------------------------
anchors = [i for i in range(1, len(H1) - 4)
           if H1[i] == 1 and H1[i+1] == 0x55 and H1[i+2] == 0x55
           and H1[i+3] == 0x55 and H1[i-1] == 0]
blocks = [bytes(H1[s:s+84]) for s in anchors]
print(f"\nHALF1: {len(anchors)} activation blocks, "
      f"{len(set(blocks))} distinct -> "
      f"{'FIXED' if len(set(blocks)) == 1 else 'VARYING'}")
ab = frames(blocks[0])
print(f"  activation (84 B / 336 frames): {rle(ab)}")

# slot table: content between previous slot end and this anchor
print("\n  slot |  off   | drive | gap | drive-frame level histogram (0/1/2/3)")
prev = 0
slots = []
for k, st in enumerate(anchors):
    seg = H1[prev:st]
    e = len(seg)
    while e > 0 and seg[e-1] == 0:
        e -= 1
    content = seg[:e]
    fr = frames(content)
    h = [int((fr == L).sum()) for L in range(4)]
    gap = st - prev + 84
    slots.append((prev, len(content), gap, h, content))
    prev = st + 84
for k, (off, clen, gap, h, _) in enumerate(slots):
    if k < 14 or clen >= 200:
        print(f"  {k:4d} | 0x{off:04x} | {clen:4d}B | {gap:3d} | "
              f"L0={h[0]:4d} L1={h[1]:3d} L2={h[2]:3d} L3={h[3]:4d}")
    elif k == 14:
        print("   ... (remaining slots: short/empty, activation-only) ...")
gaps = np.array([s[2] for s in slots])
print(f"  gap histogram: " +
      ", ".join(f"{int(v)}B×{int(c)}" for v, c in zip(*np.unique(gaps, return_counts=True))))

# --- half 2 verdict -----------------------------------------------------------
print(f"\nHALF2: high-entropy, not a waveform / not an index.")
for P in (256, 288, 300, 320, 600):
    r = len(H2) // P
    v = H2[:r*P].reshape(r, P).var(axis=0).mean()
    flag = "  <-- low (real period)" if P in (300, 600) else ""
    print(f"  period {P:3d}: rows={r:3d} mean-col-var={v:.0f}{flag}")
top = np.argsort(np.bincount(H2, minlength=256))[::-1][:6]
print("  top bytes: " + ", ".join(f"0x{t:02x}:{(H2==t).sum()*100/len(H2):.1f}%" for t in top))

# --- optional: dump frames of one slot ---------------------------------------
if dump_slot is not None and 0 <= dump_slot < len(slots):
    content = slots[dump_slot][4]
    print(f"\n=== slot {dump_slot} drive frames (RLE) ===")
    print(rle(frames(content), 200))
