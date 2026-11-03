# ruff: noqa  (scratch recording helpers, kept as used; see README.md)
import sys

sys.path.insert(
    0, "/private/tmp/claude-501/-Users-brianpugh-projects-ebsrc/fb0daf9a-52d7-45c3-a277-26fd307b6f4d/scratchpad"
)
import grind2

if sys.argv[2] == "auto":
    grind2.do(sys.argv[1], "until input 1500 advance3; 6 B; 10 -; 6 B; 10 -; 6 B; 10 -")
o = grind2.finish(sys.argv[1], auto=sys.argv[2] == "auto")
print("FIN", "TIMEOUT" if o.endswith("TIMEOUT") else "ok", grind2.party(o + grind2.do(sys.argv[1], "state")))
