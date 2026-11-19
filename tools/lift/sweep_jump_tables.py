#!/usr/bin/env python3
"""Sweep an offline Sacred.exe export for indexed code-pointer tables and report targets that are not instruction
starts.

A candidate is any instruction with a memory operand [REG*4 + imm32] (optionally with a base register) whose
displacement lies in an initialized section (jump tables of MSVC switches live in .text) and starts a run of 32-bit
absolute code addresses.
The run extends while entries are zero-free .text addresses. Output: one line per table
  first end count missing  (inclusive-exclusive RVAs, missing = targets that are not instruction starts)
and with --specs the AdmitObservedEntry table: specification list for the tables with missing targets.
"""
import gzip, json, os, re, struct, sys, argparse


def pe_sections(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    n = struct.unpack_from('<H', data, pe + 6)[0]
    opt = struct.unpack_from('<H', data, pe + 20)[0]
    base = struct.unpack_from('<I', data, pe + 24 + 28)[0]
    off = pe + 24 + opt
    out = []
    for i in range(n):
        name, vs, va, rs, ro, _, _, _, _, ch = struct.unpack_from('<8sIIIIIIHHI', data, off + 40 * i)
        out.append((name.rstrip(b'\0').decode(), va, vs, ro, rs, ch))

    return base, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--export', default='analysis/export/sacred-startup')
    ap.add_argument('--exe', default=os.environ.get('SACRED_ASSETS', '.') + '/Sacred.exe')
    ap.add_argument('--specs', action='store_true')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--dispatch',
                    help='generated shard directory; prove every table target is in the compiled dispatch tables')
    ap.add_argument('--prove', nargs='*', default=[], help='guest VAs whose tables and dispatch entries are printed')
    a = ap.parse_args()
    data = open(a.exe, 'rb').read()
    base, secs = pe_sections(data)
    text = [(va, va + max(vs, rs)) for n, va, vs, ro, rs, ch in secs if ch & 0x20000000]

    def in_text(rva):
        return any(lo <= rva < hi for lo, hi in text)

    def rd(rva):
        for n, va, vs, ro, rs, ch in secs:
            if va <= rva and rva + 4 <= va + rs:
                return struct.unpack_from('<I', data, ro + rva - va)[0]

        return None

    starts = set()
    cands = {}
    pat = re.compile(r'\*0x4 \+ 0x([0-9a-f]+)\]')
    for line in gzip.open(a.export + '/instructions.jsonl.gz', 'rt'):
        d = json.loads(line)
        rva = int(d['address']['rva'], 16)
        starts.add(rva)
        for o in d['operands']:
            m = pat.search(o['text'])
            if not m:
                continue

            disp = int(m.group(1), 16)
            if disp < base or rd(disp - base) is None:
                continue

            cands.setdefault(disp - base, []).append((rva, d['mnemonic'], o['text']))

    tables = []

    def run_from(first):
        entries = []
        at = first
        while True:
            v = rd(at)
            if v is None or not in_text(v - base):
                break

            entries.append(v - base)
            at += 4

        return entries, at

    for key in sorted(cands):
        first = key
        if key % 4:
            continue  # unaligned run: coincidental data, never a compiler-emitted pointer table

        control = any(u[1] in ('JMP', 'CALL') for u in cands[key])
        entries, at = run_from(first)
        # A switch whose case index starts at 1 or 2 biases the displacement: skip up to two unused leading slots.
        for skip in (4, 8) if control and not entries else ():
            entries, at = run_from(first + skip)
            if entries:
                first += skip
                break

        if len(entries) < (1 if control else 2):
            if a.verbose:
                print('SHORT', hex(first), len(entries), cands[key][0])

            continue

        missing = sorted({e for e in entries if e not in starts})
        tables.append((first, at, entries, missing, cands[key]))

    # merge identical/overlapping runs reported by several users
    tot_m = 0
    for first, end, entries, missing, users in tables:
        tot_m += len(missing)
        print('TABLE 0x%x 0x%x n=%d missing=%d users=%d %s'
              % (first, end, len(entries), len(missing), len(users), users[0][1]))
        if a.verbose:
            for e in missing:
                print('   MISSING 0x%x' % e)

    with_missing = sum(1 for t in tables if t[3])
    missing_targets = len({e for t in tables for e in t[3]})
    print('TOTAL tables=%d with_missing=%d missing_targets=%d' % (len(tables), with_missing, missing_targets))
    if a.dispatch:
        import glob
        entry = re.compile(r'^\{0x([0-9a-f]{8})u, region_', re.M)
        compiled = set()
        for path in glob.glob(a.dispatch + '/*.c'):
            compiled.update(int(m, 16) - base for m in entry.findall(open(path).read()))

        every = {e for t in tables for e in t[2]}
        absent = sorted(every - compiled)
        print('DISPATCH compiled_entries=%d table_targets=%d targets_not_compiled=%d'
              % (len(compiled), len(every), len(absent)))
        for e in absent[:20]:
            print('   NOT_COMPILED 0x%x' % (e + base))

        for want in a.prove:
            va = int(want, 0)
            hits = [t for t in tables if (va - base) in t[2]]
            for t in hits:
                table_users = ','.join('0x%x' % (u[0] + base) for u in t[4])
                print('PROOF target 0x%x in table 0x%x..0x%x (users %s) instruction_start=%s compiled_dispatch=%s'
                      % (va, t[0] + base, t[1] + base, table_users, (va - base) in starts, (va - base) in compiled))

            if not hits:
                print('PROOF target 0x%x in NO table' % va)

    if a.specs:
        print(','.join('table:0x%x:0x%x' % t[:2] for t in tables if t[3]))

main()
