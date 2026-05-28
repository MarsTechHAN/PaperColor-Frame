"""UC8159c register-format waveform parser/encoder for the E6 (Spectra 6) panel.

This is the data layer for the waveform simulator. It encodes/decodes the LUTs
you upload over SPI when PSR bit5 (REG) = 1 (SPI-loaded LUTs instead of OTP).

LUT register commands and sizes (from the UC8159c datasheet):
  0x20 VCOM (LUTC):  20 states x 11 bytes = 220 data bytes
  0x21 LUTB, 0x22 LUTW, 0x23 LUTG1, 0x24 LUTG2,
  0x25 LUTR0, 0x26 LUTR1, 0x27 LUTR2, 0x28 LUTR3, 0x29 LUTXON:
                     20 states x 13 bytes = 260 data bytes each

Per state:
  repeat(1 byte) + level-select + 8 frame-number bytes
    VCOM   level-select: 2 bits/phase, 4 phases/byte  -> 2 bytes (phases 1..8)
    colour level-select: 3 bits/phase, 2 phases/byte  -> 4 bytes (phases 1..8)
                         byte = (phaseA<<4)|phaseB  (bit7,bit3 unused)

Level codes (colour, 3-bit) — confirmed from the datasheet worked example
(state bytes 0x12 -> VSH,VSL ; 0x34 -> VSH_LV,VSL_LV):
  001=VSH 010=VSL 011=VSH_LV 100=VSL_LV ; 000=GND.
  101/110/111 map to VSH_LVX/VSL_LVX/(spare) per the datasheet's level list
  (VSH,VSH_LV,VSH_LVX,0,VSL_LVX,VSL_LV,VSL) — TENTATIVE, confirm on HW.
VCOM 2-bit codes map to {VSH+VCM_DC, VCM_DC, VSL+VCM_DC, float} — order TENTATIVE.

Nominal volts below are placeholders for the simulator; real rails come from
PWR(0x01)/booster (VSH/VSL ~ ±15 V, VSH_LV/VSL_LV ~ ±3-15 V) and are calibrated.
"""
from dataclasses import dataclass, field

NUM_STATES = 20
NUM_PHASES = 8

# colour 3-bit level code -> (name, signed nominal volts)
COLOR_LEVEL = {
    0b000: ("GND",      0.0),
    0b001: ("VSH",    +15.0),
    0b010: ("VSL",    -15.0),
    0b011: ("VSH_LV",  +5.0),
    0b100: ("VSL_LV",  -5.0),
    0b101: ("VSH_LVX", +9.0),   # tentative
    0b110: ("VSL_LVX", -9.0),   # tentative
    0b111: ("?7",       0.0),   # unknown
}
# VCOM 2-bit level code -> (name, signed nominal volts vs front plane)
VCOM_LEVEL = {
    0b00: ("float", 0.0),       # tentative
    0b01: ("VSH+VCM", +15.0),   # tentative ordering
    0b10: ("VCM",      0.0),
    0b11: ("VSL+VCM", -15.0),
}

LUT_CMDS = {
    "VCOM": 0x20, "LUTB": 0x21, "LUTW": 0x22, "LUTG1": 0x23, "LUTG2": 0x24,
    "LUTR0": 0x25, "LUTR1": 0x26, "LUTR2": 0x27, "LUTR3": 0x28, "LUTXON": 0x29,
}
# Pixel low-3-bits -> colour LUT (DTM selector). Which index yields which real
# colour on THIS panel is to be discovered on HW.
PIXEL_LUT = ["LUTB", "LUTG1", "LUTG2", "LUTW", "LUTR0", "LUTR1", "LUTR2", "LUTR3"]


@dataclass
class State:
    repeat: int = 0
    levels: list = field(default_factory=lambda: [0] * NUM_PHASES)  # per-phase code
    frames: list = field(default_factory=lambda: [0] * NUM_PHASES)  # per-phase frames


# ---- bit packing ---------------------------------------------------------
def _pack_color_levels(levels):
    return bytes(((levels[k] & 7) << 4) | (levels[k + 1] & 7) for k in range(0, 8, 2))

def _unpack_color_levels(b):
    out = []
    for byte in b:
        out += [(byte >> 4) & 7, byte & 7]
    return out

def _pack_vcom_levels(levels):
    b1 = ((levels[0] & 3) << 6) | ((levels[1] & 3) << 4) | ((levels[2] & 3) << 2) | (levels[3] & 3)
    b2 = ((levels[4] & 3) << 6) | ((levels[5] & 3) << 4) | ((levels[6] & 3) << 2) | (levels[7] & 3)
    return bytes([b1, b2])

def _unpack_vcom_levels(b):
    out = []
    for byte in b:
        out += [(byte >> 6) & 3, (byte >> 4) & 3, (byte >> 2) & 3, byte & 3]
    return out


# ---- encode / decode -----------------------------------------------------
def encode_color_lut(states):
    assert len(states) == NUM_STATES
    out = bytearray()
    for s in states:
        out.append(s.repeat & 0xFF)
        out += _pack_color_levels(s.levels)
        out += bytes(f & 0xFF for f in s.frames)
    assert len(out) == 260, len(out)
    return bytes(out)

def decode_color_lut(data):
    assert len(data) == 260, len(data)
    states = []
    for k in range(NUM_STATES):
        c = data[k * 13:(k + 1) * 13]
        states.append(State(c[0], _unpack_color_levels(c[1:5]), list(c[5:13])))
    return states

def encode_vcom_lut(states):
    assert len(states) == NUM_STATES
    out = bytearray()
    for s in states:
        out.append(s.repeat & 0xFF)
        out += _pack_vcom_levels(s.levels)
        out += bytes(f & 0xFF for f in s.frames)
    assert len(out) == 220, len(out)
    return bytes(out)

def decode_vcom_lut(data):
    assert len(data) == 220, len(data)
    states = []
    for k in range(NUM_STATES):
        c = data[k * 11:(k + 1) * 11]
        states.append(State(c[0], _unpack_vcom_levels(c[1:3]), list(c[3:11])))
    return states


# ---- analysis helpers ----------------------------------------------------
def state_frames(s):
    # NOTE: "repeat" semantics (times vs additional-times; 0 -> skip?) TBD.
    rep = s.repeat if s.repeat > 0 else 1
    return rep * sum(s.frames)

def lut_total_frames(states):
    return sum(state_frames(s) for s in states)

def lut_dc_impulse(states, level_map):
    """Net signed volt*frames over the LUT (should be ~0 for DC balance)."""
    tot = 0.0
    for s in states:
        rep = s.repeat if s.repeat > 0 else 1
        for ph in range(NUM_PHASES):
            volts = level_map[s.levels[ph]][1]
            tot += rep * s.frames[ph] * volts
    return tot

def dump_lut(name, states, level_map):
    lvlname = lambda c: level_map[c][0]
    print(f"== {name}: {lut_total_frames(states)} frames, DC={lut_dc_impulse(states, level_map):+.0f} ==")
    for i, s in enumerate(states):
        if s.repeat == 0 and sum(s.frames) == 0:
            continue
        lv = " ".join(f"{lvlname(s.levels[p])}:{s.frames[p]}" for p in range(NUM_PHASES) if s.frames[p])
        print(f"  s{i:2d} rep{s.repeat:3d}  {lv}")


# ---- self-test -----------------------------------------------------------
if __name__ == "__main__":
    import random
    random.seed(1)

    # round-trip colour LUT
    st = [State(random.randint(0, 255),
                [random.randint(0, 7) for _ in range(8)],
                [random.randint(0, 255) for _ in range(8)]) for _ in range(20)]
    assert decode_color_lut(encode_color_lut(st)) == st, "colour round-trip failed"

    # round-trip VCOM LUT (2-bit levels)
    sv = [State(random.randint(0, 255),
                [random.randint(0, 3) for _ in range(8)],
                [random.randint(0, 255) for _ in range(8)]) for _ in range(20)]
    assert decode_vcom_lut(encode_vcom_lut(sv)) == sv, "vcom round-trip failed"

    # verify the datasheet worked example: state level bytes 0x12,0x34 -> VSH,VSL,VSH_LV,VSL_LV
    ex = _unpack_color_levels(bytes([0x12, 0x34, 0x00, 0x00]))
    names = [COLOR_LEVEL[c][0] for c in ex[:4]]
    assert names == ["VSH", "VSL", "VSH_LV", "VSL_LV"], names
    print("round-trip OK; datasheet level example decodes to", names)
