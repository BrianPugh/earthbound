# Clean sweep status

The whole main story replayed in autodiff as save-to-save segments (`NN-to-MM-<name>.play`,
saves 01-36 of `playtest/saves/INDEX`; 37 is a side save), each one the route a player walks
through that stretch's story events (no grinding: where INDEX says a save was levelled up, the
segment plays the story events and skips the levelling). A pass is clean only if every segment
is identical vs the emu, has 0 lost frames (`--port-only` on a `-DEB_LOST_FRAME_CHECK=ON`
native build) and no source fix was needed during the pass. A real divergence gets a faithful
fix (one commit per root cause), marks the pass dirty, and the pass carries on through the
remaining segments; the next pass restarts at segment 1.

```
uv run --no-sync python playtest/autodiff.py playtest/sweep/*.play -j 4
uv run --no-sync python playtest/autodiff.py playtest/sweep/*.play --port-only --bin port/unix/build-lfc-native/earthbound
```

## How a segment is made

The original reads a late pad after a lag frame: a frame that starts in the same wall frame
as the one before it completed (lag, or an `LSTEP N mid` sample) plays the pad the emu was
holding then. Onett's streets and caves lag even overclocked, so a port recording drifts a
pixel from the original at each pad change there, and the drift can change who touches whom.
`# late-pads` (autodiff.py) reruns the port on the pads the original read (from the emu run's
LSTEP sequence). That changes what the port plays, so a segment is recorded in chunks, each
planned on the late pads of the ones before (`playtest/sweep/rec.py`):

1. `DIR/head.play` holds the header (`# save:`, `# late-pads`, setup, `---`). `rec.py DIR
   replay SESSION SAVE` starts a pt.py session on SAVE (the 256x224 build autodiff runs),
   replays the setup, the autodiff seed and the late pads of the chunks so far, and opens
   `padlog DIR/NNN.pad` for the next chunk.
2. Play the chunk: `playtest/sweep/go.py SESSION X,Y ...` walks waypoints (Auto Fighting
   battles; `--steps "N DIR; ..."` for fixed moves), talks and menus by hand with `until`.
   Keep chunks short (a door or two, a battle): `padlog off` at the end.
3. `rec.py DIR check` runs autodiff on head + chunks. Line up each load in head.play
   (playtest/README.md): a door is `# skip: K` (the first emu sample is part-way into the
   load), `# lag: K+20` (its fade-in starts a CPU-time frame later) and `# ignore: idle K+30`
   (that FADE_IN ends a logic frame later after the load lag); a battle's end is the same
   idle frame. The scratchpad helper loops for this were: add a skip at a multi-field 1-frame
   divergence, a lag 20 frames after it, an idle mask for an idle-only one; anything else is
   a divergence to investigate.
4. `rec.py DIR verify SESSION` checks the check's late-pads port run ends where the session
   is. If not, the chunk's late pads took the route elsewhere (a battle started or didn't):
   find the frame in `DIR/ad/scenario.port.log`, `rec.py DIR truncate N` before it, replay,
   and record the rest again.
5. `rec.py DIR build playtest/sweep/NN-to-MM-name.play`. Lost frames: run the lost-frame
   build on the late-pads script (`EB_BIN=port/unix/build-lfc-native/earthbound --headless
   --save COPY.srm --input DIR/ad/scenario.port.play`, grep `LOST-FRAME`); `--port-only` plays
   the recorded pads, which leave the route.

Segment 01 predates this and used `playtest/sweep/nudge.py` (moves such a pad change a frame
later in the body itself), which only works for isolated lag frames.

Harness additions for the sweep: `# overclock-on: K [MODE]` (autodiff.py) turns the emu's
overclock on after port frame K, so the setup and the first frames (NPC phases from the boot)
run at normal speed and a lag-heavy walk after it runs overclocked; `# late-pads` (above).

Recurring masks, all lag (each listed per segment):

- `ignore: idle K` 1 frame at the end of a fade-in after a door load (K = skip + 30) or after
  a battle: the NMI that would take the fade's first step ran during the load, so the
  original's fade-in ends a logic frame later (as in the autodiff suite).
- Door loads: `skip: K` (the first emu sample is part-way into the load) and `lag: K+20` (its
  fade-in starts a CPU-time frame later), as in hotel-door.play and police-gauntlet.play.

## Pass 1 (started 2026-11-02 at adb66e8b)

| Segment | Status | Logic frames | Commits | Notes |
|---|---|---|---|---|
| 01-to-02-onett-hotel | identical | 2983 (2924 compared) | 64453f54 | House phone → front door → down the hill past the library → Hotel Onett → lobby phone, Dad: Record, Continue. No battles. Masks: `idle 2315` (the hotel door's FADE_IN after the load lag). Lag/skip: 235 (front door CPU frame), skip 2285 / lag 2305 (hotel door load). Overclock on from 1000. 2 pad changes nudged |
| 02-to-03-mayor-key | identical | 17765 (17399 compared) | ac02f536 | Hotel side door → street (Pogo Punk group) → arcade (Pogo Punk; doorway Shark, No: Yes Man Junior) → overworld Lifeup → Frank, Frankystein Mark II (Lifeup, Auto Fight) → arcade → town hall, Mayor Pirkle (key, Yes) → hotel phone, Dad: Record, Continue. late-pads (26 pads). Masks: `idle` 3590, 4950, 5955 (post-battle fade-ins), 3857, 10246, 11057, 12932, 14237, 15928, 16978 (door fade-ins); `focus 8030` (Frankystein's PSI menu: the original redraws the preview window over lag frames 8031-8032 and the 8030 sample catches the focus on it part-way). 9 door skip/lag pairs |
| 03-to-04-giant-step | identical | 39109 (34916 compared) | 23f5c133 | Hotel side door → north past the library → shack (key) → cave behind it (3 levels, ropes; 2 overworld Lifeups) → Giant Step ledge → side cave → dungeon → Titanic Ant (Rockin α, Pizza, Hamburger, Bash; L10 after) → footprint melody → back down the cave → the cop at the exit (Yes) → shack → hotel phone, Dad: Record, Continue. No overclock (turning it on changed the original's lag and the late pads before the route's encounters). late-pads (155). Masks: `idle` ×22 (door fade-ins at skip+30 and post-battle fade-ins), 19 door skip/lag pairs. The rope top's enemies make the original lag every frame (`LSTEP mid`), so ~4000 frames there are uncompared |
| 04-to-05-police-twoson | identical | 19216 (18096 compared) | (this file's commit) | Hotel side door → diagonal road → police station → front-desk lecture → officer, Yes → gauntlet (4 Cops + Captain Strong: Bash, Lifeup α below 85 HP; a first try with a 50 HP threshold died to Strong and was cut) → out of the station → Onett Town gate → roadblock cops → dirt path → map seam → Twoson → hotel phone, Dad: Record, Continue. late-pads (104). Masks: `idle` 13701, 13985, 16948 (door / seam fade-ins). 9 skip/lag pairs (doors, the gauntlet's TELEPORT_TOs, the seam) |
