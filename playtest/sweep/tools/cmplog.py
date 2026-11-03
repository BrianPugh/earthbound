# ruff: noqa  (scratch recording helpers, kept as used; see README.md)
import re, sys


def st(f):
    out = []
    for ln in open(f, errors="replace"):
        if ln.startswith("STATE"):
            m = re.search(r"pos=\((\d+,\d+)\)", ln)
            md = re.search(r"modes=\[([^\]]*)\]", ln)
            out.append((m.group(1) if m else "", md.group(1) if md else ""))
    return out


a = st(sys.argv[1])
b = st(sys.argv[2])
lo = int(sys.argv[3]) if len(sys.argv) > 3 else 0
for i in range(lo, min(len(a), len(b))):
    if a[i] != b[i]:
        print("first diff at", i, a[i], b[i])
        break
