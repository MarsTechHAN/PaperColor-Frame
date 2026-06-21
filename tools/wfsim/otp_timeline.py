"""Reconstruct the full HALF1 refresh timeline as 6 colour bit-planes (1 byte =
1 frame, bits[7:2]=colours, DTM order black/white/yellow/red/blue/green) and
test the user's 3-phase model:
  (1) colours flash DIFFERENTLY (DC de-bias / shake)
  (2) all colours flash the SAME (global)
  (3) colour-selective drive, colours FINISH at staggered times.
"""
import sys
import numpy as np

path = sys.argv[1] if len(sys.argv) > 1 else "../../../WAVEFORM/otp_el040ef1_60000.bin"
a = np.frombuffer(open(path, "rb").read(), dtype=np.uint8)
H1 = a[:30000].astype(np.uint8)          # 30000 frames, 1 byte each

BITS = [7, 6, 5, 4, 3, 2]
NAMES = ["black", "white", "yellow", "red", "blue", "green"]   # DTM-order hypothesis
planes = np.stack([(H1 >> b) & 1 for b in BITS])   # (6, 30000)

# Is HALF1 one refresh or many? show where the byte-stream's character changes:
# coarse activity = transitions per window, per colour.
W = 500
nwin = len(H1) // W
print(f"frames={len(H1)}  window={W}  ({nwin} windows)")
print("\n=== per-colour FLASH activity by window (transitions/window; '.' = quiet) ===")
print("  win@frame  " + " ".join(f"{n[:3]:>3}" for n in NAMES) + "   allsame%")
for w in range(nwin):
    seg = planes[:, w*W:(w+1)*W]
    tr = [int(np.count_nonzero(np.diff(seg[c]))) for c in range(6)]
    # fraction of frames where all 6 colours equal (global "all flash together")
    allsame = np.mean(seg.min(0) == seg.max(0)) * 100
    bar = " ".join(f"{t:3d}" if t else "  ." for t in tr)
    flag = "  <ALL-SAME" if allsame > 60 else ""
    print(f"  {w*W:6d}    {bar}   {allsame:4.0f}%{flag}")

# per-colour "finish frame": last frame where this colour transitions
print("\n=== per-colour FINISH (last transition frame) — staggered? ===")
fins = []
for c in range(6):
    chg = np.nonzero(np.diff(planes[c]))[0]
    fin = int(chg[-1]) if len(chg) else -1
    fins.append(fin)
order = np.argsort(fins)
for c in order:
    print(f"  {NAMES[c]:7s} (bit{BITS[c]}): last flash @frame {fins[c]}  "
          f"({fins[c]/82:.1f}s @82Hz)")
print(f"  spread between earliest and latest finish: "
      f"{max(fins)-min(f for f in fins if f>=0)} frames")
