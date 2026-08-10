#!/usr/bin/env python3
"""aot-check.py — validate hl's Rosetta AOT cache files.

A poisoned/truncated <sha256>.aot in ~/.cache/hl-rosettad/ makes every x86_64 run
fault. This checks the header invariants and reports which files are bad.
Usage: aot-check.py [dir]   (default: ~/.cache/hl-rosettad)
Exit 0 if all OK, 1 if any file is bad.
"""
import os, sys, struct, glob

def le(b, off, n):
    return int.from_bytes(b[off:off+n], "little")

def check(path):
    sz = os.path.getsize(path)
    problems = []
    if sz < 0x1000:
        return [f"file too small ({sz} < 4096 bytes) — truncated header"]
    with open(path, "rb") as f:
        h = f.read(0x60)
    total_size  = le(h, 0x00, 8)
    version     = le(h, 0x08, 8)
    orig_size   = le(h, 0x10, 8)
    code_offset = le(h, 0x18, 8)
    if version != 1:            problems.append(f"version={version} (want 1)")
    if code_offset != 0x1000:   problems.append(f"code_offset=0x{code_offset:x} (want 0x1000)")
    if total_size <= 0x1000:    problems.append(f"total_size=0x{total_size:x} (<= 0x1000)")
    if total_size % 0x1000:     problems.append(f"total_size=0x{total_size:x} (not page-aligned)")
    if orig_size == 0:          problems.append("orig_size=0")
    if orig_size % 0x1000:      problems.append(f"orig_size=0x{orig_size:x} (not page-aligned)")
    if sz <= code_offset:       problems.append(f"no code section (size 0x{sz:x} <= code_offset)")
    # Fields after offset 0x20 have changed across macOS releases. Do not
    # reject a valid current AOT by assigning old meanings to those bytes.
    return problems

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/.cache/hl-rosettad")
    files = sorted(glob.glob(os.path.join(d, "*.aot")))
    if not files:
        print(f"(no .aot files in {d})"); return 0
    bad = 0
    for p in files:
        probs = check(p)
        name = os.path.basename(p)
        if probs:
            bad += 1
            print(f"CORRUPT {name}: {'; '.join(probs)}")
        else:
            print(f"ok      {name}  ({os.path.getsize(p)} bytes)")
    print(f"\n{len(files)} file(s), {bad} corrupt.")
    return 1 if bad else 0

sys.exit(main())
