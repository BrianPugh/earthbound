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
  `# skip:` and the report use port frames but `# lag:` takes emu frames, so in a long segment
  K+20 lands one frame early per lag line before it (harmless while the party stands still in
  the fade, wrong once it walks: 09 -> 10's hotel door). `rec.py DIR emu-frame N` prints the emu
  frame for port frame N. Some loads need 2-6 more lag lines (09 -> 10's notes); when adding a
  pair doesn't converge, look at the emu's LSTEP numbers around the load instead of looping
  autoalign (it stacks a pair per iteration on a door the party bounces through).

## Pass 1 (started 2026-11-02 at adb66e8b): dirty (fixes 7b42f9ed, ce52a147 in segment 08 -> 09)

| Segment | Status | Logic frames | Commits | Notes |
|---|---|---|---|---|
| 01-to-02-onett-hotel | identical | 2983 (2924 compared) | 64453f54 | House phone → front door → down the hill past the library → Hotel Onett → lobby phone, Dad: Record, Continue. No battles. Masks: `idle 2315` (the hotel door's FADE_IN after the load lag). Lag/skip: 235 (front door CPU frame), skip 2285 / lag 2305 (hotel door load). Overclock on from 1000. 2 pad changes nudged |
| 02-to-03-mayor-key | identical | 17765 (17399 compared) | ac02f536 | Hotel side door → street (Pogo Punk group) → arcade (Pogo Punk; doorway Shark, No: Yes Man Junior) → overworld Lifeup → Frank, Frankystein Mark II (Lifeup, Auto Fight) → arcade → town hall, Mayor Pirkle (key, Yes) → hotel phone, Dad: Record, Continue. late-pads (26 pads). Masks: `idle` 3590, 4950, 5955 (post-battle fade-ins), 3857, 10246, 11057, 12932, 14237, 15928, 16978 (door fade-ins); `focus 8030` (Frankystein's PSI menu: the original redraws the preview window over lag frames 8031-8032 and the 8030 sample catches the focus on it part-way). 9 door skip/lag pairs |
| 03-to-04-giant-step | identical | 39109 (34916 compared) | 23f5c133 | Hotel side door → north past the library → shack (key) → cave behind it (3 levels, ropes; 2 overworld Lifeups) → Giant Step ledge → side cave → dungeon → Titanic Ant (Rockin α, Pizza, Hamburger, Bash; L10 after) → footprint melody → back down the cave → the cop at the exit (Yes) → shack → hotel phone, Dad: Record, Continue. No overclock (turning it on changed the original's lag and the late pads before the route's encounters). late-pads (155). Masks: `idle` ×22 (door fade-ins at skip+30 and post-battle fade-ins), 19 door skip/lag pairs. The rope top's enemies make the original lag every frame (`LSTEP mid`), so ~4000 frames there are uncompared |
| 04-to-05-police-twoson | identical | 19216 (18096 compared) | 255d59d8 | Hotel side door → diagonal road → police station → front-desk lecture → officer, Yes → gauntlet (4 Cops + Captain Strong: Bash, Lifeup α below 85 HP; a first try with a 50 HP threshold died to Strong and was cut) → out of the station → Onett Town gate → roadblock cops → dirt path → map seam → Twoson → hotel phone, Dad: Record, Continue. late-pads (104). Masks: `idle` 13701, 13985, 16948 (door / seam fade-ins). 9 skip/lag pairs (doors, the gauntlet's TELEPORT_TOs, the seam) |
| 05-to-06-pencil-eraser | identical | 25278 (23014 compared) | a82d3567 | Hotel phone → street → diagonal roads to Apple Kid's house (Hamburger, invest Yes, $200 Yes; Receiver phone from the mouse) → main road east (2 battles) → southeast cave rooms → Peaceful Rest Valley, river west bank to the pencil statue (overworld Lifeup α first: a first take died to a valley battle at 55 HP and was cut; flag 111) → back to the cave mouth → cave rooms → Twoson (Apple Kid's call) → main road west (Mobile Sprout; 2 Ramblin' Evil Mushrooms met facing them, Rockin α + Bash: a first take got a surprise attack and lost on Auto Fight, cut) → Burglin Park, Apple Kid (Pencil eraser) → diagonal road north → hotel street → lobby phone, Dad: Record, Continue. late-pads (106). Masks: `idle` 7348, 9135, 10239, 11470, 13034, 16610, 18668, 20488 (post-battle fade-ins). 10 door skip/lag pairs. Route notes: valley waypoints are in the scenario's recording dir history (BFS from 4280,2262 to 4643,1409 on a flag-11 scout session); the 1st cave room's door needs `6 RIGHT; 12 UP; 8 LEFT` from 7438,10087 and the 2nd room's Twoson door `12 UP` from 6921,10072 (walking the go.py heading at the doors bounced the party through the door pair 7 times in the late-pads run); the park's west gap is UP at x ≤ 1152 from 1152,7137 |
| 06-to-07-valley-happy | identical | 34496 (31170 compared) | 7c12e7b8 | Hotel phone → street → diagonal road SW to the main road (Runaway Dogs + Cop) → east to the southeast cave (door: RIGHT from 3598,7104) → cave rooms (Magic butterfly; overworld Lifeup α; room 2's right door `14 UP; 8 RIGHT` from 7144,10074) → valley → west bank to the statue → Pencil eraser from 4643,1409 facing UP (the eraser stays in Goods in both) → log bridge east (BFS waypoints 4720,1352 … 5229,1280) → switchback (5208,1571 → 5189,1577 → 5158,1608 → 5104,1608: the plain LEFT heading sticks on a ledge at 5144,1576) → 5700,1176 → 5687,1336 → the Mobile Sprout ×2 + Territorial Oak group in front of the exit (Auto Fight lost there at PP 0, cut; `120 UP; 120 DOWN` then DOWN+RIGHT meets a lone Oak instead, Bashed) → exit cave door UP at 5960,1576 → cave RIGHT to 5367,2520, `5 LEFT; 30 UP` → Happy Happy → BFS waypoints 3767,7584 3790,7608 3743,7654 3736,7684 3736,7717 3965,7712 4130,7712 4136,7649, `12 LEFT; 30 UP` → drug store, `11 DOWN; 70 LEFT`, phone npc407: Dad, Record, Continue. late-pads (87); 4 chunks truncated where the late pads met (or missed) a battle or caught a corner. Masks: `idle` 4054, 7026, 9684, 11410, 15106, 21505, 24559, 26876, 31410, 32856 (post-battle fade-ins), 9849, 11735, 31746, 33020 (door fade-ins). 7 door skip/lag pairs |
| 07-to-08-franklin-badge | identical | 14679 (13508 compared) | f629bc16 | Drug store phone → front door (`10 DOWN; 113 RIGHT; 8 UP`) → village door UP from 4033,7648 → cave LEFT (Insane Cultist) → valley top 5944,1104 → BFS waypoints 5920,1127 5821,1127 5816,1158 5800,1175 5833,1175 5824,1184 5791,1184 5784,1200 5751,1200 5760,1230 5783,1253 5807,1276 5807,1309 5833,1309 5833,1323 5814,1323, UP → cabin; `60 LEFT; 8 UP`, Paula npc436 (Franklin badge) → out the cabin (`8 DOWN; 100 RIGHT`, lands 5824,1232) → Pokey's ambush on the first steps (Insane Cultist ×2 + Spiteful Crow: Rockin α ×3, one turn lost to homesickness) → 5847,1288 5913,1288 5936,1265 5960,1241 5973,1214 5976,1182 5953,1159 5960,1129 5936,1105, UP → cave → RIGHT to the door at 4324,3400 → village → drug store (`12 LEFT; 30 UP` from 4136,7650) → phone: Dad, Record, Continue. late-pads; 1 chunk truncated where the late pads missed a battle. Masks: `idle` 7075 (post-battle), 346, 9386, 13382 (door fade-ins). 7 door skip/lag pairs (the drug store's interior door keeps the music, so its pair has no bgm change) |
| 08-to-09-carpainter | identical after 2 fixes | 17973 (16950 compared) | 7b42f9ed, ce52a147, (this file's commit) | Drug store phone → village (`# overclock-on: 200`: the village and HQ lag most frames, and at normal speed the late pads made the walk catch corners) → HQ door 3832,7896 (UP from 3840,7937) → crowd: talk npc380 from 8008,2368; `40 DOWN; 45 LEFT; 30 UP`, npc381; `16 UP`, npc382; `40 UP; 4 DOWN; 100 LEFT; 10 UP; 100 LEFT; 3 RIGHT; 20 DOWN`, npc384; `14 DOWN`, npc387 (Insane Cultist, Auto Fight); `14 DOWN; 60 LEFT; 40 DOWN+LEFT; 20 LEFT; 20 DOWN; 100 LEFT` (`12 UP; 60 LEFT` where it sticks), `40 UP+LEFT; 20 UP; 10 RIGHT; 150 UP+LEFT` → reception; `12 DOWN; 250 LEFT; 6 RIGHT; 40 UP` → Carpainter's room, `30 DOWN; 60 RIGHT`, x 7552, `100 UP`, talk npc376, No → battle (Rockin α ×5, the key) → room exit UP at 7468,1944 → reception door 8152,1744 (UP from 8156,1762) → hall, RIGHT along y 2320 to the door 8088,2320 → Pokey's "Hah! I lied!" → village (step DOWN past npc418) → cave → valley → cabin, `60 LEFT; 14 RIGHT` to 7001,9816, Key to the cabin (Paula joins at L1) → out (`8 DOWN; 100 RIGHT`, photographer) → cave → village → drug store, `11 DOWN; 70 LEFT; 2 UP` (npc407 at -16,-5: at -16,-8 Talk to misses) → Dad, Record, Continue. The cultist battle's homesickness roll found 2 bugs: ATTEMPT_HOMESICKNESS inflicted group 7 (a PSI shield) instead of 6 (7b42f9ed), and its RENDER_AND_DISABLE_ENTITIES frame was never presented (LOST-FRAME, the port ran a frame ahead; ce52a147). Masks: `idle` 16269 (post-battle), 3518, 3914, 8748, 9257, 11104, 11569, 12084, 13590, 16555, 17149 (door fade-ins). 14 door skip/lag pairs |
| 09-to-10-mondo-mole | identical | 69542 (60277 compared) | 5e49868a, (this file's commit) | Drug store phone → village → Lilliput Steps door (around npc421: `42 LEFT; 30 DOWN; 72 RIGHT; 20 UP; 20 RIGHT`) → spiral (leg 15 waypoints; a take where a Mighty Bear group knocked Paula out was cut) → overworld Lifeup α ×2 → Mondo Mole (Paula Shield α on herself and Ness, then Ness bash, Paula bash while the mole's PSI shield is up, Paula Fire α otherwise; a first take lost at L23/L16 to homesick turns and was cut; won L24/L17) → melody → spiral back → village → valley cave → Peaceful Rest Valley reversed (06→07's waypoints backwards; from 5648,1272 go 5688,1333 → `UP` → 5580,1176; the log-bridge strip run-18 teleported past walks: 5226,1248 5171,1248 5155,1256 5135,1264 5111,1288 5057,1288 5040,1300 5036,1320 5004,1331 4980,1350 4719,1350; west bank from 4584,1600 needs `4 DOWN; 20 DOWN+RIGHT`, the emu row's `30 LEFT; 100 DOWN` is a dead-end pocket) → Twoson cave rooms (`12 UP; 8 LEFT` from 7442,10082; `12 UP` from 6922,10072) → preschool (gate UP at x 2118 from the road; father's scene fires walking in, FLG_POLA) → Everdred's call → Burglin Park (y 7168 west, 1159,7141 → 1150,7093, `4 DOWN; 30 RIGHT` under the fence) → Everdred (Wad of bills) → npc329 from 2351,6521 (Backstage pass) → photographer → theater, pass on npc226 from 7576,7384, hall, npc231, the show → hall door back lands behind the counter: talk to npc226 ("Oh, pardon me") → office, manager npc253 from 7572,7616 facing RIGHT, Wad of bills → street, west along y 6549 (y 6553 sticks on the dog at 2217) → hotel phone: Dad, Record, Continue. late-pads (443); 4 chunks truncated or dropped where the late pads changed a battle or a walk. 0 lost frames. Masks: `idle` ×27 (door / post-battle fade-ins; `idle 57272-57275` after the theater door), `party 25323-25560` (Ness's HP meter rolling after the sanctuary heal: the roller phase shifts at the next battle-entry lag, known-original-behaviour rolling-meter row). 34 skips / 46 lags: doors as usual, but the preschool, Everdred's house, the theater lobby (both ways) and the backstage TELEPORT_TO spend more CPU-time frames in the original (LSTEP jumps during the load), so they carry 3 / 2 / 3 / 7 lag lines; Everdred's exit bounces through the door pair 5 times in both games |
| 10-to-11-winters | in progress (identical to the Threed jail, 18814 frames) | | Twoson hotel phone → street (truck at x≈1840: drop to y 6590) → band npc320 from 2221,6585 (from 6591 Talk to misses), Yes → bus `8 DOWN; 40 LEFT; 20 UP` → ride (2 TELEPORT skip/lag pairs) → Threed → street west (wanderer npc522 on y 9154: `14 UP`) → around the tree at 5297,9000 (x 5272) → road up-right → cemetery (No Good Fly + Smelly Ghost: Rockin α + Fire α ×2) → woods → gatekeepers' area at 5170,8392 (flag 46 fires walking in) → cemetery gate (`14 LEFT; 24 DOWN` from 5400,8648) → road down to the street → hotel (the girl walks in) → corridor door 7784,9272 → capture → jail. Next: the cell door (Paula's prayer), Jeff in Snow Wood |

## Where the next agent resumes

Pass 1 is dirty (2 fixes in 08 -> 09) but carries on: segment 10 -> 11 next, on
`playtest/saves/10-ToThreed.srm` (the Twoson hotel lobby phone; the Runaway Five bus to Threed is next). After the last segment,
restart at segment 1 for pass 2.
