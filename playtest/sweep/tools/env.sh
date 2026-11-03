# Sourced by the sweep helper scripts. S = this tools dir; W = scratch dir for their
# outputs (route_last.txt, the grind driver's STOP file): $SWEEP_WORK, else $TMPDIR.
S=${${(%):-%x}:A:h}
W=${SWEEP_WORK:-${TMPDIR:-/tmp}}
cd ${S:h:h:h}
export EB_BIN=${EB_BIN:-$PWD/port/unix/build-native/earthbound}
rec(){ .venv/bin/python playtest/sweep/rec.py "$@"; }
go(){ .venv/bin/python playtest/sweep/go.py "$@"; }
pt(){ .venv/bin/python playtest/pt.py "$@"; }
