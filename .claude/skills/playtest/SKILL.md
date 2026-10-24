---
name: playtest
description: Play the C port in game to verify a change, reproduce a bug, or check a scene against the asm, using the headless playtest harness (playtest/pt.py). Use whenever a task says playtest, verify/check/reproduce in game, reach a scene, or measure in-game behaviour.
---

# Playtesting the C port

The harness is `playtest/pt.py` (interactive sessions) and `playtest/run.py` (batch). **`playtest/README.md` is the command reference; read it once before your first command.** This skill is the workflow, the shortcuts, and scene recipes earlier agents paid to discover. Use them before searching yourself.

## Setup

```zsh
cmake -S port/unix -B port/unix/build-NAME -DCMAKE_BUILD_TYPE=Release && cmake --build port/unix/build-NAME
pt(){ EB_BIN=$PWD/port/unix/build-NAME/earthbound uv run --no-sync python playtest/pt.py "$@"; }   # EB_BIN must be absolute; a PT="..." variable won't word-split in zsh
pt start NAME1 --save playtest/saves/NN-Name.srm
```

- Use your own build dir and session-name prefix; other agents may be playing at the same time.
- `pt stop` every session when you're done.
- Commit only your own paths, and do it quickly: the pre-commit hook stashes unstaged files while it runs. Never `git stash` yourself.

## Working loop

- Drive turn by turn: send a short batch with `pt do`, read the TEXT/STATE lines, and **Read the SHOT PNG**. Don't write long blind scripts; they waste reruns.
- For motion or effects, use `--sheet` with `wait N; shot` repeated to get a contact sheet. For exact numbers, run `state` every frame (`wait 1; state`, ×N) and diff the positions. For scroll and palette, `aux F1` dumps the PPU registers and CGRAM into the session `debug/` dir.
- Before experimenting, `snap here`, then `restore here` to retry. Snaps only work within the same process.
- `pt history NAME` turns a session into an exact repro script, since the game is deterministic.
- `pt video NAME [--skip-boot]` renders the session (finished or live) to an MP4 with audio and one chapter per `do`, for a person to watch.

## Talking, menus and saving tokens

- **To talk, open the menu and pick Talk to:** A opens the pause menu, and A again picks "Talk to", so the sequence is `press A; until input 60; press A`. Talk to reaches an NPC within |dy| ≤ 12 and |dx| ≤ 15, or |dy| ≤ 7 and |dx| ≤ 25 (NEAR offsets, 8×8 hitboxes), whichever way you face (all 4 directions are probed; this matches the asm). The easy way: walk into the NPC until you stop (dy ±8 or dx ±16, always in range), then talk. Shop clerks behind counters are talked to across the counter. If "Talk to" faces nothing, it may use a phone or object in front of you instead.
- **OBJECT NPCs need Check, not Talk to:** signs, statues and some story NPCs (e.g. Ness's Nightmare in Magicant) are type OBJECT. "Talk to" answers "Who are you talking to?" for them; use Check (`press L`), standing directly in front.
- **Close menus with `press B; wait 30`:** `until idle … advance` never closes a menu by itself. B on the plain overworld toggles the HP/PP + money window (mode `HPPP_DISPLAY`, `idle=modes`), and another B closes it. That is normal game behaviour.
- **Horizontal menus** (Buy/Sell, Yes/No) move with LEFT/RIGHT, not DOWN.
- **Use `--quiet` (and `--no-shot` when you don't need to look) while walking or lining up.** Full TEXT/STATE/PARTY/NEAR output on every step is most of an agent's context. Run `state`/`text` or a plain `do` when you need them.

## Finding a scene

1. `playtest/saves/INDEX` lists the 36 saves by story point. Pick the closest one.
2. Find the script: grep `asm/data/events/` and the extracted text (`src/assets/` text yml / `MSG_*` labels) for the event or message, then find what spawns it (an NPC's text pointer, a door, a hotspot).
3. Get coordinates and flags from the NPC config / map data (`NpcConfig` has the event flag and script). `NEAR` in STATE shows npc ids with offsets once you're close.
4. **When playtesting a stretch of the story, walk there from the save** (routes, doors and the battles on the way are part of the test). `teleport X Y [DIR]` (DIR 0 = up, clockwise; 2 = right, 4 = down) is for checking one specific scene, recovering when stuck, or re-checking a suspected bug; say in reports where you used it. Use `flag N 0|1` to put the story state into shape. Talk with `press A` facing the NPC.

## Shortcuts

- **`debug on`** turns on debug mode without opening the menu.
  - In battle, SELECT+START wins instantly, A cycles PSI and B cycles the swirl.
  - On the overworld, hold **B+R** to open the debug menu. Page with the "..." entry. Items: 1 Flag, 2 Goods, 3 Save, 7 Event, 8 Warp, 10 Teleport, 17 CAST, 19 **STAFF** (jumps straight to the staff roll and photo credits).
- `give ITEM [CHAR]`, `hp`/`pp`/`heal`, `status CHAR GROUP VAL` (e.g. `status 2 2 2` possesses Paula), `money`. In battle these also update the battler.
- `warp TOWN` goes to a PSI Teleport town. `flag 11 1` keeps overworld enemies away while you set up a scene.
- `until COND MAX advance` reads through dialogue without picking menu options. `until said:WORD` waits for specific text.

## Gotchas (each one cost an agent a run)

- **Area triggers are strict:** `TEST_PLAYER_IN_AREA` passes only for |d| < radius, so standing exactly on the edge fails. Approach from the side the script expects. The Runaway Five bus, for example, only boards from below.
- **Walk away and NPCs despawn:** roughly 300+ px off-screen, tasks like EVENT_8's free the entity. When it respawns it has its *default* script, so any armed script is lost. Re-talk to re-arm it.
- **Budget frames realistically:** the bus ride is ~5000 frames, the Magicant exit montage ~1100, and the credits ~20000. A `TIMEOUT` mid-cutscene is usually not a hang. Check STATE `modes`/`idle` and the screenshot before you call it a bug.
- **`until` only sees text printed while it runs:** in `press A; wait 1; until said:Ready`, "@Rea" already printed during the press, so it times out. Put the `until` right after the action that starts the text, and wait for a word further along (`press A; until said:Voy 600 advance`).
- **Holding a direction through a cutscene** (`until text 600 UP`) keeps the button held inside it and can disturb scripts. Use the default `-` pattern to wait.
- **Rolling HP meter:** "mortal damage" sets the target HP to 0, but the meter rolls down over time. If the battle ends first (the last enemy dies, or a Smilin' Sphere explodes and ends it), each meter stops where it is and the character survives with that HP. Several enemy actions can still hit a member whose meter is still rolling. Both are original EarthBound behaviour. Compare the printed damage with the meter only while the battle is still running.
- **`music=` in STATE is the map track** (`ml.current_map_music_track`). It doesn't change in battle, so battle music can't be checked from it.
- **`debug on` + SELECT+START wins skip the victory text**, exp and drops. Play a battle out (Auto Fight: `press RIGHT 2; press A; until idle 6000 advance`) when you need those.
- **Doors locked by story flags do nothing:** a door whose flag isn't set runs its text (if any) and stays shut, e.g. the Monotoli lobby door needs flag 143. Check the door's event flag before reporting a dead door.
- **Deep Darkness is meant to be mostly black** until the Hawk Eye is used (flag 288); only some blocks stay lit.
- **Towns have no overworld enemies, and spawning needs movement.** Enemies only spawn when the camera crosses a 64 px boundary, only from tiles with encounter placements, and ones placed far off screen despawn within frames (all asm behaviour). Walk back and forth on a route, cave or dungeon; `teleport`/`warp` clear enemies first.
- **Finding an NPC's coordinates:** look up its id in `sprite_placement.json` through the sector pointer table: `pt[(sy*32+sx)*2] - 0x6BE7` indexes `placement_data`, entries are (id16, y8, x8), world x = `sx*256 + x8` (y likewise with the sector row).
- **Menus can drop an early press:** `until input` / `in:SELECTION_MENU` can fire while a menu still prints its last item; add `wait 10` before the next press.
- **Game over menu:** a No to "another shot?" asks "...Is that okay?" with the cursor on No; a Yes there goes to the title screen and loses everything since the last Dad save. Never chain `press RIGHT`/`press A` battle steps that may land in a game over, and don't `snap` blindly after a fight that may have been lost.
- **Enemy count:** `ow_enemies=65535`/`65534` after `teleport` or a door is the asm's own wraparound, and spawning still works. Don't report it.
- **Blinking party:** `blink=N` after a battle means the party sprites blink, so a shot may miss them. Use `wait 1; shot`.
- **Before calling something a port bug, rule out how you walked.** Check the asm's conditions, such as the area box, flags, and facing, against what you actually did.
- **Comment confirmed asm behaviour:** if behaviour looks wrong but matches the asm, the C site may already have a comment saying so. Read it. If you confirm a new one, add such a comment.

## Regression check

`playtest/regress.py` replays ~30 short cases (`playtest/regress/cases/*.play`: a save plus an input script with `check LABEL` checkpoints) covering shared systems: file select and naming, towns, doors, talk and Yes/No, shops, pause menus, phone and Escargo, swirls, battles, game over, PSI Teleport, sanctuaries, Lumine Hall, Deep Darkness, Magicant, credits, attract mode, savestates. Each checkpoint records STATE (minus `frame=`), PARTY, ENEMIES, window text, the TEXT printed since the last one, `until` met/TIMEOUT, and a screenshot. Hangs (60 s wall clock), crashes and missing checkpoints are reported.

```zsh
uv run --no-sync python playtest/regress.py compare port/unix/build-old/earthbound port/unix/build-new/earthbound   # the main use
uv run --no-sync python playtest/regress.py record NAME --bin BIN; uv run --no-sync python playtest/regress.py check NAME --bin BIN2
uv run --no-sync python playtest/regress.py diff RUN_DIR_A RUN_DIR_B   # re-diff finished runs; -k CASE narrows any command
```

- **Fix agents: before committing, build the pre-fix tree (`git worktree add` into your scratch dir; symlink `asm/bin`, `src/assets` and `src/vendor/tamp` from the main checkout) and the post-fix tree, both wide-viewport, run `compare`, and explain every diff in the report and commit message.** Read each diff PNG (`playtest/out/regress/diff/A-vs-B/CASE--LABEL.png`: A, B, changed pixels in red with the bbox). If unsure whether a diff is right, check the scene in emu mode.
- Checkpoints line up by label, so a timing shift only adds a `timing:` note; everything after a real divergence (a menu cursor, an extra prompt) differs too, so explain the first diff of each case.
- New case: copy one, keep it short (a few thousand frames), put an event `until` (`battle`, `in:MODE`, `idle`, `said:WORD`) before every `check`, and run `record` on it until every `until` is `met`.

## Checking the original

When a suspected divergence comes up (port behaviour that might or might not be the original's), reproduce the scene in the original game with emu mode and compare. Setup and the differences are in `playtest/README.md` ("The original game"); once: `playtest/emu/build.sh`, `uv pip install numpy`, `make`.

- Start an emu session from the same save: `pt start o1 --emu --save playtest/saves/NN-Name.srm`, then send the same steps you sent the port session (`teleport`, `warp`, `flag`, walking, `press`, `until idle/input/battle/text/in:/out:`, `snap`/`restore` all work).
- Compare scene by scene, not frame by frame: lag, RNG and the intro length differ, so frame numbers, enemy spawns and NPC wander positions won't line up. Line up on events instead (`until input`, `until in:TEXT_PROMPT`, `until battle`), then compare the screenshots (window position and size, sprites, palette, layering) and STATE (pos, music, money, HP/PP, battle).
- Emu window text isn't decoded (no TEXT lines, no `until said:`): read the shot. `modes=` is the asm call chain, which also tells you which asm routine the original is in at that moment.
- Emu shots are 256 px wide; the port's are 320, so the same thing sits 32 px further right in the port.
- The port is an extended-viewport build, so pixel differences from the original are expected and fine. Judge the port by whether things look good and extend well into the wider view: no off-centre scenes, objects or windows stranded in the gutters, empty or garbage gutters, or effects confined to the 256-px band when they should cover the screen. Report a pixel difference inside the SNES band only when it's visibly wrong (e.g. a misaligned layer), not merely different.
- Say in the report which side you checked in emu mode, with both shot paths.

## Scene recipes (verified 2026-10-22)

| Scene | Save | Steps |
|---|---|---|
| Possession mini ghost | 24-Museum | `status 2 2 2`, walk; FOLLOW `ghost:` orbits about ±48 px |
| Surprise attack, both ways | 17-GoldMine | `teleport 3424 9312`, walk LEFT into the Cute Li'l UFO (party first). An enemy touching you from behind gives the enemy-first case |
| Bomb splash | 17-GoldMine | from the UFO battle: `give 147 1`, throw it at the middle enemy |
| Jeff overnight repair | 17-GoldMine | give Jeff a broken item with IQ requirement ≤ his IQ, `flag 696 0` (100% chance), sleep at the desert cabin (npc752) |
| Level-3 attacks | 30-DeepDark | `teleport 5600 7712`: Zap Eel + Hard Crocodile (bite, tail swing) |
| Thunder and Storm (level-4 INTERTWINE) | 26-Dalaam | `teleport 2080 5530 0`, talk to npc1118; INTERTWINE comes about enemy turn 3 |
| Mu training (EVENT_340 fade) | 26-Dalaam | `flag 16 0; flag 273 0`; `teleport 6824 8318 0`, talk to npc1089; `teleport 4988 3840 0`, walk UP; answer Yes ×4 |
| Mu spirit fight → Poo joins at Summers beach (EVENT_342/345) | 26-Dalaam | Mu training above with `10 UP` before talking and `until in:SELECTION_MENU 4000 advance; wait 10; press A` ×4, `until idle 4000 advance` ×2; then `teleport 6841 8296 0`, talk to npc1089, `until said:might 5000 advance; until in:TELEPORT_TO 1000 advance` (emu: `until in:TELEPORT`); Poo dashes in from the left ~250 frames later |
| Paula's prayer fade-back (EVENT_766 $32,$3C) | 21-Monotoli | set flags 15, 318 and 672 (and 143 if the door stays shut), then leave Monotoli Building through the door at 5648,5984 |
| Monotoli backs away (reversed move-toward) | 21-Monotoli | teleport to 5656,5984, then walk right out of the area |
| Fake Monotoli dissolve (DELETE_TPT style 9) | 19-Moonside | `flag 11 1`, `teleport 7604 4714 2`, talk to npc1395 |
| Topolla doorman (style 6 delete) | 19-Moonside | `teleport 4504 5876 0`, talk to npc840 |
| Magicant: Nightmare → Sanctuary montage → wakeup | 34-Magicant | `teleport 496 9140 0`, `press L` (Check; the Nightmare is an OBJECT, only y=9140 works), `until battle 3000 advance`; then instant-win, `until said:absorbed`. The montage runs ~1100 frames |
| Lier into his hole (reversed move-toward) | 03-GiantStep | `teleport 7508 1236 2`, talk to npc119 |
| Runaway Five bus to Threed | 10-ToThreed | talk to the band at 2224,6584, Yes, then `8 DOWN; 40 LEFT; 20 UP` into the strip under the bus; ride ~5000 frames |
| Twoson–Threed tunnel ghosts (move-toward) | 10-ToThreed | walk into the tunnel; the ghosts follow and warp you back to Twoson |
| Summers sailor to Scaraba (diagonal velocity) | 27-Kraken | `teleport 7264 2908`, talk to npc1065, pay; sailor walks DOWN_LEFT at 0.354 px/frame/axis |
| Credits photo slides | any (36-CavePast) | `debug on`; set flags 698-729 (photos); B+R → page to STAFF → A |
| Fire Spring entry + quake | 33-FireSpring | `until text 120 UP; 30 DOWN; 160 LEFT` → 2000,3221 (EVENT_672/674 quake fires on a 400-1800 frame random timer here); cave door 1880,3096: `60 UP; 180 UP+LEFT; 120 LEFT; 50 LEFT; 100 UP` → 2872,6792 |
| Carbon Dog / Diamond Dog | 33-FireSpring | `teleport 3344 6410 0; 30 UP`, talk to npc1276; Auto Fight wins in ~13000 frames with 4 members |
| Fire Spring sanctuary | 33-FireSpring (after boss) | `until in:DOOR_TRANSITION 120 UP` → 6352,9880; `30 DOWN; until text 100 DOWN+LEFT; until text 100 LEFT; until text 200 UP` → hotspot 31 ~6128,9831; melody ~1300 frames |
| Magicant: Sea of Eden entrance | 34-Magicant | bush maze NW of town traps you; from `teleport 5760 4690 6`: `8 DOWN; 110 LEFT; 200 UP; until battle 250 LEFT` (enemy NPC), `16 DOWN; 40 LEFT; 150 DOWN+LEFT; 60 LEFT; 200 UP; 30 LEFT; 200 UP; 20 DOWN; 200 RIGHT` (wounded memory), `10 DOWN; 200 RIGHT; 30 RIGHT; 250 UP+RIGHT; 150 UP; 250 RIGHT; 250 UP`, pink maze to 5556,3904, `10 LEFT; press L` → 432,9584 |
| Sea of Eden Krakens → Nightmare | 34-Magicant | from 432,9584: `until text 200 RIGHT; until text 150 DOWN; until text 300 RIGHT` ×2 (Kraken 1), `30 RIGHT; until text 300 UP` ×2, `20 LEFT; until text 200 UP; until text 300 LEFT\|UP` (Kraken 2), `until text 300 UP; until text 300 LEFT; 80 DOWN; until battle 300 LEFT` (Kraken 3), `until text 105 LEFT; until text 60 UP; 20 DOWN; 44 LEFT; until text 60 UP` → 496,9137, `press L`. Nightmare is slow on Auto Fight; debug SELECT+START wins |
| Debug menu in emu | any | the original's cursor redraw lags one item and labels are off: "STONE" runs CAST, the third garbled row below CAST runs STAFF. `press DOWN 5; wait 40; press A; wait 60` for STAFF (port: `press UP 2`) |
| Meteorite piece | 35-Onett | `flag 11 1; teleport 2075 128 4`, `press L` (npc198 at 2080,144), Yes |
| Andonuts takes the Meteorite piece | 35-Onett (with piece) | `warp 4; 20 RIGHT; 30 UP` to 387,7656 facing UP (npc728), Goods → Ness → Meteorite piece → Use |
| Saturn Valley hotel sleep (clears flag 169) | 35-Onett | door 216,7768 → 6488,8008; `24 DOWN; 10 UP; 20 LEFT` to 6471,8032, talk to npc710, Yes |
| Phase Distorter 2 → Lost Underworld | 35-Onett (flag 134 set, 169 clear) | talk to Andonuts from 387,7657 facing UP, No (sets FLG_DOSEI_INFO_EQUIP), talk again, Yes; `20 RIGHT; 30 UP` into npc741; arrival 5488,340 (~700 frames) |
| PD3 → Cave of the Past | after PD2 | walk RIGHT to the Starman (Poo Starstorm), into hotspot 8 (5904-5952,216-240) from 5912,217; Apple Kid npc1271 from 5935,251 facing RIGHT (flag 285); robot Andonuts npc1270 from 5776,180 facing LEFT; Yes ×3; arrival 592,6964 after ~4000 frames |
| Cave of the Past doors | 36-CavePast | door pairs 416,6360↔896,7360 and 672,5976↔864,7616; final door 943 at 192,6040 (from 199,6070: `10 LEFT; 40 UP`) → lair 4240,2032 |
| Post-Giygas → Mom → ENDING | after the Giygas win | farewells auto-run (~11000 frames), escort Paula (Yes), `warp 2`, walk to 2044,7103 (Polestar); PSI Teleport Onett, `teleport 2640 440 0; until text 200 UP` (house door 2648,336), `20 DOWN; until text 200 LEFT; 10 UP; 50 RIGHT` to 7576,344, talk to Mom npc14 twice, Yes |
| Debug CAST | 36-CavePast | `debug on`, B+R, `press DOWN 5`, A ×3, `press UP 5; press DOWN`, A |
| Dalaam different-tileset sector | 36-CavePast | `flag 11 1; teleport 4448 3480 4` |
| Damage screen shake | 06-ToHappy | `teleport 6112 7776`, fight Zap Eel + Even Slimier Little Pile; a mortal hit shakes ~60 frames plus 10-frame holds |

When you reach a new scene that took effort to find, **add a row here** (save, exact steps) in the same commit as your work.

### Added from the 2026-10-23 playthrough

| Scene | Save | Steps |
|---|---|---|
| Onett street battle (Spiteful Crow) | 01-Start | leave the house by the east door, `teleport 2660 440 4`, `until battle 500 DOWN\|RIGHT\|LEFT` |
| Dad's phone call (Record, Continue) | 02-Sharks | the save starts by the hotel phone: `press A; until input 300; press A`, Dad is the first entry |
| Game over → Yes (money halved, revive at the phone) | 06-ToHappy | `teleport 6112 7776`, lose to the Zap Eel + Even Slimier Little Pile, answer Yes; revive at 7556,7248 |
| Happy Happy drug store shop | 07-InHappy | `48 RIGHT; 20 UP` from the start (`8 RIGHT; 2 UP` reaches the phone), then talk across the counter; Buy/Sell is horizontal |
| Leave Happy Happy drug store / reach east cave | 07-InHappy | `120 RIGHT; 20 DOWN; 20 UP; 30 RIGHT` exits to 4120,7616; `teleport 4184 7730 4; 30 RIGHT; 20 UP; 20 RIGHT` reaches the east cave (npc421 blocks the cliff path) |
| Runaway Five bus via the band | 10-ToThreed | `teleport 2224 6600 0; 8 UP`, talk, Yes, `8 DOWN; 40 LEFT; 20 UP`, then `until idle 4000 advance` ×2 (about 7000 frames) |
| Desert battle, 3-member party | 12-Zombies | `teleport 4000 9300 4; until battle 900 DOWN\|DOWN\|LEFT` |
| Threed hotel phone | 12-Zombies | at the start, talk to the phone in front; Dad is first |
| Desert cabin shop (buy, equip, sell old) | 15-Desert | inside the cabin, about 12 px LEFT of the start to face the clerk, then talk |
| Desert enemies (Skelpion, Booka, Smilin' Sphere) | 20-TarahRama / 24-Museum | `teleport 1400 9400 4; wait 60; until battle 2500 LEFT\|UP\|RIGHT\|DOWN` |
| Scaraba desert enemies (Octobot, UFOs) | 28-Scaraba | `teleport 1400 5600 4; until battle 1500 RIGHT\|DOWN\|LEFT\|UP` |
| Dungeon Man robots | 29-DungeonM | from the start, `until battle 1500 RIGHT\|RIGHT\|UP\|RIGHT\|DOWN` |
| Monotoli lobby door | 21-Monotoli | `flag 143 1; teleport 5656 5984 0; 40 UP` |
| Giygas prayer phase (speech 2 → Pray) | 36-CavePast | Giygas recipe below to the 2000-HP stage, then each round `heal; status 2 1 0; status 3 1 0; status 4 1 0` and everyone attacks (~7 rounds); Pray is `press RIGHT; press RIGHT; press A` in Paula's menu. Fastest: `pt.py replay p6g NEW` if that session still exists |
| Threed zombies | 12-Zombies | `debug on; teleport 5894 8704 6` (road south of the cemetery); `100 LEFT` ×3 then `100 RIGHT` ×6, repeat; ~1 spawn per 100–150 frames. Not at the `warp threed` point |
| Plague Rat → Magnet Hill melody | 25-Sewer | `flag 11 1; teleport 3552 3240 0; 20 UP`, talk to npc995 (dy about −12), win; then `teleport 3840 4152 6; until text 300 LEFT` |
| Pink Cloud melody | 26-Dalaam | after Thunder and Storm: `teleport 2072 5512 6; until text 300 LEFT` |
| Kraken voyage | 27-Kraken | `teleport 7264 2940 0`, talk to npc1065, Yes, `until said:Kraken 3000 advance`; after the battle `until idle 4000 advance` lands in Scaraba |
| Scaraba arms dealer | 28-Scaraba | `teleport 7512 7016 4; 14 LEFT; 30 UP`, talk to npc1125 |
| Escargo Express delivery | 29-DungeonM | talk to the phone at the start, Escargo → Delivery → item; courier ~600 frames later, $18 |
| Submarine → Deep Darkness | 30-DeepDark | `flag 11 1; teleport 4168 120 0; 20 UP`, Check npc1197; `until idle 3000 advance` (~3300 frames) |
| Hawk Eye lighting fade (flag 288) | 30-DeepDark | after the submarine, `teleport 6152 7598 7`; Paula's Goods, Hawk eye is item 12 (`press DOWN 5; press RIGHT`), Use. Only works in sectors whose per-sector item is HAWK_EYE (x<5376 for y 6912-7551, x<6400 for y≥7552); the arrival lawn says "can't be used here". The arrival basin (5480-6000, 7100-7450) is closed off: leave it east along y≈7280, then south at x≈6100-6200 |
| Master Barf (Poo's Starstorm entrance) | 30-DeepDark | Hawk Eye first, then `teleport 5096 7618 0`, talk to npc1229 (dx -8, dy -10); Auto Fight, Poo swoops in after ~5000 frames |
| Tenda Village shy Tendas / elder | 30-DeepDark | the cave door is at 4502,7049 (walk UP); elder npc1252 at 464,336, innkeeper npc1251 at 464,112 |
| Apple Kid kidnapped call (door 904) | 30-DeepDark | in Tenda Village walk to 664,106 then RIGHT into the doorway; the call plays on arrival outside (flag 379 not set) |
| Dr. Andonuts' lab, Eraser Eraser | any Winters save | `teleport 320 4930 0; until text 200 UP` enters the lab; mouse npc616 at 8101,2872 (talk from 8101,2872 facing UP) |
| Enter Moonside (cafe bottles) | 19-Moonside | `teleport 5520 6584 0; 10 UP; press L` |
| Moonside → Evil Mani-Mani | 19-Moonside | `teleport 5490 4458 2`, talk, No; `teleport 7720 4616 0; 10 DOWN; 30 UP`, L; `teleport 7624 4730 0; 20 UP`, L |
| Talah Rama gift (slot-14 item name) | 20-TarahRama | `teleport 7984 5716 0`, use the Pencil eraser, `40 UP`; drop 1 item; `teleport 7568 6811 6`, L |
| Monotoli confession, Paula rejoins | 21-Monotoli | `teleport 5632 6040 0` (Stop! scene), then `teleport 5620 6010 2`, L |
| Shrooom! → Rainy Circle | 22-RainyCircle | `teleport 5128 186 0; 20 UP`, L; after the win `until text 600 UP+RIGHT\|RIGHT\|UP\|UP+RIGHT` |
| Summers museum Omar guard | 24-Museum | `teleport 7528 9550 0; 10 UP`, L, Yes; `until text 400 UP` into the hieroglyph room |
| 4-member battle | 24-Museum | `teleport 5128 216 0`, walk LEFT out of the cave: Mighty Bear Seven + Cave Boy |
| Giygas final battle (Pokey, redirect, speeches 1–2) | 36-CavePast | `flag 11 1; teleport 192 6056 0; 30 UP` (door 943 → lair, hotspot 14); `teleport 4224 1000 0; wait 20; 60 UP; until in:BATTLE 6000 advance; wait 150; until input 900 advance`. Hits on the 9999-HP Giygas are redirected onto the party, so target Pokey (`press A; press DOWN; press A`) |
| Sphinx star → pyramid opens | 28-Scaraba | stepping plates are doors 775-779 (Sphinx text 774 at 1792,4960). Step plate 1 (1792,5000) once for the Sphinx text (sets flag 152), step off and on again, then plates 2 (1744,5088), 3 (1848,5032), 4 (1728,5032), 5 (1832,5088), then 1; flags 2-6 track progress. Pyramid door ~1792,4884 |
| Pyramid route (Guardian General, Hawk eye) | 28-Scaraba | doors in order: 786 → 788 (560,1352) → 790 (4272,4552) → stairs → 791 → casket room → 794 (976,3688) → stairs → 801 → 803 (4272,5064) → stairs → boss npc1180 at 4296,8944 (Check) → 805 → switch at 4136,5352 (flag 347) → back to casket room, hole 795 (892,3700) → Hawk eye (Check at 4208,5592) → 807 → 809 → hole 811 → 812 → 814 exits at 1728,5472. Stairs need the direction held (`until idle 900 RIGHT`). Dump doors with `door_config_table`/`door_pointer_table` |
| Dungeon Man entry | 28-Scaraba | after the pyramid: Star Master takes Poo at 1726,5514; Yariman npc1149 (1624,5408) gives the key (needs a free slot); use it facing npc1150 from 1346,4992 → inside at 4552,912 |
| Tenda Village → Lumine Hall cave (rope hole) | 32-LumineHall | `16 RIGHT; 100 UP` out of the hollow, then west/south to 136,392 and `20 RIGHT` into the hole (doors 152/160,392) → rope at 1184,2576; talking rock npc1260 below; doorway 1064,2696 (walk UP) → cave at 4008,912 |
| Lumine cave maze → Electro Specter | 32-LumineHall | upper cave pits drop to a lower maze with ropes back up; route: from 3680,672 west/north to the pit at 3864,552 → right room → maze → rope 3848,2104 → top at 3856,128 → west; boss npc1246 (sparkle) at 3504,200, Talk to from 3522,208 |
| Lumine Hall sanctuary (wall text) | 32-LumineHall | after the boss: `30 UP; 200 LEFT` to 3222,141, then hole 3160,136 → 7320,4792; `until text 300 LEFT` (stops at 7160,4791), `30 UP; until in:PROCESS_INTERACTION 600 LEFT` into hotspot 30 (6576-6600,4792-4816; armed only by arriving through the hole). Fast: `flag 196 1; teleport 3222 141; 60 LEFT`. Wall text + flashes ≈ 4200 frames, `until idle 6000 advance`. Floor hole to the Lost Underworld at 6440,4808 → 2864,3080 |
| Lost Underworld Tenda camp / Fire Spring | 33-FireSpring | camp gate ~2343,2688 (Tenda boss "dinosaur cage" text); Fire Spring entrance 2072,3222 |
| Lost Underworld rocks / geysers | 32-LumineHall | talking rock npc1292 at 2368,2608 (camp); rocks 1247 (3912,2664), 1248 (3368,2872), 1249 (3232,2504); geysers npc1303 2672,2992 (`teleport 2705 2978 0`), npc1305 2160,3184, npc1304 2320,2480. Party walks 0.5 px/frame here |
| Stonehenge entry → Eraser eraser | 31-StonehBase | lab exit is the arched door at ~8146,2883 (walk UP); outside, go LEFT to x≈180 then UP between the ledge and the lab to Stonehenge (~340,4450). Its centre (336,4440) is a ladder hole → cave 7016,3660; exit right (7120,3744) → stairs (hold RIGHT) → zigzag. Eraser npc671 at 6488,6064: Use the item from 6492,6056 (reach it via the band's right end: `10 DOWN; 10 RIGHT; 20 DOWN; 20 LEFT`); Check/Use from above says nothing |
| Stonehenge Base holes → Starman Deluxe | 31-StonehBase | after the eraser: zigzag bottom hole 6400,6376 → 8160,4904 → lower room hole 7680,5096 → 7648,5416 → hole 8176,5608 → 6672,5256 → hole 6144,5544 → 6720,6728 → hole 7408,6888 → 6240,3672 → hole 6896,4296 → capsule room 7704,6120 → hole 7920,6120 → boss room; talk to npc672 from 8064,4720. Diagonal corridors are hard to walk; `teleport 7704 6128 0` gets to the capsules |
| Tenda shyness book → player-name prompt | 31-StonehBase (after the boss) | book is Check on the Onett library back-room shelf at 6984,1352 facing UP (door 7880,440 in the library); in Tenda Village use the book (Jeff's Goods) facing elder npc1252 from 465,344; photo man → name entry (TEXT_INPUT) |
| Ness's house front door from inside | 01-Start / run-02 | stand at 7768,335 and walk RIGHT into the door tile 7800,336 (UP from the doorway is unreliable) |
| Hotel Onett ATM, stay, phone | 02-Sharks / run-03 | door 2040,1416 (UP); side door 8072,1464 (UP) → ATM at 7216,1088, Check facing LEFT from 7236,1091; clerk from 8038,1488 facing UP ($35); phone from 7998,1488 facing UP. After the stay exit the room by its right diagonal-wall door (8175,1631) → hallway, then RIGHT into the arch at the right end → lobby |
| Arcade Sharks → Frank / Frankystein Mark II | run-03 | from the hotel walk to 1588,1756, UP into the arcade (→ 7976,712); walking to 7820,696 triggers the Pogo Punk fight; talk to the doorway Shark, No (Yes Man Junior fight); UP through the door → 1592,1624, talk to Frank npc146. L5 loses to Frankystein; bring L7+ |
| New game from a blank SRAM | none | `head -c 8192 /dev/zero > blank.srm`; `pt start NAME --save blank.srm --no-boot`; `300 -; press START` (title), then `press START; wait 60; press START; wait 60; press A` (attract → file 1 → text-speed menu); A ×3 (Fast, Stereo, Plain); each name: `press A; wait 30; press DOWN 5; wait 6; press A; wait 30; press RIGHT 2; wait 6; press A; wait 40` (Don't Care → OK); Yep, `until idle 6000 advance` wakes in the bedroom (~8100 frames) |
| Intro night: Pokey at the barricade → Mom → Pokey's knock | run-01 (or new game) | bedroom → hallway: `until text 300 LEFT`; stairs door is 7432,1000 (`4 LEFT; UP` from 7440); front door 7800,336. Barricade Pokey npc161 talked to from 2120,248 facing RIGHT; Mom npc148 blocks the front door (talk from 2652,352), Yes → bed; walk out to the hallway (music 116 knock), Pokey npc17 by the front door; Mom npc14 → change clothes |
| Leave home at night with Pokey (door 467) | run-01 | after Pokey joins ("I'll follow at a safe distance"), the front door stays shut (FLG_MYHOME_DOOR_CLOSE 467, same in emu) until Dad's first call: talk to phone npc21 from 7658,328 facing UP. Then `20 DOWN; 170 RIGHT; 40 UP+LEFT; 30 RIGHT` exits to 2656,344 |
| Night walk: house → meteorite → Picky → Buzz Buzz | run-01 (after the call) | long western loop, all walkable: 2640,400 → 2554,426 → 2265,423 → 2243,616 → 2184,550 → 2100,508 → 2045,472 → 1964,476 → 1916,516 → 1798,498 → 1640,498 → 1564,444 → 1532,356 → 1532,332 → 1596,316 → 1660,316 → 1756,372 → 1756,252 → 1820,188 → 1948,188 → 2044,268 → 2108,268; pass the barricade on its right (2165,232 then UP to 2169,184), 2180,196 → 2220,156 → 2220,140 → 2188,124 → 2116,124 → 2116,148 → 2036,156 → 2012,124; Picky npc124 facing UP. Walking back east triggers Buzz Buzz (Yes, Yes, Yes) |
| Starman Junior | after Buzz Buzz joins | hotspot 43 = x 2488-2624, y 520-528 on the slope south of Ness's house (reverse the loop above, or walk 2436,604 → 2532,508); Auto Fight wins at L2 |
| Lardna swats Buzz Buzz → Sound Stone | after Starman Jr | Pokey's door 2392,344 (walk UP from 2396,356); inside, `12 UP; 70 LEFT` to 7050,856 and talk to Aloysius npc44 (facing LEFT); then `16 DOWN; 40 RIGHT` to 7099,878, talk to Buzz Buzz npc46; Yes, then No. Exit door 7376,872 |
| Ness's house phone (day, around the table) | run-02 | from left of the dining table (about 7554,321; Mom and the table block the direct way): `40 LEFT; 50 DOWN; 140 RIGHT; 60 UP; 8 DOWN; 22 LEFT` to 7654,328, face UP, talk (npc21) |
| Cracked bat (Tracy's room present) | run-01 | Tracy's door 7560,984 (`UP` from x 7564 in the hallway); present npc1412 at 7464,744: stand at 7461,736, `press DOWN; press L`. King npc16 in the living room, talk, Yes |
| Winding path → mist → Saturn Valley | 13-Undergrnd | the overworld door 4232,2040 only fires walking DOWN from the pink winding path above it: `flag 11 1; teleport 4232 2020 4; 20 DOWN` → mist 200,6048; mist door 416,6360 walk UP (`teleport 416 6376 0; 20 UP`) → Saturn Valley 904,7360. Walking UP from the grass below stops at y≈2048 (also in the original) |
| Twoson → Peaceful Rest Valley → Happy Happy on foot | 06-ToHappy | the "SE cave" is the cave at the east end of Twoson's main road, door 3632,7104 (walk UP); cave rooms exit at 7920,10056 → valley 4280,2248 (mushroom girl npc444 and the mole npc448 block narrow spots); pencil statue 4648,1400 (use the eraser from 4643,1408 facing UP); valley exit is the cave at 5960,1568 → walk right to 5360,2504, UP → Happy Happy 3536,7584 |
| Happy Happy HQ crowd → reception | 07-InHappy | HQ door 3832,7896 (walk UP) → 8080,2320; 1-tile path 8012,2364 → 7948,2404 → 7948,2340 → 7852,2316 → 7852,2348 → 7764,2356 → 7716,2404 → 7628,2404 → 7540,2340, then hold UP+LEFT up the stairs → 8134,1734. Blockers step aside when talked to (one becomes an Insane Cultist battle) |
| Sky Runner launch → crash in Threed | 11-Winters | `teleport 320 4930 0; until text 300 UP`, talk to Andonuts npc615 (he wanders; walk into him), answer through; then from 7850,2872 walk LEFT (area is 24×2 px around 7824,2872, so approach at y=2872); flight ~6500 frames, BOOM ≈ frame +6000 |
| Monotoli 47F → 48F → Sentry Robots → Clumsy Robot | 21-Monotoli | give the maid (npc951, 3520,4208) the yogurt dispenser first; lobby door 3448,4120 (walk UP, flag 143 unset); back room 5448,5416 → elevator npc869 at 5184,5568, talk (sets flag 350; `flag 9 1` if the next door ignores you); door 5160,5560 → 48F 6080,6248; walk LEFT into Sentry Robots npc881/npc882; Clumsy Robot room `teleport 6080 5992 6; 10 UP; 40 LEFT`, talk; Auto Fight ends with the Runaway Five rescue (~6000 frames) |
| Magic Cake dream → Dalaam palace | 23-Summers | `flag 617 1; teleport 6000 2000 0; wait 30; teleport 5184 2884 0; wait 30; 20 LEFT; 20 UP; 20 RIGHT`, talk to npc1069, `until idle 8000 advance` (~3400 frames) |
| Dalaam palace → Mu rope | 23-Summers (after the cake) | palace east door 8144,8328 → 4552,3528; village 4440,4000; south path 4536,4120 → east to the rope base 4968,4120; rope top (Mu) 4976,3816. The ledge east of 4712,3918 (4824,3904) is a dead end |
| Dungeon Man rope room / outside | 29-DungeonM | `teleport 4504 110 0; until idle 900 UP; 100 UP` climbs to 432,592 outside; ropes at 4408/4456/4504/4552,8; holes back in at 488,936 and 424,1128. Door/hole dump: `door_pointer_table` → `door_config_table` (5-byte y,x,type,offset) → `door_data` (type 2: dest x = word@8 ×8, y = (word@6 & 0x3FFF) ×8) |
| Grapefruit Falls password → Belch Base | 14-BelchBase | `teleport 6368 264 0; until text 60 UP` ("Say the password!", door 6368,256 = TEXT_DOOR_461), then no d-pad: `until text 11500` (~10700 frames, "Okay... you may enter."), `until idle 3000 advance` → base 3104,5448. (4200,8616 is the Dalaam Pink Cloud cave, not the falls) |
| Belch Base → password slug npc691 | 14-BelchBase (in the base) | `69 RIGHT; 56 DOWN+RIGHT; 63 RIGHT; 16 UP+RIGHT; 23 RIGHT; 32 UP+RIGHT; 6 RIGHT; 8 DOWN+RIGHT; 114 DOWN` bumps Fly Honey slug npc688 (No → Slimy Little Pile); repeat the same moves from 3255,5504 (minus the first 2) to the ladder 3448,5550, `200 DOWN`, then `29 LEFT; 24 UP+LEFT; 23 LEFT; 32 UP+LEFT; 366 LEFT; 32 UP+LEFT; 74 LEFT; 112 DOWN+LEFT; 86 LEFT; 16 UP+LEFT; 149 LEFT; 24 UP+LEFT; 51 LEFT; 32 UP+LEFT` → 2212,5745 (battles on the way shift your position; check `state` after each) |
| Dungeon Man 4-rope room on foot | 29-DungeonM | from the start: `74 RIGHT; 8 DOWN+RIGHT; 137 RIGHT; 8 DOWN+RIGHT; 69 RIGHT; 16 UP+RIGHT; 34 UP; 40 UP+LEFT; 6 LEFT; 56 UP+LEFT; 23 UP; 24 UP+RIGHT; 40 UP; 24 UP+LEFT; 51 LEFT; 6 LEFT; 24 DOWN+LEFT; 29 DOWN; 16 DOWN+LEFT; 6 LEFT; 8 DOWN+LEFT; 6 LEFT; 16 DOWN+LEFT; 120 LEFT; 32 DOWN+LEFT; 23 LEFT; 4 LEFT; 171 UP; 80 UP+RIGHT; 17 RIGHT; 112 UP+RIGHT; 10 UP` → 4456,105 under rope 2 (signs block x≈4476-4500); `120 UP` climbs to 360,592 |
| Dungeon Man phone room (end room) | 29-DungeonM | holes are unreachable on foot: `teleport 488 960 0; 30 UP` falls to 4592,472; then `20 DOWN; 30 DOWN` off the plateau, west along y≈616 to 4240, north along x≈4176 to 4153,294; the phone npc1201 is Check |
| Brickroad cave → Winters lab | 30-DeepDark | `teleport 204 3864 0; 40 UP` (cave 5128,160); `46 LEFT; 24 DOWN+LEFT; 52 LEFT; 40 DOWN+LEFT; 30 LEFT` → 432,4264; then south keeping x≤296 at y≈4424 (the Stonehenge hole is at 336,4440), down x≈232 to y 4864, `24 DOWN+RIGHT; 12 RIGHT; 48 DOWN+RIGHT` to 317,4935, `until text 300 UP` |
| Stonehenge Base diagonal maze | 31-StonehBase | `teleport 6672 5264 2`, then follow the corridor (east/down, west y≈5384, down, east/down, west y≈5608, up-left, down-left, west) to hole 6144,5544; at 6410,5594 use `12 DOWN+LEFT; 6 DOWN; 30 LEFT` |
| Lost Underworld talking rock npc1292 | 32-LumineHall | `teleport 2864 3080 4`, walk (0.5 px/frame, ~2600 frames) via 2592,2760 → 2392,2720 → camp gate 2343,2688 → 2362,2617, Talk to facing UP |
| Onett drug store (Tee ball bat, cap, bracelet) | run-03 / run-04 | door 1600,1128 (walk UP; from the hotel exit: north along the diagonal road to y≈1090, `50 DOWN; 300 LEFT`, `10 LEFT; until text 200 UP`) → 7912,1512; sports clerk npc9: `28 LEFT; 30 UP` (7873,1496 facing UP). After picking Buy add `wait 20` before the next press, or the menu eats it. Exit: walk DOWN; to reach the arcade street go west of the town hall: `20 UP; 100 LEFT; 40 LEFT; 200 DOWN` → 1371,1512 |
| Onett Shark grinding (Skate/Pogo Punk, Yes Man Junior) | run-04 | enemy placement group 8 (rate 10%) covers x 1280-1791, y 1600-1791. From 1413,1495: `80 DOWN; 40 DOWN+LEFT; 60 DOWN` → 1374,1727, then `until battle 5000 RIGHT×5\|LEFT×5`. Vertical oscillation at x≈1368 spawns nothing. Pairs (Punk + Yes Man Junior) beat a L5 Ness with Tee ball bat/cap/bracelet |

## Viewport and visual review (every shot)

Correct game logic isn't enough: report anything ugly or not adapted to the wide viewport, even when it matches the asm. For every screenshot, look at the whole 320×240 frame, gutters included (x<32, x≥288, y<8, y≥232):

- gutters that are black, empty, garbage, stale tilemap, or a layer that stops at the 256-px band edge
- scenes, windows, sprites or effects off-centre or stuck to one side
- NPCs/objects popping in, out or half-drawn at the gutter edge
- full-screen effects (fades, flashes, darkness/lighting, battle BGs, swirls, mosaic, palette effects, parallax) confined to the band
- anything a player would call ugly: seams, flicker, misaligned layers, wrong priority

Put these in their own "Viewport/visual" section of the report, with shot paths and the pixel region. If unsure, report it and mark it "unsure". Pixel differences from the original inside the band are fine.

## Reporting

Report per item: PASS/FAIL/NOT REACHED, evidence (shot/sheet paths, decoded text, measured vs expected numbers), and for a FAIL the exact asm vs C divergence (file:line on both sides). Keep it short; the person reading it already has the context.
