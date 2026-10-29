"""prof_diff.py <before> <after> <out> -- the samples taken between two
prof.txt snapshots (RECOMP_PROF), in the same format, for prof_report.py."""
import sys, collections
def load(p):
    c = collections.Counter(); meta = []
    for ln in open(p):
        s = ln.split()
        if not s: continue
        if s[0] in ('#', 'T'): meta.append(ln); continue
        if s[0] == 'M': c[('M', s[2], ' '.join(s[3:]))] += int(s[1]); continue
        c[(s[0], s[2])] += int(s[1])
    return c, meta
a, _ = load(sys.argv[1]); b, meta = load(sys.argv[2])
with open(sys.argv[3], 'w') as f:
    for ln in meta: f.write(ln)
    for k, v in b.items():
        d = v - a.get(k, 0)
        if d <= 0: continue
        if k[0] == 'M': f.write('M %d %s %s\n' % (d, k[1], k[2]))
        else: f.write('%s %d %s\n' % (k[0], d, k[1]))
