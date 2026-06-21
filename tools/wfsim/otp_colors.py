"""Extract the per-color drive waveforms from the E6 OTP dump.

CORRECT MODEL (E6 / Spectra-6 = 4 particles White/Yellow/Red/Blue, 1 bit per
pixel, 6 displayed colors; color is set purely by the temporal pulse pattern,
not by a per-frame voltage level):

  HALF1 [0..30000) is the waveform, read as 1 byte = 1 frame.
  In each byte, bits[7:2] are SIX color bit-planes (1 bit per color per frame);
  bits[1:0] are unused (always 0 in colour-drive bytes).

  - colour-drive frames: each colour's bit = 1 (rest/hold) most of the time,
    0 = that colour participates in the current drive phase. The six planes are
    distinct (pairwise corr 0.23-0.48) with different cadences -> 6 colours.
  - the recurring fixed 84-byte ACTIVATION block (01,55..,5a,aa..,ff..,fd,55..,
    5a,aa..) is a global SHAKE: in bit-plane view the even-bit and odd-bit
    colour groups toggle in ANTIPHASE = the two charge-polarity particle groups
    driven oppositely (W/Y vs R/B), which agitates/erases before colour drive.

Evidence this beats the "4 sequential frames / 2-bit level" reading: only
bits[7:2] ever carry colour data; bits[1:0] are dead; the six planes are
independent rather than 4 redundant copies.  Run:
    python3 otp_colors.py [path]
"""
import sys
import numpy as np

path = sys.argv[1] if len(sys.argv) > 1 else "../../../WAVEFORM/otp_el040ef1_60000.bin"
a = np.frombuffer(open(path, "rb").read(), dtype=np.uint8)
H1 = a[:30000]

COLOR_BITS = [7, 6, 5, 4, 3, 2]          # the six colour bit-planes


def rle(x, n=32):
    x = list(x); out = []; i = 0
    while i < len(x) and len(out) < n:
        j = i
        while j < len(x) and x[j] == x[i]:
            j += 1
        out.append(f"{x[i]}x{j-i}"); i = j
    return " ".join(out) + (" …" if i < len(x) else "")


def runstats(x):
    if len(x) < 2:
        return 1.0, 1
    chg = np.nonzero(np.diff(x))[0]
    r = np.diff(np.concatenate(([-1], chg, [len(x)-1])))
    return float(r.mean()), int(r.max())


# segment on the 84-byte activation block (anchor = 0x00,01,55,55,55)
anchors = [i for i in range(1, len(H1) - 4)
           if H1[i] == 1 and H1[i+1] == 0x55 and H1[i+2] == 0x55
           and H1[i+3] == 0x55 and H1[i-1] == 0]
prev, drive_segs = 0, []
for st in anchors:
    seg = H1[prev:st]
    e = len(seg)
    while e > 0 and seg[e-1] == 0:           # strip 16-byte zero pad
        e -= 1
    if e:
        drive_segs.append(np.array(seg[:e]))
    prev = st + 84
drive = np.concatenate(drive_segs)
print(f"colour-drive frames = {len(drive)}  (across {len(drive_segs)} drive segments)")
print(f"all drive bytes have bits[1:0]==0 ? {(drive & 3 == 0).all()}  "
      f"distinct bytes = {len(np.unique(drive))}")

# global per-colour stats
print("\n=== 6 per-colour bit-planes (whole drive) ===")
print("  color | bit | %rest(=1) | mean-run max-run")
planes = {}
for c, b in enumerate(COLOR_BITS):
    p = (drive >> b) & 1
    planes[b] = p
    mr, mx = runstats(p)
    print(f"   C{c}   | b{b}  | {p.mean()*100:5.1f}%   | {mr:6.2f} {mx:5d}")

# pairwise correlation (confirm 6 distinct colours, not redundant copies)
P = np.stack([planes[b].astype(float) for b in COLOR_BITS])
P -= P.mean(1, keepdims=True)
C = (P @ P.T) / len(drive)
d = np.sqrt(np.diag(C))
corr = C / np.outer(d, d)
print("\n=== pairwise correlation (>~0.9 would mean redundant; these are distinct) ===")
print("       " + "   ".join(f"C{c}" for c in range(6)))
for i in range(6):
    print(f"  C{i}: " + " ".join(f"{corr[i,j]:+.2f}" for j in range(6)))

# decode first rich drive segment per colour
s0 = max(drive_segs, key=len)
print(f"\n=== longest drive segment ({len(s0)} frames) — per-colour pulse pattern ===")
print("   (1 = rest/hold, 0 = colour driven this frame)")
for c, b in enumerate(COLOR_BITS):
    print(f"   C{c}(b{b}): {rle((s0 >> b) & 1, 30)}")
