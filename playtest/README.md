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
$PT do s1 "40 RIGHT" --crop 96x96 --zoom 3        # final shot: 96x96 around the leader, 3x
$PT history s1 > repro.play                       # every command sent so far, as a script
$PT replay s1 s2                                  # rebuild s1 from its starting save in a new session s2
$PT stop s1
```

`do NAME "COMMANDS"` sends `;`-separated commands, waits until they have run,
then prints what happened and finishes with `text`, `state` and a screenshot:

```
TEXT w1| @Ness picked up the receiver.        <- every line printed this step
UNTIL met after 47 frames (frame=948)
WIN w7* [Call:]| Dad                          <- text now visible, per window (* = focus)
MENU w7| >[Dad] [Mom] [Escargo Express] [Mach Pizza] [Stoic Club]
STATE - frame=956 modes=[OVERWORLD>QUICK_CHECKTALK>...>SELECTION_MENU] idle=modes input=1 pos=(6714,9418) dir=UP music=27 money=46629 battle=0 ow_enemies=0 surface=0x08(shallow-water)
PARTY 1:Ness L46 HP 379/379 PP 131/131 | 2:Paula L38 HP 141/141 PP 140/140 st=2:possessed | ...
FOLLOW 2:(-15,+0) 3:(-30,+0) 4:(-45,+0) ghost:(+0,-8)   <- followers' (dx,dy) from the leader, px
NEAR npc1023 (+6,-10) | enemy Spiteful Crow (+211,+105)   <- (dx,dy) from the leader, px
SHOT /abs/path/playtest/out/sessions/s1/shots/0001.png frame=956
```

Read the `SHOT` PNG to see the screen. Options: `--no-shot`, `--quiet` (skip
the automatic text/state), `--sheet` (tile this step's shots into one image),
`--scale N` (upscale shots), `--crop WxH` / `--zoom N` (crop the final shot
around the leader / scale it, as `shot --crop W H --zoom N`), `--timeout S`.

Session files are in `playtest/out/sessions/NAME/`:

| File | Contents |
|---|---|
| `log.txt` | all game output; each `do` batch is echoed first as a `> cmd; cmd` line |
| `history.play` | every command sent, one per line, grouped under `# boot` / `# do N` |
| `start.srm` / `save.srm` | the save the session started from / the live SRAM (in-game saves land here) |
| `shots/` | screenshots and `--sheet` images |
| `snaps/` | `snap` savestates |
| `debug/` | `aux F1` dumps: screenshot BMP, VRAM, CGRAM, PPU registers, VRAM tile sheets |

`pt.py history NAME` prints `history.play` (`--no-boot` drops the boot batch,
so `run.py` can play it, since it boots by itself). `pt.py replay NAME NEW`
starts session NEW from NAME's `start.srm` and resends the whole history;
the game is deterministic, so NEW ends in the same state, frame for frame.

### Video

```
$PT video s1                                      # -> playtest/out/sessions/s1/s1.mp4
$PT video s1 --out bus.mp4 --scale 4 --skip-boot  # 4x pixels, start after the boot batch
```

`pt.py video NAME` renders a session, finished or still running, to an MP4.
It replays `history.play` from `start.srm` in a separate headless game (the
running session is not touched), streams every frame as raw RGB through a
FIFO to ffmpeg (H.264, nearest-neighbour `--scale N`, default 3) and records
the SPC700 output (AAC). Each `do` batch becomes a chapter titled with its
commands, so a player's chapter menu jumps to a step. Video frame *i* is the
game frame shown at poll *i*, so a `SHOT ... frame=F` is video frame F (F
minus the boot length with `--skip-boot`) until a `restore` rewinds the frame
counter. Options: `--fps N` (output rate; the game runs at 60), `--no-audio`,
`--crf N` (x264 quality, default 18), `--keep` (keep `video-tmp/` with the
replay's log, raw audio and chapter file). Expect ~1000 frames/s at scale 3.
The binary must have `--record-video` (`$EB_BIN`, built after 2026-10-23), and
the replay only matches the session if it was played with the same game code.

The binary options behind it: `--record-video PATH` writes raw rgb24 frames at
the viewport size (printed first as `RECORD video WxH fps=60`, so the reader
can be started before PATH, usually a FIFO, is opened), `--record-audio PATH`
writes raw s16le stereo, 534 samples per frame (32040 Hz), and
`--record-paused` waits for the script's `record on`. Both need `--headless`.
Recording does not change the game: replays print the same output with or
without it.

STATE fields: `modes` is the mode stack, root first. `idle=yes` means the
player can walk; otherwise it names the blocker (`modes`, `battle`,
`interaction`, `input-disabled`, `leader-frozen`, `fade`, or the leader moving
by itself: `escalator`, `stairs`, `auto-move` (camera_mode), `auto-walk` (a
replayed door/escalator approach)). `surface` is the leader's
`trodden_tile_type` (SURFACE_FLAGS: `shallow-water`, `deep-water`,
`sunstroke`, `ladder-stairs`, `hide-lower`/`hide-upper` = drawn behind
foreground, `solid`). `walk=N` is a non-normal walking style (3 bicycle,
7 ladder, 8 rope, 12 escalator, 13 stairs), `party_status=N` is nonzero
(1 charred, 3 Skip Sandwich), `debug=1` means `ow.debug_flag` is set. `input=1` means the
game is waiting for a button (idle, a menu, a button-wait text prompt, a
target select). `battle` is nonzero during battles; `ENEMIES` then lists
enemy HP. `blink=N` is post-battle invulnerability: the party sprites blink on
alternate frames, so a shot may miss them (`wait 1; shot`). In text, `@` is the
EarthBound bullet that starts each statement.

PARTY `st=G:NAME,...` lists nonzero afflictions by group, numbered as the
`status` command takes them (1 persistent-easyheal: unconscious, diamondized,
paralyzed, nauseous, poisoned, sunstroke, cold; 2 persistent-hardheal:
mushroomized, possessed; 3 temporary: asleep, crying, immobilized,
solidified; 4 strange; 5 can't-concentrate; 6 homesick; 7 shields). FOLLOW
gives each follower's offset from the leader in party order (`hidden` = no
sprite drawn) and the possession mini ghost's, if it exists.

## Command reference

One command per line (or `;`-separated in `pt.py do`); `#` starts a comment.

Frame-advancing:

| Command | Meaning |
|---|---|
| `N BUTTONS` | hold BUTTONS for N frames. BUTTONS: `-` or `+`-joined `A B X Y L R START SELECT UP DOWN LEFT RIGHT` |
| `wait N` | N frames, nothing held |
| `press BUTTONS [COUNT]` | tap COUNT times (2 frames down, 4 up) |
| `until COND [MAX] [PATTERN]` | play PATTERN until COND or MAX frames (default 3600); prints `UNTIL met/TIMEOUT` |

A PATTERN keeps its buttons held through whatever happens before COND,
cutscenes included. Use the default `-` to wait out a cutscene: `until text
600 UP` that triggers an event keeps holding UP into it, which can disturb
event scripts that expect no input (and `until text` ends at the first
printed character, mid-cutscene).

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
| `shot [--crop W H] [--zoom N] [LABEL\|PATH.png]` | write the current frame as PNG (`shots/NNNN-LABEL.png`); `--crop` keeps a WxH box centred on the leader (clamped to the screen), `--zoom` scales it up N times |
| `state [LABEL]` / `text` | print STATE/PARTY/FOLLOW/NEAR/ENEMIES lines / WIN+MENU lines |
| `flag N [0\|1]` | read / set an event flag |
| `give ITEM [CHAR]` | give item id to char 1-6 (default: first with room). Prints `GIVE item N (Name) -> char C Name`, or `FAILED` when the inventory (or every inventory) is full |
| `status CHAR GROUP VALUE` | set an affliction: `afflictions[GROUP-1] = VALUE` (GROUP 1-7 as in PARTY `st=`, VALUE 0 clears), or GROUP 8 = `party_status` (CHAR ignored). E.g. `status 2 2 2` possesses Paula. In a battle it also sets the member's battler, which the battle copies back over the char afflictions |
| `debug on\|off` | set `ow.debug_flag` (battle debug keys, B+R debug menu) without opening the debug menu |
| `money N`, `hp CHAR N`, `pp CHAR N`, `heal` | edit the party (`hp`/`pp` set the current value and its rolling target; `heal` fills every member's HP/PP). In a battle they also set the member's battler, so the next hit is taken from the new HP |
| `rng [A B]` | print `RNG A B`, the game RNG state (`rng_state` / the asm's `RAND_A`/`RAND_B`); with 2 numbers, set it first. RAND isn't saved in SRAM, so seed both sides with it before comparing RNG-dependent steps against the original |
| `hpt` | print `HPT id:HP/PP ... exp E ... bank B`: each member's HP/PP *rolling target* (where the meter will settle, so big hits show before the meter catches up), exp, and the bank balance |
| `aux F1\|F5\|F6\|F7` | pulse a debug key. F1 dumps PPU/VRAM/CGRAM into `debug/` (in the session dir). F5 also requests the debug menu, which opens on the next overworld frame (even mid-cutscene) and takes input: prefer `debug on` |
| `sync ID` | print `SYNC ID` (pt.py uses this) |
| `note TEXT` | print `> TEXT` (pt.py logs each batch with it) |
| `mark TEXT` | print `MARK n=FRAMES frame=F TEXT`, FRAMES = frames recorded so far (`pt.py video` chapters) |
| `record on\|off` | resume / pause `--record-video`/`--record-audio` |
| `end` / `quit` | quit the game |

Between frames (at the main-loop root):

| Command | Meaning |
|---|---|
| `teleport X Y [DIR]` | move the party to world pixel X,Y and reload map, NPCs, music (needs `idle`, else prints `ERR ... not idle: REASON`). DIR 0-7, 0=up clockwise, default 4=down |
| `warp NAME\|N` / `warps` | teleport to a PSI Teleport town (`warp summers`) / list them |
| `snap NAME` / `restore NAME` | save / load a savestate in `snaps/` (same process only) |

What `teleport`/`warp` reset: they run `initialize_map` (as the debug-menu
teleport) and then the tail of the PSI Teleport arrival:

- Every non-party entity is removed (`CLEAR_ALL_ENEMIES`) and the
  destination's NPCs spawn. Buzz Buzz and the pending timed deliveries are
  respawned (`SPAWN_BUZZ_BUZZ`), so a delivery still arrives, but its
  timer restarts, as after any door. The possession mini ghost is recreated
  on the next frame.
- The party entities are rebuilt at the destination, stacked on the leader
  (followers fan out once you walk). `character_mode` comes from the
  destination sector and `walking_style` resets to normal, which ends a
  bicycle ride, ladder, rope or escalator.
- Kept: `party_status` (a Skip Sandwich keeps working until its timer runs
  out; checked: 62 px vs 42 px in 30 frames after a warp), afflictions,
  inventory, event flags, money, scheduled overworld tasks. Unlike a real
  PSI Teleport, temporary event flags 1-10 are not cleared and no
  transition plays.

## Recipes

```
until input 600                                     # wait for the next menu/prompt/control
press A; until input 300                            # pick the highlighted item
press DOWN; press A                                 # move the cursor, then pick
until in:SELECTION_MENU 2000 advance                # read dialogue up to the next choice
until idle 3000 advance                             # read dialogue until the player can walk
snap here; ...experiment...; restore here           # branch and retry
status 2 2 2; 40 RIGHT; shot --crop 96 96 --zoom 3  # possess Paula, look at the mini ghost
press A; until input 300; press RIGHT; press A      # use an item: menu -> Goods -> Who?...
until input 300; press A; until input 300           # ...char 1 -> item list (DOWN/RIGHT to pick)...
press A; until input 300; press A; until input 300  # ...item -> Use -> Whom? (if it asks)...
press A; until idle 600 advance                     # ...target -> read the result
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

## Frame-by-frame comparison (framehash)

`regress.py compare` diffs 2 builds at the cases' checkpoints. To compare
every frame, use `framehash` (playtest/framehash.py); don't write your own
FIFO reader:

```
uv run --no-sync python playtest/regress.py framehash OLD_BIN NEW_BIN
uv run --no-sync python playtest/regress.py framehash OLD_BIN NEW_BIN -k bus --png 5
uv run --no-sync python playtest/regress.py framehash OLD_BIN NEW_BIN --no-cases \
    --session bk1 --script sun=path/save.srm:path/input.play
```

Each case runs on both binaries at once (`--record-video` to a FIFO), every
frame is hashed, and the report gives per case the frame counts and each
differing frame with its changed pixels split into the SNES band
(`[PAD_LEFT, PAD_LEFT+256) x [PAD_TOP, PAD_TOP+224)`, e.g. x[32,288) y[8,232)
at 320x240) and the gutters/pad rows outside it. The viewport comes from the
game's `RECORD video WxH` line; 320x240, 256x224 and 240x240 builds work, and
both binaries must match. Extra cases: `--script NAME=SAVE:SCRIPT` (played as
is, so include the boot lines) and `--session NAME` (a pt.py session's
start.srm + history.play). Other options: `-j N` parallel cases (2 games
each, default half the CPUs), `--timeout S` per case (default 3x the case's,
600 for extras), `--png N` diff PNGs for the first N differing frames per
case (A | B | changed pixels, band outlined), `--list N` frames listed per
case. Output: `playtest/out/regress/framehash/A-vs-B/` (`report.txt`,
`CASE.csv` with every differing frame, `CASE.A.hashes`, logs, `png/`).

It can't hang: the FIFO is opened non-blocking, reads wait on `select`
against the case deadline, a case that runs out of time has its games killed
and shows as TIMEOUT, a game that exits without recording shows as NO VIDEO,
and Ctrl-C/SIGTERM kill the games and remove the temp dirs.

## Background jobs

Long runs (framehash, regress, videos, BFS scripts) go in the background; keep
them from piling up:

- Don't wait on a job whose output has stopped changing for ~5 minutes: check
  the results it has written, kill it, and carry on.
- Leave no jobs running when you report (`pt.py stop NAME` for sessions).
- `uv run --no-sync python playtest/stale_jobs.py` lists the playtest-related
  processes (games, pt.py, regress.py, emu, ffmpeg, scripts under playtest/ or
  a scratchpad) with elapsed time, %CPU and the age of their newest output, and
  flags idle ones whose output is older than `--minutes` (default 15) as
  STALE. It never kills anything; `--all` prints the output paths it checked.

## Story saves

`playtest/saves/` is gitignored. It holds 36 emulator SRAM files (8 KB `.srm`,
loaded directly by `--save`) covering the whole story, from
<https://fantasyanime.com/legacy/earthb_saves.htm>; `saves/INDEX` lists each
save's story point. To recreate it, download the zips from that page and copy
each `earthbound.srm` to `playtest/saves/NN-Name.srm`. The last zip ("End")
holds only ZSNES savestates and is skipped.

## The original game (emu mode)

`pt.py start NAME --emu --save SAVE.srm` runs the ORIGINAL game instead of
the port: `playtest/emu.py` loads a snes9x libretro core with ctypes, boots
the retail ROM with the save in SRAM and plays the same command language.
`do`, `stop`, `history`, `replay` and `video` work unchanged on an emu session
(the session dir holds an `engine` file saying `emu`). Use it to check what
the original does in a scene. The core is deterministic too, so `replay`
rebuilds an emu session frame for frame.

One-time setup:

```
playtest/emu/build.sh            # clone + build snes9x's libretro core into playtest/emu/build/ (~25 s)
uv pip install numpy             # emu.py converts frames with numpy
make                             # emu.py reads build/earthbound.map and build/US/*.lst
```

The ROM is `earthbound.sfc` in the repo root (or `build/earthbound.sfc`, or
`$EB_ROM`); `$EB_EMU_CORE` overrides the core path. Symbols (the map's exports
plus every top-level label in the listings) are cached in
`playtest/out/emu-symbols.json` and rebuilt when the map or listings change.

```
pt start o1 --emu --save playtest/saves/05-Twoson.srm    # title -> file 1 -> Continue (~900 frames)
pt do o1 "teleport 1352 7010 0; wait 20; 56 UP; until text 600; wait 60"
pt do o1 "until input 300"                                # stops at the text prompt
```

What differs from the port:

- `modes=[...]` in STATE is the asm call chain, outermost first
  (`GAME_INIT>MAIN_LOOP>OPEN_MENU_BUTTON>SELECTION_MENU>...`), recovered from
  the JSL/JSR return addresses on the 65816 stack at the end of the frame. The
  original has no mode stack, so names are asm labels. `wait=MODE` names the
  input loop that waits for a button (`TEXT_PROMPT` = `CC_13_14`,
  `SELECTION_MENU`, `NUMBER_SELECT`, `CHAR_SELECT`, `BATTLE_ENEMY_SELECT`,
  `BATTLE_ROW_SELECT`).
- `idle` means the CPU is in one of `MAIN_LOOP`'s per-frame calls, no window is
  open, and the same flags as the port's `idle` are clear (battle, pending
  interaction, input disabled, leader frozen, escalator/stairs, auto-move,
  fade). `input` is idle or a `wait=` loop. `until idle`/`input`/`in:`/`out:`
  settle for 3 frames like the port.
- `in:`/`out:` take the port's names above, `OVERWORLD`, `BATTLE`
  (`BATTLE_MODE_FLAG`), or any asm function label (`in:SHOW_TITLE_SCREEN`).
- Window text is not decoded: no `TEXT` lines, `until said:WORD` prints an ERR,
  and `until text` fires when a window opens or a window's text cursor moves.
  `text` prints each open window's box and title (`WIN w1*| (box 12,1 17x6)`)
  and the focused menu's items with the cursor (`MENU w0| >[Talk to] [Goods] ...`).
  Read the screenshot for the words.
- STATE adds `bgm=` (`CURRENT_MUSIC_TRACK`, the track actually playing);
  `music=` is `CURRENT_MAP_MUSIC_TRACK` as in the port. FOLLOW has no ghost.
- Shots are 256x224 (the port's wide viewport is 320 wide: the same window sits
  32 px further right there).
- `teleport X Y [DIR]` / `warp` / `warps` work: while the CPU spins in
  `WAIT_UNTIL_NEXT_FRAME` under the main loop, emu.py writes a small 65816
  stub to unused WRAM (the bottom of the hardware stack, `$7E1F00`) and points
  PC at it through the core's savestate. The stub does what the port's
  teleport does (force a map reload, `INITIALIZE_MAP`,
  `FLUSH_ENTITY_CREATION_QUEUE`, `ENABLE_ALL_ENTITIES`, `SPAWN_BUZZ_BUZZ`), then
  resumes the wait loop. It needs `idle`, like the port.
- `flag`, `money`, `hp`/`pp`/`heal` (also the battler in a battle), `status`,
  `give` (first empty slot), `rng`, `hpt` and `debug on|off` read/poke WRAM directly. `aux` keys
  don't exist (ERR).
- `snap`/`restore` are core savestates in `snaps/` (they work across
  processes, too).
- Frame counts are SNES frames and the original's intro is longer, so `frame=`
  is not comparable with the port. RNG and lag differ too: compare scenes,
  not frames.

## Implementation

The command player is `port/unix/platform/sdl2_input_script.c` (`--input FILE`
for batch, `--session FIFO` for interactive). Window text is captured as it is
printed through `text_trace_hook` (src/game/text.h), which only the desktop
harness sets.
