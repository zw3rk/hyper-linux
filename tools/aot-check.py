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
    code_align  = le(h, 0x50, 4)
    entry_count = le(h, 0x54, 4)
    if version != 1:            problems.append(f"version={version} (want 1)")
    if code_offset != 0x1000:   problems.append(f"code_offset=0x{code_offset:x} (want 0x1000)")
    if code_align != 0x1000:    problems.append(f"code_align=0x{code_align:x} (want 0x1000)")
    if total_size == 0:         problems.append("total_size=0")
    if entry_count == 0:        problems.append("entry_count=0")
    if sz <= code_offset:       problems.append(f"no code section (size 0x{sz:x} <= code_offset)")
    # total_size is the mapped (code+BSS) size and is >= the stored code; the file
    # must at least hold the header+code up to code_offset. A well-formed file has
    # size >= code_offset and typically < total_size (BSS not stored).
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
