import re, sys
from pathlib import Path

d = Path(sys.argv[1])
tot = sum(len(c.read_text().split()) for c in sorted(d.glob("[0-9][0-9][0-9].pad")))
p = d / "head.play"
out = []
dropped = []
for ln in p.read_text().split("\n"):
    m = (
        re.match(r"# (skip|lag|ignore: idle) (\d+)", ln)
        or re.match(r"# (skip|lag): (\d+)$", ln)
        or re.match(r"# ignore: idle (\d+)$", ln)
    )
    nums = re.findall(r"^# (?:skip|lag): (\d+)$|^# ignore: idle (\d+)", ln)
    v = [int(x) for t in nums for x in t if x]
    if v and v[0] > tot - 2:
        dropped.append(ln)
        continue
    out.append(ln)
p.write_text("\n".join(out))
print("total", tot, "dropped", dropped)
