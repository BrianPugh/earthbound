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
| 04-to-05-police-twoson | identical | 19216 (18096 compared) | 255d59d8 | Hotel side door → diagonal road → police station → front-desk lecture → officer, Yes → gauntlet (4 Cops + Captain Strong: Bash, Lifeup α below 85 HP; a first try with a 50 HP threshold died to Strong and was cut) → out of the station → Onett Town gate → roadblock cops → dirt path → map seam → Twoson → hotel phone, Dad: Record, Continue. late-pads (104). Masks: `idle` 13701, 13985, 16948 (door / seam fade-ins). 9 skip/lag pairs (doors, the gauntlet's TELEPORT_TOs, the seam) |
| 05-to-06-pencil-eraser | identical | 25278 (23014 compared) | a82d3567 | Hotel phone → street → diagonal roads to Apple Kid's house (Hamburger, invest Yes, $200 Yes; Receiver phone from the mouse) → main road east (2 battles) → southeast cave rooms → Peaceful Rest Valley, river west bank to the pencil statue (overworld Lifeup α first: a first take died to a valley battle at 55 HP and was cut; flag 111) → back to the cave mouth → cave rooms → Twoson (Apple Kid's call) → main road west (Mobile Sprout; 2 Ramblin' Evil Mushrooms met facing them, Rockin α + Bash: a first take got a surprise attack and lost on Auto Fight, cut) → Burglin Park, Apple Kid (Pencil eraser) → diagonal road north → hotel street → lobby phone, Dad: Record, Continue. late-pads (106). Masks: `idle` 7348, 9135, 10239, 11470, 13034, 16610, 18668, 20488 (post-battle fade-ins). 10 door skip/lag pairs. Route notes: valley waypoints are in the scenario's recording dir history (BFS from 4280,2262 to 4643,1409 on a flag-11 scout session); the 1st cave room's door needs `6 RIGHT; 12 UP; 8 LEFT` from 7438,10087 and the 2nd room's Twoson door `12 UP` from 6921,10072 (walking the go.py heading at the doors bounced the party through the door pair 7 times in the late-pads run); the park's west gap is UP at x ≤ 1152 from 1152,7137 |
| 06-to-07-valley-happy | identical | 34496 (31170 compared) | (this file's commit) | Hotel phone → street → diagonal road SW to the main road (Runaway Dogs + Cop) → east to the southeast cave (door: RIGHT from 3598,7104) → cave rooms (Magic butterfly; overworld Lifeup α; room 2's right door `14 UP; 8 RIGHT` from 7144,10074) → valley → west bank to the statue → Pencil eraser from 4643,1409 facing UP (the eraser stays in Goods in both) → log bridge east (BFS waypoints 4720,1352 … 5229,1280) → switchback (5208,1571 → 5189,1577 → 5158,1608 → 5104,1608: the plain LEFT heading sticks on a ledge at 5144,1576) → 5700,1176 → 5687,1336 → the Mobile Sprout ×2 + Territorial Oak group in front of the exit (Auto Fight lost there at PP 0, cut; `120 UP; 120 DOWN` then DOWN+RIGHT meets a lone Oak instead, Bashed) → exit cave door UP at 5960,1576 → cave RIGHT to 5367,2520, `5 LEFT; 30 UP` → Happy Happy → BFS waypoints 3767,7584 3790,7608 3743,7654 3736,7684 3736,7717 3965,7712 4130,7712 4136,7649, `12 LEFT; 30 UP` → drug store, `11 DOWN; 70 LEFT`, phone npc407: Dad, Record, Continue. late-pads (87); 4 chunks truncated where the late pads met (or missed) a battle or caught a corner. Masks: `idle` 4054, 7026, 9684, 11410, 15106, 21505, 24559, 26876, 31410, 32856 (post-battle fade-ins), 9849, 11735, 31746, 33020 (door fade-ins). 7 door skip/lag pairs |

## Where the next agent resumes

Pass 1, segment 07 -> 08: start a recording dir on `playtest/saves/07-InHappy.srm` (the Happy
Happy drug store phone).
