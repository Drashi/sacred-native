#!/usr/bin/env python3
"""Sweep a PE module for stored code pointers whose target is not an instruction of the offline export.

Sources of a 32-bit value that points into an executable section:
  data   4-aligned dwords in initialized non-executable sections (callback slots, vtables, EH/unwind tables)
  imm    `push imm32`, `mov r32, imm32` and `mov [mem], imm32` operands of exported instructions
         (atexit/_onexit handlers, window procedures, callbacks handed to registration functions)
A candidate is reported when it is neither the start of an exported instruction nor inside one (never split
existing code) and sits on a function boundary: the previous exported instruction ends exactly at the target,
or the byte before is 0xcc/0x90 padding. Ghidra then decodes it in the recipe (`entries-file:` spec) and drops
anything it cannot decode as a function, so a false positive is harmless.

  sweep_code_pointers.py --exe E --export DIR            print candidates (RVAs, with kind and source)
  sweep_code_pointers.py --exe E --export DIR --write F  merge candidates into the list file consumed by the recipe
        (repeat after each regeneration until no candidates remain: newly decoded code can expose more pointers)
  sweep_code_pointers.py --exe E --export DIR --check F [--must 0xVA ...]
        static gate: every listed RVA is an exported instruction start and every --must VA is listed
"""
import argparse, bisect, gzip, json, struct, sys


def load_pe(path):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    n = struct.unpack_from('<H', data, pe + 6)[0]
    opt = struct.unpack_from('<H', data, pe + 20)[0]
    base = struct.unpack_from('<I', data, pe + 24 + 28)[0]
    off = pe + 24 + opt
    secs = []
    for i in range(n):
        name, vs, va, rs, ro, _, _, _, _, ch = struct.unpack_from('<8sIIIIIIHHI', data, off + 40 * i)
        mapped = max(vs, rs) if ch & 0x20000000 else min(vs, rs) if vs else rs
        secs.append((name.rstrip(b'\0').decode(errors='replace'), va, mapped, ro, rs, ch))

    return data, base, secs


def load_instructions(export):
    starts = {}
    with gzip.open(export + '/instructions.jsonl.gz', 'rt') as f:
        for line in f:
            d = json.loads(line)
            starts[int(d['address']['rva'], 16)] = (d['length'], d['mnemonic'], bytes.fromhex(d['bytes']))

    return starts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--export', required=True)
    ap.add_argument('--write')
    ap.add_argument('--check')
    ap.add_argument('--must', nargs='*', default=[])
    a = ap.parse_args()
    data, base, secs = load_pe(a.exe)
    starts = load_instructions(a.export)
    ordered = sorted(starts)

    if a.check:
        listed = {}
        for line in open(a.check):
            line = line.split('#')[0].strip()
            if line:
                listed[int(line.split()[0], 16)] = line

        bad = [r for r in listed if r not in starts]
        for r in bad:
            print('NOT_COMPILED 0x%x' % r)

        missing = [m for m in a.must if (int(m, 0) - base) not in listed]
        for m in missing:
            print('MISSING_REQUIRED_ENTRY %s' % m)

        unlisted = [m for m in a.must if (int(m, 0) - base) not in starts]
        for m in unlisted:
            print('REQUIRED_NOT_COMPILED %s' % m)

        print('CODE_POINTER_ENTRIES listed=%d compiled=%d required=%d'
              % (len(listed), len(listed) - len(bad), len(a.must)))
        return 1 if bad or missing or unlisted else 0

    def inside(rva):
        i = bisect.bisect_right(ordered, rva) - 1
        return i >= 0 and ordered[i] < rva < ordered[i] + starts[ordered[i]][0]

    execs = [(va, va + sz) for n, va, sz, ro, rs, ch in secs if ch & 0x20000000]

    def in_exec(v):
        return any(lo <= v - base < hi for lo, hi in execs)

    def byte_at(rva):
        for n, va, sz, ro, rs, ch in secs:
            if va <= rva < va + rs:
                return data[ro + rva - va]

        return None

    cand = {}
    for n, va, sz, ro, rs, ch in secs:
        if ch & 0x20000000 or n == '.rsrc' or not (ch & 0x40000000):
            continue

        for k in range(0, rs - 3, 4):
            v = struct.unpack_from('<I', data, ro + k)[0]
            if in_exec(v):
                cand.setdefault(v - base, ('data', '%s+0x%x' % (n, k)))

    for rva, (length, mnemonic, raw) in starts.items():
        if length < 5:
            continue

        op = raw[0]
        if (mnemonic == 'PUSH' and op == 0x68) or (mnemonic == 'MOV' and (0xb8 <= op <= 0xbf or op in (0xc7,))):
            v = struct.unpack_from('<I', raw, length - 4)[0]
            if in_exec(v):
                cand.setdefault(v - base, ('imm', '0x%x' % (rva + base)))

    ends = {rva + s[0] for rva, s in starts.items()}
    out = []
    for rva in sorted(cand):
        if rva in starts or inside(rva):
            continue

        if rva in ends or byte_at(rva - 1) in (0xcc, 0x90):
            out.append((rva, cand[rva]))

    for rva, (kind, src) in out:
        print('0x%x  # %s %s' % (rva, kind, src))

    print('CANDIDATES %d (of %d stored code pointers)' % (len(out), len(cand)), file=sys.stderr)
    if a.write:
        kept = {}
        try:
            for line in open(a.write):
                if line.split('#')[0].strip():
                    kept[int(line.split()[0], 16)] = line.rstrip('\n')
        except FileNotFoundError:
            pass

        for rva, (kind, src) in out:
            kept.setdefault(rva, '0x%x  # %s %s' % (rva, kind, src))

        with open(a.write, 'w') as f:
            f.write('# RVAs of stored code pointers (data slots / immediates) found by '
                    'tools/lift/sweep_code_pointers.py.\n')
            f.write('# Read by the AdmitObservedEntry `entries-file:` spec; Ghidra drops entries it cannot decode.\n')
            for rva in sorted(kept):
                f.write(kept[rva] + '\n')

    return 0


sys.exit(main())
