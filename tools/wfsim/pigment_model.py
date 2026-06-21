"""Electrophoretic 6-colour pigment model for the E6 (Spectra-6-class) panel.

Physics grounding (see refs at bottom):
  * 4 pigment particle types: White + subtractive Cyan/Magenta/Yellow.
  * White carries one charge polarity; C/M/Y the OPPOSITE polarity but with
    DIFFERENT charge magnitudes -> different electrophoretic MOBILITIES.
  * The display drive (from our OTP decode) has only ONE drive magnitude per
    polarity (2-bit levels: 1=-V, 2=+V, 0/3=hold). So colour is selected by the
    TEMPORAL PATTERN of +V / -V / hold pulses, NOT by voltage magnitude.

Mechanism the model captures:
  Under a field all C/M/Y (same polarity) move the SAME direction but at speeds
  set by their mobilities; White moves opposite. Particles are clamped to the
  cup walls z in [0,1] (0 = rear electrode, 1 = viewing window). Wall-trapping +
  multi-phase pulses of different durations let the controller leave a chosen
  SUBSET of colour pigments in front of white -> a specific subtractive colour.

Colour rendering (subtractive): a colour pigment in front of white filters the
reflected light. C absorbs Red, M absorbs Green, Y absorbs Blue. The visible
RGB = product of the filters of the colour pigments that end up in front of
white; pigments behind white are hidden.

The model takes a per-frame drive sequence (levels 0..3, same encoding as the
OTP body) and predicts one of the 6 panel colours. This is the tool for
"completely understanding" what a given waveform does, and for designing new
ones. Parameters are physically-ordered but not yet HW-calibrated (that's the
hardware calibration loop, task #15).
"""
from __future__ import annotations
from dataclasses import dataclass
import numpy as np

# ----------------------------------------------------------------- drive levels
# Same 2-bit encoding as the decoded OTP waveform body.
LEVEL_VOLT = {0: 0.0, 1: -1.0, 2: +1.0, 3: 0.0}     # 0/3 hold, 1=-V, 2=+V


# ------------------------------------------------------------------- pigment set
@dataclass
class Pigment:
    name: str
    sign: float          # charge polarity (+1 / -1)
    mobility: float      # speed per unit field-time (charge magnitude / drag)
    latency: int         # frames of continuous same-polarity field before it breaks free (stiction)
    absorb: tuple        # RGB transmission when in front of white (subtractive)

# White = + polarity. C/M/Y = - polarity, distinct mobilities (C fastest .. Y slowest)
# AND distinct break-free latencies (nimble small-tau .. sluggish large-tau). The
# latency is what makes a DC-BALANCED waveform's run-STRUCTURE (not its net DC)
# select which pigments move -> the real single-magnitude ACeP mechanism.
# Transmissions: C absorbs R -> (0,1,1); M absorbs G -> (1,0,1); Y absorbs B -> (1,1,0).
# White is SLUGGISH (high latency): it only moves under long reset runs, so short
# selective pulses reposition the colour pigments against a parked white. The
# three colours have a graded latency ladder (C nimble .. Y slow) so run-length
# gates which of them move: run>1 -> C; run>4 -> +M; run>8 -> +Y.
PIGMENTS = [
    Pigment("W", +1.0, 1.00, 11, (1.0, 1.0, 1.0)),
    Pigment("C", -1.0, 0.95, 1,  (0.0, 1.0, 1.0)),
    Pigment("M", -1.0, 0.55, 4,  (1.0, 0.0, 1.0)),
    Pigment("Y", -1.0, 0.30, 8,  (1.0, 1.0, 0.0)),
]
IDX = {p.name: i for i, p in enumerate(PIGMENTS)}

# The 6 panel colours as RGB anchors (for nearest-colour classification).
NAMED = {
    "white":  (1, 1, 1), "black": (0, 0, 0), "yellow": (1, 1, 0),
    "red":    (1, 0, 0), "blue":  (0, 0, 1), "green":  (0, 1, 0),
}


# --------------------------------------------------------------------- simulator
class Cup:
    """One microcup. State = vertical position z in [0,1] of each pigment band."""

    def __init__(self, step_gain=0.02):
        # start well-mixed in the middle
        self.z = np.full(len(PIGMENTS), 0.5)
        self.k = step_gain                      # position change per frame per unit (mobility*field)
        self.run_sign = 0.0                     # polarity of the current contiguous field run
        self.run_frames = 0                     # length so far of that run (for the latency gate)
        self.lat = np.array([p.latency for p in PIGMENTS])

    def reset(self, z=None):
        self.z = np.full(len(PIGMENTS), 0.5) if z is None else np.array(z, float)
        self.run_sign = 0.0
        self.run_frames = 0
        return self

    def step(self, level):
        V = LEVEL_VOLT[level]
        if V == 0.0:
            # hold lets particles re-settle/stick: next field must re-accumulate
            self.run_sign = 0.0
            self.run_frames = 0
            return self.z
        s = 1.0 if V > 0 else -1.0
        if s == self.run_sign:
            self.run_frames += 1
        else:
            self.run_sign = s
            self.run_frames = 1
        for i, p in enumerate(PIGMENTS):
            if self.run_frames > p.latency:    # broken free this frame -> drifts
                self.z[i] += self.k * p.mobility * p.sign * V
        np.clip(self.z, 0.0, 1.0, out=self.z)
        return self.z

    def run(self, levels):
        for lv in levels:
            self.step(lv)
        return self.z

    # ---- colour rendering
    def rgb(self):
        zW = self.z[IDX["W"]]
        out = np.array([1.0, 1.0, 1.0])
        for name in ("C", "M", "Y"):
            i = IDX[name]
            if self.z[i] > zW + 1e-6:           # this colour pigment is in front of white
                out *= np.array(PIGMENTS[i].absorb)
        return out

    def color(self):
        rgb = self.rgb()
        best, bd = None, 1e9
        for nm, anchor in NAMED.items():
            d = float(np.sum((rgb - np.array(anchor)) ** 2))
            if d < bd:
                bd, best = d, nm
        return best, rgb


# ---------------------------------------------------------- waveform primitives
def phase(level, n):
    return [level] * n

# canonical multi-phase drives (each a list of (level,n)); built for the model's
# wall-trapping mechanism, all using only +V/-V/hold.
def seq(*phases):
    out = []
    for lv, n in phases:
        out += phase(lv, n)
    return out
