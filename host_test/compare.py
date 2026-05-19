#!/usr/bin/env python3
"""Compare two palette-index buffers (Python reference vs C output).

Usage:
    python3 compare.py py_indices.bin c_indices.bin
"""
import sys
import numpy as np
from pathlib import Path

if len(sys.argv) != 3:
    sys.exit("usage: compare.py py_indices.bin c_indices.bin")

py = np.frombuffer(Path(sys.argv[1]).read_bytes(), dtype=np.uint8)
ci = np.frombuffer(Path(sys.argv[2]).read_bytes(), dtype=np.uint8)
if py.shape != ci.shape:
    sys.exit(f"size mismatch: {py.shape} vs {ci.shape}")

N = py.size
agree = (py == ci).sum()
print(f"per-pixel agreement: {agree}/{N} = {100*agree/N:.2f}%")

names = ["black","white","yellow","red","blue","green"]
print("\nclass-wise recall (py as reference):")
print(f"  {'class':<7} {'py-count':>8} {'c-count':>8} {'TP':>8} {'recall':>8} {'prec':>8}")
for k in range(6):
    py_mask = (py == k)
    c_mask  = (ci == k)
    tp = (py_mask & c_mask).sum()
    py_n = py_mask.sum()
    c_n  = c_mask.sum()
    rec = (tp / py_n * 100) if py_n else 0
    pre = (tp / c_n  * 100) if c_n  else 0
    print(f"  {names[k]:<7} {py_n:>8d} {c_n:>8d} {tp:>8d} {rec:>7.2f}% {pre:>7.2f}%")

# Confusion matrix
print("\nconfusion matrix (rows=python truth, cols=C prediction):")
print(f"  {'':>7} " + " ".join(f"{n[:6]:>6}" for n in names))
for k in range(6):
    row = [(py == k) & (ci == j) for j in range(6)]
    counts = [m.sum() for m in row]
    print(f"  {names[k]:<7} " + " ".join(f"{c:>6d}" for c in counts))
