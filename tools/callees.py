"""Where does a function's time go? Splits a profile's main-thread samples under FUNC by direct callee.

  python -I tools/callees.py <profile> <name substring> [--depth 1] [--top 25]
"""
import argparse, collections, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('func')
    ap.add_argument('--depth', type=int, default=1, help='callee levels to show (1 = direct callees)')
    ap.add_argument('--top', type=int, default=25)
    a = ap.parse_args()
    names = report.Names(report.local_setting('BMP_RECON'))
    mods, total, under = {}, 0, 0
    paths = collections.Counter()
    for line in open(a.profile, encoding='utf-8', errors='replace'):
        p = line.split()
        if not p:
            continue
        if p[0] == 'module':
            mods[p[1]] = p[2]
        elif p[0] == 'stack' and p[1] == '0':
            n = int(p[2])
            total += n
            fr = []
            for i, x in enumerate(p[3:]):
                m, _, r = x.partition(':')
                fr.append(names.name(mods.get(m, '?'), int(r, 16) - (0 if i == 0 else 1)))
            hit = [i for i, f in enumerate(fr) if a.func in f]
            if not hit:
                continue
            k = hit[-1]  # outermost occurrence
            under += n
            chain = fr[max(0, k - a.depth):k][::-1] or ['(self)']
            paths[' > '.join(chain)] += n
    print(f'{a.func}: {100 * under / max(1, total):.1f}% of main-thread samples')
    for c, n in paths.most_common(a.top):
        print(f'{100 * n / total:6.2f}%  {c}')


if __name__ == '__main__':
    main()
