"""Summarise a bperf profile: frame timing, then the main thread's hottest functions (self and inclusive).

  python -I tools/report.py <game>/bperf_profile_*.txt [--recon DIR] [--top N] [--thread N]

Names come from the multiplayer project's Ghidra exports (<recon>/<module>_functions.tsv, rva in hex), default from
BMP_RECON in local-paths.txt; this mod's own code (dbghelp.dll) from build/dbghelp.pdb, so profile with the build
that is in build/. Other modules are shown as module+rva.
"""
import argparse, bisect, collections, ctypes, ctypes.wintypes as wt, os, statistics, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def local_setting(key):
    try:
        for line in open(os.path.join(ROOT, 'local-paths.txt'), encoding='utf-8'):
            k, _, v = line.partition('=')
            if k.strip().upper() == key:
                return v.strip()
    except OSError:
        pass
    return os.environ.get(key)


class Names:
    def __init__(self, recon):
        self.tables = {}
        if not recon:
            return
        for mod in ('Application-steam-x64.exe', 'Runtime-steam-x64.dll'):
            path = os.path.join(recon, mod.replace('.', '_') + '_functions.tsv')
            if not os.path.exists(path):
                continue
            rows = []
            with open(path, encoding='utf-8', errors='replace') as f:
                next(f)
                for line in f:
                    p = line.rstrip('\n').split('\t')
                    rows.append((int(p[0], 16), int(p[1]), p[2]))
            rows.sort()
            self.tables[mod.lower()] = ([r[0] for r in rows], rows)

        self.own = None  # (dbghelp handle, load base) for build/dbghelp.dll's PDB
        self.own_cache = {}
        dll = os.path.join(ROOT, 'build', 'dbghelp.dll')
        if os.path.exists(dll) and os.path.exists(dll[:-3] + 'pdb'):
            try:
                # the system's dbghelp, by full path: this project's own build is named dbghelp.dll too
                dbg = ctypes.WinDLL(os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'dbghelp.dll'))
                h = ctypes.c_void_p(0x6265)
                dbg.SymSetOptions(0x2)  # UNDNAME
                dbg.SymInitialize(h, os.path.dirname(dll).encode(), False)
                dbg.SymLoadModuleEx.restype = ctypes.c_uint64
                if dbg.SymLoadModuleEx(h, None, dll.encode(), None, ctypes.c_uint64(0x10000000), 0, None, 0):
                    self.own = (dbg, h, 0x10000000)
            except OSError:
                pass

    def own_name(self, rva):
        if rva not in self.own_cache:
            class SYMBOL_INFO(ctypes.Structure):
                _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                            ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                            ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                            ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                            ('Name', ctypes.c_char * 512)]
            dbg, h, base = self.own
            si = SYMBOL_INFO(); si.SizeOfStruct = 88; si.MaxNameLen = 500
            disp = ctypes.c_uint64()
            ok = dbg.SymFromAddr(h, ctypes.c_uint64(base + rva), ctypes.byref(disp), ctypes.byref(si))
            self.own_cache[rva] = f'bperf!{si.Name.decode()}' if ok else f'dbghelp.dll+{rva:x}'
        return self.own_cache[rva]

    def name(self, mod, rva):
        if self.own and mod.lower() == 'dbghelp.dll':
            return self.own_name(rva)
        t = self.tables.get(mod.lower())
        short = mod.split('-')[0] if mod.lower().endswith(('-steam-x64.exe', '-steam-x64.dll')) else mod
        if t:
            i = bisect.bisect_right(t[0], rva) - 1
            if i >= 0:
                start, size, name = t[1][i]
                if rva < start + max(size, 1):
                    return f'{short}!{name}'
        return f'{short}+{rva:x}'


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * p))] if xs else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('--recon', default=local_setting('BMP_RECON'))
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--thread', type=int, default=None, help='thread index (default: main thread)')
    a = ap.parse_args()
    names = Names(a.recon)

    mods, threads, frames, stacks = {}, {}, [], []
    seconds = 0
    with open(a.profile, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == 'seconds':
                seconds = float(p[1])
            elif p[0] == 'module':
                mods[p[1]] = p[2]
            elif p[0] == 'thread':
                threads[int(p[1])] = {'tid': p[2], 'busy': int(p[4]), 'idle': int(p[6]), 'main': 'main' in p[7:]}
            elif p[0] == 'frame':
                frames.append(tuple(int(x) for x in p[1:]))
            elif p[0] == 'stack':
                frames_ = []
                for fr in p[3:]:
                    m, _, r = fr.partition(':')
                    frames_.append((mods.get(m, '?'), int(r, 16)))
                stacks.append((int(p[1]), int(p[2]), frames_))

    print(f'{a.profile}: {seconds:.1f} s, {len(frames)} frames, {sum(s[1] for s in stacks)} busy samples')
    if frames:
        ft = [x[0] / 1000 for x in frames]
        up = [x[1] / 1000 for x in frames]
        dr = [x[2] / 1000 for x in frames]
        ticks = [x[4] if len(x) > 4 else 0 for x in frames]
        tick_ms = sum(up) / max(1, sum(ticks))
        print(f'  frame ms: avg {statistics.mean(ft):.2f}  p50 {pct(ft, .5):.2f}  p95 {pct(ft, .95):.2f}  '
              f'p99 {pct(ft, .99):.2f}  max {max(ft):.2f}   -> {1000 / statistics.mean(ft):.1f} fps')
        print(f'  update ms/frame avg {statistics.mean(up):.2f} (p95 {pct(up, .95):.2f})   '
              f'draw ms/frame avg {statistics.mean(dr):.2f} (p95 {pct(dr, .95):.2f})')
        print(f'  sim ticks/frame avg {statistics.mean(ticks):.2f}   update ms per sim tick ~{tick_ms:.3f}   '
              f'sim rate {sum(ticks) / max(seconds, 1e-9):.0f} ticks/s (60 = 1x speed)')
        if frames and len(frames[0]) > 6:
            solves = sum(x[5] for x in frames)
            solve_ms = sum(x[6] for x in frames) / 1000
            print(f'  path solves: {solves / max(1, sum(ticks)):.2f}/tick, {1000 * solve_ms / max(1, solves):.0f} us each, '
                  f'{solve_ms / len(frames):.2f} ms/frame ({100 * solve_ms / max(1e-9, sum(ft)):.1f}% of frame time)')
    print('  threads:', ', '.join(f"#{i}{'(main)' if t['main'] else ''} busy {t['busy']}" for i, t in sorted(threads.items())))

    want = a.thread if a.thread is not None else next((i for i, t in threads.items() if t['main']), 0)
    total = sum(n for t, n, _ in stacks if t == want)
    if not total:
        print('no samples for that thread')
        return
    self_ = collections.Counter()
    incl = collections.Counter()
    for t, n, fr in stacks:
        if t != want:
            continue
        resolved = [names.name(m, r if i == 0 else r - 1) for i, (m, r) in enumerate(fr)]
        self_[resolved[0]] += n
        for nm in set(resolved):
            incl[nm] += n
    print(f'\nthread #{want}: {total} samples\n\n-- self (where the CPU is) --')
    for nm, n in self_.most_common(a.top):
        print(f'{100 * n / total:6.2f}%  {nm}')
    print('\n-- inclusive (function + everything it calls) --')
    for nm, n in incl.most_common(a.top):
        print(f'{100 * n / total:6.2f}%  {nm}')


if __name__ == '__main__':
    sys.exit(main())
