Scratch helpers sweep agent A used for 10 -> 11 .. 14 -> 15. The scripts source `env.sh` next to
them (S = this dir); their outputs (route_last.txt, the grind driver's STOP file) go to
`$SWEEP_WORK` (default `$TMPDIR`). The Python ones still assume the main checkout's path.

- env.sh: cd to the checkout, EB_BIN, `rec`/`pt`/`go` shell functions.
- chunk.sh DIR SESSION CMDS...: open the next padlog chunk, run pt commands (`go:X,Y ...` and
  `steps:N DIR; ...` go through go2.py, `fight` = Auto Fight via fin.py), close it, check, verify.
- stage.sh DIR SESSION SAVE CMDS...: chunk.sh + autoalign.py (masks above the previous length
  only, AA_MIN) + replay onto the late pads when verify says DIFFERENT. The workhorse.
- goto.sh DIR SESSION SAVE X Y [TRIES]: BFS (bfs.py, 24 px steps, enemies off via flag 11) from
  a logged snap, record the route with stage.sh, repeat until within 16 px; fights in between.
- route.sh DIR SESSION SAVE "X Y" ...: goto.sh over a list of targets.
- bfsrec.sh / bfsrecF.sh (+ bfsfine.py, 8 px steps, 4 px grid): BFS only, prints the route; the
  fine one finds tile-exact diagonal slopes the coarse one misses.
- autoalign.py DIR [MAX]: add door skip/lag pairs and fade-in idle masks for first divergences;
  refuses frames below AA_MIN (a killed or timed-out check once made it mask frame 65).
- prunehead.py DIR: drop masks past the chunks' end after cutting chunks.
- grind2.py / grind4.py / grind4drv.py: the Lake Tess grinding driver (phases walk / fight /
  heal, each one chunk, synced after each; `touch $S/STOP` stops it between phases).
- go2.py: go.py with advance3 and 6-frame battle-menu presses. fin.py: finish a battle.
- doors.py X0 Y0 X1 Y1: list doors in a rectangle. sessrun.py/cmplog.py: find where the
  late-pads run leaves the session's recorded route.

Pitfalls: `snap` runs one game frame, so keep the padlog open over it (or the recording loses
that frame); `restore` needs no padlog. Never run pt commands on a recording session with the
padlog off unless you restore a logged snap afterwards.
