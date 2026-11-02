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
3. `playtest/padlog_body.py FILE` is the body. Run autodiff; line up loads with `# skip:` /
   `# lag:` and fade-in ends with `# ignore: idle K` (playtest/README.md), and run
   `playtest/sweep/nudge.py SCENARIO` for walks through lag-heavy places: it moves a pad
   change that falls on the frame after an emu lag-merged frame one frame later (the original
   reads the previous pad there, so the walk drifts a pixel). That changes the inputs, not
   what is compared: both sides play the nudged body.
4. Masks: every `# ignore:` is listed below with its reason. A mask on game state that lag
   can't explain is a divergence.

Harness additions for the sweep: `# overclock-on: K [MODE]` (autodiff.py) turns the emu's
overclock on after port frame K, so the setup and the first frames (NPC phases from the boot)
run at normal speed and a lag-heavy walk after it runs overclocked.

## Pass 1 (started 2026-11-02 at adb66e8b)

| Segment | Status | Logic frames | Commits | Notes |
|---|---|---|---|---|
| 01-to-02-onett-hotel | identical | 2983 (2924 compared) | (this file's commit) | House phone → front door → down the hill past the library → Hotel Onett → lobby phone, Dad: Record, Continue. No battles. Masks: `idle 2315` (the hotel door's FADE_IN after the load lag). Lag/skip: 235 (front door CPU frame), skip 2285 / lag 2305 (hotel door load). Overclock on from 1000. 2 pad changes nudged |
