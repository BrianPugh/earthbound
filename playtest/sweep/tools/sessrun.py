import sys
from pathlib import Path

sys.path.insert(0, "/Users/brianpugh/projects/ebsrc/playtest/sweep")
sys.path.insert(0, "/Users/brianpugh/projects/ebsrc/playtest")
import rec
from padlog_body import runs

d = Path(sys.argv[1])
cut = int(sys.argv[2])
out = sys.argv[3]
eff = rec.effective_pads(d, d / "scenario.play")
body = rec.body_pads(d)
pads = eff[:cut] + body[cut:]
head = (d / "head.play").read_text().replace("# late-pads\n", "#\n")
items = [f"{n} {p}" for n, p in runs(pads)]
Path(out).write_text(
    head.rstrip("\n") + "\n" + "\n".join("; ".join(items[i : i + 10]) for i in range(0, len(items), 10)) + "\n"
)
