# Scripted playtests

Headless, deterministic playtests of the desktop port, driven by a small
input-script language. Two ways to run it:

- **Interactive session** (`pt.py`): a long-lived game process you step from
  separate shell commands. The game blocks between steps (no frame advances
  while it waits), so every step sees exactly the result of the previous one.
  Built for an AI agent playing turn by turn.
- **Batch** (`run.py`): boot a save, play a whole script, write a log and a
  contact sheet.

Both use `$EB_BIN` as the game binary (default `port/unix/build/earthbound`).

## Interactive session

```
PT="uv run --no-sync python playtest/pt.py"
$PT start s1 --save playtest/saves/24-Museum.srm   # boots file 1 -> Continue, waits for control
$PT do s1 "20 DOWN; 2 LEFT; press L; until input 600"
$PT do s1 "press A; until input 300" --no-shot
$PT stop s1
```

`do NAME "COMMANDS"` sends `;`-separated commands, waits until they have run,
then prints what happened and finishes with `text`, `state` and a screenshot:

```
TEXT w1| @Ness picked up the receiver.        <- every line printed this step
UNTIL met after 47 frames (frame=948)
WIN w7* [Call:]| Dad                          <- text now visible, per window (* = focus)
MENU w7| >[Dad] [Mom] [Escargo Express] [Mach Pizza] [Stoic Club]
STATE - frame=956 modes=[OVERWORLD>QUICK_CHECKTALK>...>SELECTION_MENU] idle=modes input=1 pos=(6714,9418) dir=UP music=27 money=46629 battle=0 ow_enemies=0
PARTY 1:Ness L46 HP 379/379 PP 131/131 | 2:Paula L38 HP 141/141 PP 140/140 | ...
NEAR npc1023 (+6,-10) | enemy Spiteful Crow (+211,+105)   <- (dx,dy) from the leader, px
SHOT /abs/path/playtest/out/sessions/s1/shots/0001.png frame=956
```

Read the `SHOT` PNG to see the screen. Options: `--no-shot`, `--quiet` (skip
the automatic text/state), `--sheet` (tile this step's shots into one image),
`--scale N` (upscale shots), `--timeout S`. Session files (log, shots, snaps,
the live `save.srm`) are in `playtest/out/sessions/NAME/`.

STATE fields: `modes` is the mode stack, root first. `idle=yes` means the
player can walk; otherwise it names the blocker (`modes`, `battle`,
`interaction`, `input-disabled`, `leader-frozen`, `fade`). `input=1` means the
game is waiting for a button (idle, a menu, a button-wait text prompt, a
target select). `battle` is nonzero during battles; `ENEMIES` then lists
enemy HP. `blink=N` is post-battle invulnerability: the party sprites blink on
alternate frames, so a shot may miss them (`wait 1; shot`). In text, `@` is the
EarthBound bullet that starts each statement.

## Command reference

One command per line (or `;`-separated in `pt.py do`); `#` starts a comment.

Frame-advancing:

| Command | Meaning |
|---|---|
| `N BUTTONS` | hold BUTTONS for N frames. BUTTONS: `-` or `+`-joined `A B X Y L R START SELECT UP DOWN LEFT RIGHT` |
| `wait N` | N frames, nothing held |
| `press BUTTONS [COUNT]` | tap COUNT times (2 frames down, 4 up) |
| `until COND [MAX] [PATTERN]` | play PATTERN until COND or MAX frames (default 3600); prints `UNTIL met/TIMEOUT` |

`until` conditions: `idle`, `input`, `battle` (enemy touched / battle
started), `text` (any text printed), `said:WORD` (printed text contains WORD),
`in:MODE` / `out:MODE` (mode name such as `BATTLE`, `SELECTION_MENU`,
`TEXT_PROMPT`, or number). `in`/`out`/`idle`/`input` must hold 3 frames in a
row, with buttons released, so a following `press` is not lost. PATTERN is
`|`-separated BUTTONS cycled every 48 frames; `X*` taps X every other frame.
PATTERN `advance` taps A only while a text prompt waits for it, so it never
opens or picks from a menu: `until idle 3000 advance` reads through dialogue.

Instant (no frame passes):

| Command | Meaning |
|---|---|
| `shot [LABEL\|PATH.png]` | write the current frame as PNG (`shots/NNNN-LABEL.png`) |
| `state [LABEL]` / `text` | print STATE/PARTY/NEAR/ENEMIES lines / WIN+MENU lines |
| `flag N [0\|1]` | read / set an event flag |
| `give ITEM [CHAR]` | give item id to char 1-4 (default: first with room) |
| `money N`, `hp CHAR N`, `pp CHAR N`, `heal` | edit the party |
| `aux F1\|F5\|F6\|F7` | pulse a debug key (F1 = full PPU/VRAM dump into `debug/`) |
| `sync ID` | print `SYNC ID` (pt.py uses this) |
| `end` / `quit` | quit the game |

Between frames (at the main-loop root):

| Command | Meaning |
|---|---|
| `teleport X Y [DIR]` | move the party to world pixel X,Y and reload map, NPCs, music (needs `idle`). DIR 0-7, 0=up clockwise, default 4=down |
| `warp NAME\|N` / `warps` | teleport to a PSI Teleport town (`warp summers`) / list them |
| `snap NAME` / `restore NAME` | save / load a savestate in `snaps/` (same process only) |

## Recipes

```
until input 600                                     # wait for the next menu/prompt/control
press A; until input 300                            # pick the highlighted item
press DOWN; press A                                 # move the cursor, then pick
until in:SELECTION_MENU 2000 advance                # read dialogue up to the next choice
until idle 3000 advance                             # read dialogue until the player can walk
snap here; ...experiment...; restore here           # branch and retry
warp onett; until battle 600 LEFT|UP|RIGHT|DOWN     # wander until an encounter
press A; until input 300; press A; until input 300  # battle: Bash + confirm target, per member
```

## Batch runs

```
port/unix/build/earthbound --headless --save SAVE.srm --input my.play
uv run --no-sync python playtest/run.py NAME playtest/saves/24-Museum.srm playtest/scripts/smoke.play
```

`run.py` boots the save (file 1 -> Continue), plays the script and writes
`playtest/out/NAME/{log.txt,sheet.png}`: the output lines and a labelled
contact sheet of every `shot`. Script files use `.play` (`*.txt` is gitignored).

## Story saves

`playtest/saves/` is gitignored. It holds 36 emulator SRAM files (8 KB `.srm`,
loaded directly by `--save`) covering the whole story, from
<https://fantasyanime.com/legacy/earthb_saves.htm>; `saves/INDEX` lists each
save's story point. To recreate it, download the zips from that page and copy
each `earthbound.srm` to `playtest/saves/NN-Name.srm`. The last zip ("End")
holds only ZSNES savestates and is skipped.

## Implementation

The command player is `port/unix/platform/sdl2_input_script.c` (`--input FILE`
for batch, `--session FIFO` for interactive). Window text is captured as it is
printed through `text_trace_hook` (src/game/text.h), which only the desktop
harness sets.
