"""Find where two runs' sim-heap histories diverge.
Usage: python -I heap_diff.py <A_draws.log> <B_draws.log>
Compares the sequence of sim-heap events (M alloc, X reused free, x quarantined free) and prints the first
difference with context - typically a block freed inside a tick in one run but outside (quarantined) in the other."""
import sys

KINDS = ('M', 'X')  # quarantined frees (x) never change future addresses

def events(path):
    out = []
    for line in open(path, errors='replace'):
        p = line.split()
        if len(p) > 3 and p[1] in KINDS:
            out.append(line.rstrip('\n'))
    return out

def key(line):
    p = line.split()
    return (p[0], p[1], p[3])  # tick, kind, offset (ignore stack for alignment)

def main(a, b):
    ea, eb = events(a), events(b)
    print(f'A: {len(ea)} heap events, B: {len(eb)}')
    n = min(len(ea), len(eb))
    i = next((k for k in range(n) if key(ea[k]) != key(eb[k])), None)
    if i is None:
        print('heap histories identical')
        return
    print(f'first difference at event #{i}:')
    for k in range(max(0, i - 6), min(n, i + 6)):
        mark = '>>' if k == i else '  '
        print(f' {mark} A {ea[k]}')
        print(f'    B {eb[k]}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
