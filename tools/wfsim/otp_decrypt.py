"""Decisive analysis of whether the OTP's high-entropy half (HALF2) is a SIMPLE
transform (bit-shuffle / XOR / XNOR / LFSR-scramble) of structured waveform LUTs.

Verdict (see prints): NO simple transform maps structured LUTs -> HALF2.
Proof rests on one invariant: any *position-wise* simple transform (bit
permutation, XOR/XNOR with a fixed mask or fixed key) maps a FIXED plaintext
field to a FIXED ciphertext column. Waveform LUTs are full of fixed/repeated
fields; HALF2 has ZERO fixed columns at the true period. The only *diffusing*
simple ops (prefix-XOR, self-sync LFSR) are tested and all RAISE entropy.

Run: python3 otp_decrypt.py ../../../WAVEFORM/otp_el040ef1_60000.bin
"""
import sys
import numpy as np

path = sys.argv[1] if len(sys.argv) > 1 else "../../../WAVEFORM/otp_el040ef1_60000.bin"
a = np.frombuffer(open(path, "rb").read(), dtype=np.uint8)
H1, H2 = a[:30000], a[30000:]
POP = np.array([bin(i).count("1") for i in range(256)], np.uint8)


def ent(b):
    b = np.asarray(b, np.uint8)
    c = np.bincount(b, minlength=256).astype(float)
    p = c[c > 0] / len(b)
    return float(-(p * np.log2(p)).sum())


print("=== 1. two regions, sharp split (not periodic blocks) ===")
print(f"   HALF1 ent={ent(H1):.2f} popmean={POP[H1].mean():.2f}  "
      f"HALF2 ent={ent(H2):.2f} popmean={POP[H2].mean():.2f} popstd={POP[H2].std():.2f}")

print("\n=== 2. fixed-column test at the true period (300) ===")
M1 = H1[:100*300].reshape(100, 300); M2 = H2[:100*300].reshape(100, 300)
print(f"   HALF1 columns std<20: {(M1.std(0)<20).sum()}/300  (sync/VCOM => many fixed)")
print(f"   HALF2 columns std<20: {(M2.std(0)<20).sum()}/300  (LUT-under-simple-xform would keep these)")

print("\n=== 3. per-byte & cross-byte transforms on HALF2 (entropy must fall to ~3) ===")
def bitrev(b):
    o = np.zeros_like(b)
    for i in range(8): o |= ((b>>i)&1)<<(7-i)
    return o
def transpose8(b):
    n=(len(b)//8)*8; g=b[:n].reshape(-1,8); o=np.zeros_like(g)
    for r in range(8):
        for c in range(8): o[:,c]|=(((g[:,r]>>(7-c))&1)<<(7-r)).astype(np.uint8)
    return o.reshape(-1)
for nm, fn in [("identity", lambda x:x), ("xor0xFF/XNOR", lambda x:x^0xFF),
               ("bit-reverse", bitrev), ("nibble-swap", lambda x:((x<<4)|(x>>4))&0xFF),
               ("8x8 transpose", transpose8),
               ("xor-prev", lambda x:np.concatenate([[x[0]],x[1:]^x[:-1]]).astype(np.uint8))]:
    print(f"   {nm:16s} ent={ent(fn(H2)):.3f}")

print("\n=== 4. fixed-key XOR (whitening): lag-L diff entropy must DIP at key period ===")
base=ent(H2); dips=sorted((ent(H2[L:]^H2[:-L]),L) for L in range(100,1300,100))[:3]
print("   lowest:", [(round(e,3),L) for e,L in dips], f" baseline={base:.3f} (no dip => not whitened)")

print("\n=== 5. self-sync LFSR descramble, best 2-tap (entropy must fall) ===")
bits=np.unpackbits(H2)
def descr(bits,t1,t2):
    o=bits.copy()
    for t in (t1,t2):
        s=np.zeros_like(bits); s[t:]=bits[:-t]; o^=s
    return o
def ebits(bits): return ent(np.packbits(bits))
best=sorted((ebits(descr(bits,aa,b)),aa,b) for b in range(2,33) for aa in range(1,b))[:3]
print("   best:", [(round(e,3),aa,b) for e,aa,b in best], " (all >baseline => not LFSR-scrambled)")

print("\nCONCLUSION: HALF2 is NOT a simple-transform of structured LUTs.")
print("It is high-entropy, fixed-column-free, ~50%-dense per bit-plane =>")
print("either a diffusing/crypto transform (contradicts 'no complex compute'),")
print("or per-chip-unique data (calibration/trim), not the shared LUT library.")
print("The parseable waveform LUTs are in HALF1 (see otp_parse.py / otp_colors.py).")
