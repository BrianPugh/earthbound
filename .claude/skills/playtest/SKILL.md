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

## Finding a scene

1. `playtest/saves/INDEX` lists the 36 saves by story point. Pick the closest one.
2. Find the script: grep `asm/data/events/` and the extracted text (`src/assets/` text yml / `MSG_*` labels) for the event or message, then find what spawns it (an NPC's text pointer, a door, a hotspot).
3. Get coordinates and flags from the NPC config / map data (`NpcConfig` has the event flag and script). `NEAR` in STATE shows npc ids with offsets once you're close.
4. `teleport X Y [DIR]` next to it (DIR 0 = up, clockwise; 2 = right, 4 = down). Use `flag N 0|1` to put the story state into shape. Talk with `press A` facing the NPC.

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
- **Enemy count:** `ow_enemies=65535`/`65534` after `teleport` or a door is the asm's own wraparound, and spawning still works. Don't report it.
- **Blinking party:** `blink=N` after a battle means the party sprites blink, so a shot may miss them. Use `wait 1; shot`.
- **Before calling something a port bug, rule out how you walked.** Check the asm's conditions, such as the area box, flags, and facing, against what you actually did.
- **Comment confirmed asm behaviour:** if behaviour looks wrong but matches the asm, the C site may already have a comment saying so. Read it. If you confirm a new one, add such a comment.

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
| Paula's prayer fade-back (EVENT_766 $32,$3C) | 21-Monotoli | set flags 15, 318 and 672, then leave Monotoli Building through the door at 5648,5984 |
| Monotoli backs away (reversed move-toward) | 21-Monotoli | teleport to 5656,5984, then walk right out of the area |
| Fake Monotoli dissolve (DELETE_TPT style 9) | 19-Moonside | `flag 11 1`, `teleport 7604 4714 2`, talk to npc1395 |
| Topolla doorman (style 6 delete) | 19-Moonside | `teleport 4504 5876 0`, talk to npc840 |
| Magicant: Nightmare → Sanctuary montage → wakeup | 34-Magicant | talk to npc1358 at 496,9136, instant-win, `until said:absorbed`. The montage runs ~1100 frames |
| Lier into his hole (reversed move-toward) | 03-GiantStep | `teleport 7508 1236 2`, talk to npc119 |
| Runaway Five bus to Threed | 10-ToThreed | talk to the band at 2224,6584, Yes, then `8 DOWN; 40 LEFT; 20 UP` into the strip under the bus; ride ~5000 frames |
| Twoson–Threed tunnel ghosts (move-toward) | 10-ToThreed | walk into the tunnel; the ghosts follow and warp you back to Twoson |
| Summers sailor to Scaraba (diagonal velocity) | 27-Kraken | `teleport 7264 2908`, talk to npc1065, pay; sailor walks DOWN_LEFT at 0.354 px/frame/axis |
| Credits photo slides | any (36-CavePast) | `debug on`; set flags 698-729 (photos); B+R → page to STAFF → A |
| Damage screen shake | 06-ToHappy | `teleport 6112 7776`, fight Zap Eel + Even Slimier Little Pile; a mortal hit shakes ~60 frames plus 10-frame holds |

When you reach a new scene that took effort to find, **add a row here** (save, exact steps) in the same commit as your work.

## Reporting

Report per item: PASS/FAIL/NOT REACHED, evidence (shot/sheet paths, decoded text, measured vs expected numbers), and for a FAIL the exact asm vs C divergence (file:line on both sides). Keep it short; the person reading it already has the context.
