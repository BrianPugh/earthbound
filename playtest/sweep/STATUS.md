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

1. `pt.py start NAME --save playtest/saves/NN-Name.srm` on a native 256x224 build (the
   autodiff boot is the same), then `rng 4660 22136; fc 0; padlog FILE` (the autodiff seed).
2. Walk the route (`playtest/sweep/go.py SESSION X,Y ...` walks waypoints and Auto Fights the
   battles on the way; doors, talks and menus by hand with `until`), `padlog off`.
3. Run autodiff; line up loads with `# skip:` / `# lag:` and fade-in ends with
   `# ignore: idle K` (playtest/README.md).

The original reads a late pad on the frame after a lag-merged frame (the frame starts before
the next NMI reads the joypad). Onett's streets lag even overclocked (enemies chasing), so a
port recording drifts a pixel from the original at the first pad change after one, and in a
long lag stretch (the arcade door, 3746-3801 in 02-to-03) the drift changes who touches whom.
`# late-pads` (autodiff.py) reruns the port on the pads the original actually read, computed
from the emu run. Since that changes what the port plays, record in chunks so later moves are
planned on those pads: `playtest/sweep/rec.py DIR check` (scenario = DIR/head.play + the padlog
chunks DIR/NNN.pad), add the skip/lag/ignore lines it needs to head.play, then
`rec.py DIR replay SESSION SAVE` restarts the session on the late pads and opens the next
chunk's padlog. `rec.py DIR build OUT.play` writes the scenario. Segment 01 predates this and
used `playtest/sweep/nudge.py` (moves such pad changes a frame later in the body itself),
which only works for isolated lag frames: iterated over a lag stretch it walks the route off
course.
4. Masks: every `# ignore:` is listed below with its reason. A mask on game state that lag
   can't explain is a divergence.

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
| 01-to-02-onett-hotel | identical | 2983 (2924 compared) | (this file's commit) | House phone → front door → down the hill past the library → Hotel Onett → lobby phone, Dad: Record, Continue. No battles. Masks: `idle 2315` (the hotel door's FADE_IN after the load lag). Lag/skip: 235 (front door CPU frame), skip 2285 / lag 2305 (hotel door load). Overclock on from 1000. 2 pad changes nudged |
| 02-to-03-mayor-key | identical | 17765 (17399 compared) | (this file's commit) | Hotel side door → street (Pogo Punk group) → arcade (Pogo Punk; doorway Shark, No: Yes Man Junior) → overworld Lifeup → Frank, Frankystein Mark II (Lifeup, Auto Fight) → arcade → town hall, Mayor Pirkle (key, Yes) → hotel phone, Dad: Record, Continue. late-pads (24 pads). Masks: `idle` 3590, 4950, 5955 (post-battle fade-ins), 3857, 10246, 11057, 12932, 14237, 15928, 16978 (door fade-ins); `focus 8030` (Frankystein's PSI menu: the original redraws the preview window over lag frames 8031-8032 and the 8030 sample catches the focus on it part-way). 9 door skip/lag pairs |
