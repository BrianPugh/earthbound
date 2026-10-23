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
- **Enemy count:** `ow_enemies=65535`/`65534` after `teleport` or a door is the asm's own wraparound, and spawning still works. Don't report it.
- **Blinking party:** `blink=N` after a battle means the party sprites blink, so a shot may miss them. Use `wait 1; shot`.
- **Before calling something a port bug, rule out how you walked.** Check the asm's conditions, such as the area box, flags, and facing, against what you actually did.
- **Comment confirmed asm behaviour:** if behaviour looks wrong but matches the asm, the C site may already have a comment saying so. Read it. If you confirm a new one, add such a comment.

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
| Enter Moonside (cafe bottles) | 19-Moonside | `teleport 5520 6584 0; 10 UP; press L` |
| Moonside → Evil Mani-Mani | 19-Moonside | `teleport 5490 4458 2`, talk, No; `teleport 7720 4616 0; 10 DOWN; 30 UP`, L; `teleport 7624 4730 0; 20 UP`, L |
| Talah Rama gift (slot-14 item name) | 20-TarahRama | `teleport 7984 5716 0`, use the Pencil eraser, `40 UP`; drop 1 item; `teleport 7568 6811 6`, L |
| Monotoli confession, Paula rejoins | 21-Monotoli | `teleport 5632 6040 0` (Stop! scene), then `teleport 5620 6010 2`, L |
| Shrooom! → Rainy Circle | 22-RainyCircle | `teleport 5128 186 0; 20 UP`, L; after the win `until text 600 UP+RIGHT\|RIGHT\|UP\|UP+RIGHT` |
| Summers museum Omar guard | 24-Museum | `teleport 7528 9550 0; 10 UP`, L, Yes; `until text 400 UP` into the hieroglyph room |
| 4-member battle | 24-Museum | `teleport 5128 216 0`, walk LEFT out of the cave: Mighty Bear Seven + Cave Boy |
| Giygas final battle (Pokey, redirect, speeches 1–2) | 36-CavePast | `flag 11 1; teleport 192 6056 0; 30 UP` (door 943 → lair, hotspot 14); `teleport 4224 1000 0; wait 20; 60 UP; until in:BATTLE 6000 advance; wait 150; until input 900 advance`. Hits on the 9999-HP Giygas are redirected onto the party, so target Pokey (`press A; press DOWN; press A`) |

## Reporting

Report per item: PASS/FAIL/NOT REACHED, evidence (shot/sheet paths, decoded text, measured vs expected numbers), and for a FAIL the exact asm vs C divergence (file:line on both sides). Keep it short; the person reading it already has the context.
