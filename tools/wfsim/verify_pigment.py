"""Verify the (nonlinear, latency-gated) pigment model:
 (1) reach all 6 colours with DC-BALANCED (net=0) ±V patterns,
 (2) pattern-sensitivity: same net DC, different run-structure -> different colour
     (impossible for the old linear model),
 (3) bistability, (4) distinct RGB, (5) behaviour on the REAL OTP waveform.

Colour selection uses run-LENGTH as a frequency gate against the latency ladder
(W sluggish tau=11; C/M/Y = 1/4/8): run>1 moves C, run>4 adds M, run>8 adds Y.
A globally DC-balanced waveform still sorts pigments because single-frame balance
pulses (run=1) move nothing.
"""
import numpy as np
from collections import Counter
from pigment_model import Cup, LEVEL_VOLT, NAMED


def runs(level, runlen, n):
    """n contiguous runs of `runlen` frames at `level`, each followed by a hold."""
    wf = []
    for _ in range(n):
        wf += [level] * runlen + [0]
    return wf


def balance(wf):
    """Append motion-free single-frame opposite pulses until net DC = 0."""
    net = sum(LEVEL_VOLT[x] for x in wf)
    lv = 1 if net > 0 else 2
    out = list(wf)
    for _ in range(int(round(abs(net)))):
        out += [lv, 0]
    return out


# 6 DC-balanced colour waveforms (phases concatenated, then auto-balanced)
def build(phases):
    wf = []
    for p in phases:
        wf += p
    return balance(wf)

WAVEFORMS = {
    "white":  build([[2] * 120]),
    "black":  build([[1] * 120]),
    "red":    build([[1] * 120, runs(2, 3, 30)]),
    "yellow": build([[1] * 120, runs(2, 7, 40)]),
    "blue":   build([[1] * 120, runs(2, 12, 40), runs(1, 7, 30)]),
    "green":  build([[1] * 120, runs(2, 7, 40), runs(1, 3, 30)]),
}


def play(wf, tail=()):
    c = Cup().reset(); c.run(wf)
    if tail:
        c.run(list(tail))
    return c


def main():
    print("=== PIGMENT MODEL VERIFICATION (nonlinear / DC-balanced) ===\n")

    print("(1) Reach all 6 colours with DC-BALANCED (net=0) patterns:")
    for col, wf in WAVEFORMS.items():
        c = play(wf); got, _ = c.color()
        net = sum(LEVEL_VOLT[x] for x in wf)
        print(f"   {col:7s}: got {got:7s}  net DC {net:+.0f}  {len(wf):4d} frames  "
              f"{'OK' if got == col else 'MISMATCH'}")
    cov = sum(play(wf).color()[0] == col for col, wf in WAVEFORMS.items())
    print(f"   coverage: {cov}/6   (all DC-balanced)\n")

    print("(2) Pattern-sensitivity: SAME net DC (=0), DIFFERENT run-structure:")
    probes = {
        "rapid alt [+,-]x120":      [2, 1] * 120,
        "long runs [+10,-10]x12":   ([2] * 10 + [1] * 10) * 12,
        "long- / short+ pulses":    [1] * 60 + runs(2, 1, 60),
        "long+ / short- pulses":    [2] * 60 + runs(1, 1, 60),
    }
    seen = set()
    for name, wf in probes.items():
        c = play(wf); col, _ = c.color()
        seen.add(col)
        print(f"   {name:26s} net={sum(LEVEL_VOLT[x] for x in wf):+.0f} -> {col}")
    print(f"   distinct outcomes from net=0 inputs: {len(seen)} "
          f"(a LINEAR model would give exactly 1) -> {'PATTERN-SENSITIVE' if len(seen) > 1 else 'FAIL'}\n")

    print("(3) Bistability (append 80 hold frames):")
    allok = True
    for col, wf in WAVEFORMS.items():
        before = play(wf).color()[0]
        after = play(wf, tail=[0] * 80).color()[0]
        allok &= before == after
        print(f"   {col:7s}: {before} -> {after}  {'ok' if before == after else 'DRIFT'}")
    print(f"   bistable: {'YES' if allok else 'NO'}\n")

    print("(4) Rendered RGB (must be 6 distinct):")
    rgbs = {}
    for col, wf in WAVEFORMS.items():
        rgb = play(wf).color()[1]; rgbs[col] = tuple(round(float(x), 2) for x in rgb)
        print(f"   {col:7s}: {rgbs[col]}")
    print(f"   distinct: {len(set(rgbs.values()))}/6\n")

    print("(5) REAL OTP waveform body (Region 1) through the upgraded model:")
    a = np.frombuffer(open("/tmp/otp_full.bin", "rb").read(), dtype=np.uint8)[:0x74C0]
    def unpack(seg):
        return np.stack([(seg >> 6) & 3, (seg >> 4) & 3, (seg >> 2) & 3, seg & 3], 1).reshape(-1)
    nunits = len(a) // 300
    cols = Counter()
    for u in range(nunits):
        lv = unpack(a[u * 300:(u + 1) * 300]).tolist()
        cols[play(lv).color()[0]] += 1
    print(f"   {nunits} units -> predicted-colour histogram: {dict(cols)}")
    print("   (now responds to waveform STRUCTURE, not just net DC; true per-colour")
    print("    mapping still needs the opaque OTP Region-2 selection or the HW loop.)")


if __name__ == "__main__":
    main()
