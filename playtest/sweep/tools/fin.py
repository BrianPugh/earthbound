import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import grind2

if sys.argv[2] == "auto":
    grind2.do(sys.argv[1], "until input 1500 advance3; 6 B; 10 -; 6 B; 10 -; 6 B; 10 -")
o = grind2.finish(sys.argv[1], auto=sys.argv[2] == "auto")
print("FIN", "TIMEOUT" if o.endswith("TIMEOUT") else "ok", grind2.party(o + grind2.do(sys.argv[1], "state")))
