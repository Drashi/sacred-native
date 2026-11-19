#!/usr/bin/env python3
"""Folds a SACRED_PROFILE sample file (runtime/modules/profiler.cpp) with the executable's symbols.

usage: profile_report.py SAMPLES BINARY [--thread ID] [--top N]
Prints samples per thread, then for the busiest thread (or --thread) the self and inclusive top lists by symbol and
self time by guest module (region_XXXXXXXX symbols: granny.dll 0x1000xxxx, otherwise Sacred.exe and the rest).
"""
import argparse, bisect, collections, subprocess


def symbols(binary):
    out = subprocess.run(['nm', '-n', binary], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'tT':
            addrs.append(int(parts[0], 16))
            names.append(parts[2].lstrip('_'))

    return addrs, names


def module(name):
    if name.startswith('region_'):
        a = int(name[7:15], 16)
        if 0x10000000 <= a < 0x10100000:
            return 'granny.dll'

        if a < 0x02000000:
            return 'Sacred.exe'

        return 'guest other %08x' % (a & 0xfff00000)

    return 'host'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('samples')
    ap.add_argument('binary')
    ap.add_argument('--thread')
    ap.add_argument('--top', type=int, default=40)
    args = ap.parse_args()
    addrs, names = symbols(args.binary)
    cache = {}
    lines = open(args.samples).read().splitlines()
    for line in lines:
        if line.startswith('sym '):
            _, a, n = line.split(' ', 2)
            cache[int(a, 16)] = n

    lines = [l for l in lines if not l.startswith('sym ')]

    def sym(pc):
        if pc not in cache:
            i = bisect.bisect_right(addrs, pc) - 1
            cache[pc] = names[i] if i >= 0 else hex(pc)

        return cache[pc]

    header = lines[0].split()
    ticks = int(header[1])
    threads = collections.defaultdict(list)
    for line in lines[1:]:
        p = line.split()
        if len(p) < 2:
            continue

        # frame 1 is lr: a real caller only in a leaf; it is dropped when it repeats frame 2's symbol
        host, _, guest = ' '.join(p[1:]).partition('|')
        threads[p[0]].append(([int(x, 16) for x in host.split()], [int(x, 16) for x in guest.split()]))

    print('ticks', ticks)
    for t, s in sorted(threads.items(), key=lambda kv: -len(kv[1]))[:8]:
        print(f'thread {t}: {len(s)} samples ({100*len(s)/ticks:.0f}% of ticks running)')

    tid = args.thread or max(threads, key=lambda t: len(threads[t]))
    stacks = threads[tid]
    total = len(stacks)
    self_, incl, mods = collections.Counter(), collections.Counter(), collections.Counter()
    for st, _ in stacks:
        names_ = [sym(pc) for pc in st]
        self_[names_[0]] += 1
        mods[module(names_[0])] += 1
        for n in set(names_):
            incl[n] += 1

    print(f'\n--- thread {tid}: {total} samples')
    print('--- self by module')
    [print(f'{v:7d} {100*v/total:5.1f}% {k}') for k, v in mods.most_common()]
    print('--- self')
    [print(f'{v:7d} {100*v/total:5.1f}% {k}') for k, v in self_.most_common(args.top)]
    print('--- inclusive')
    [print(f'{v:7d} {100*v/total:5.1f}% {k}') for k, v in incl.most_common(args.top)]

    regions = sorted(int(n[7:], 16) for n in names if len(n) == 15 and n.startswith('region_') and '.' not in n)

    def region(a):
        i = bisect.bisect_right(regions, a) - 1
        return regions[i] if i >= 0 else 0

    gself, gincl, gpairs = collections.Counter(), collections.Counter(), collections.Counter()
    guest_total = 0
    for _, g in stacks:
        if not g:
            continue

        guest_total += 1
        gself[region(g[0])] += 1
        seen = set()
        for k, a in enumerate(g):
            r = region(a) if k == 0 else a
            if r not in seen:
                gincl[r] += 1
                seen.add(r)

        if len(g) > 1:
            gpairs[(region(g[0]), g[1])] += 1

    if guest_total:
        print(f'--- guest: {guest_total} samples; self by block region (source_pc)')
        [print(f'{v:7d} {100*v/guest_total:5.1f}% region_{k:08x}') for k, v in gself.most_common(args.top)]
        print('--- guest inclusive (frame 0 by region, then return addresses)')
        [print(f'{v:7d} {100*v/guest_total:5.1f}% {k:08x}') for k, v in gincl.most_common(args.top)]
        print('--- guest (block region, first return address)')
        for k, v in gpairs.most_common(args.top):
            print(f'{v:7d} {100*v/guest_total:5.1f}% region_{k[0]:08x} <- {k[1]:08x}')

main()

