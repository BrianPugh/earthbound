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
| `fc [N]` | print `FC N`, the low byte of the frame counter (the asm's 1-byte `FRAME_COUNTER`); with a number, set it first. Its phase picks the HP/PP roller's member and blink phases; autodiff.py seeds it to 0 on both sides |
| `ehp ENEMY N [MAX]` | in a battle, set enemy ENEMY's (the `[i]` index in ENEMIES) HP and HP target to N, and with MAX its max HP (damage and healing clamp to it) |
| `egroup N\|off` | from now on (every frame, enemies spawned later too) give every overworld enemy battle group N (`ENTITY_NPC_IDS` = N + 0x8000) and the group's enemies (`ENEMY_BATTLE_GROUPS_TABLE`, each repeated by its count) in turn as `ENTITY_ENEMY_IDS`, until `egroup off`: touching one starts group N's battle (its other enemies join from the ones nearby that pathfind in). Same on the emu. Battles a save can't otherwise reach (the `battle-*` enemy scenarios); the sprites stay the map's |
| `hpt` | print `HPT id:HP/PP ... exp E ... bank B`: each member's HP/PP *rolling target* (where the meter will settle, so big hits show before the meter catches up), exp, and the bank balance |
| `digest` | print `DIGEST flags=HEX items=ID:HEX,... bgm=N ents=SLOT:SCRIPT@X,Y/DIR/SPRITE,... wins=ID:X,Y,WxH,... focus=N` (raw state for autodiff.py; emu.py prints the same) |
| `frac` | print `FRAC leader=X,Y ent24=X,Y bright=HH`: the leader's (game_state) and entity slot 24's 16-bit sub-pixel x,y and the brightness the logic frame ended with (port `g_inidisp_logic_end`, emu `INIDISP_MIRROR`); the port adds `fade=STEP/COUNTER`. For 1-px walking phase and fade-timing diffs that `state` can't show |
| `padlog FILE\|off` | port only: write the pad of every later frame to FILE, one hex word a line. `playtest/padlog_body.py FILE` turns it into a fixed-length autodiff body (see "Writing a long scenario") |
| `regs` / `overclock MODE` | emu only: print `REGS PC=... (FUNCTION+OFF) A= S=` (where a hang spins) / change snes9x's CPU overclock mid-run (`light`, `compatible`, `max`, `disabled`) |
| `peek SYMBOL [INDEX] [COUNT]` | emu only: print `PEEK SYMBOL[INDEX] HHHH ...`, COUNT 16-bit WRAM words from SYMBOL + 2*INDEX (entity tables by slot, e.g. `peek ENTITY_OBSTACLE_FLAGS 2`) |
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

## Regression goldens in git

`playtest/regress/golden/main/` holds the machine-independent half of every
case's result (no screenshots, frame numbers or timings), recorded with the
wide-viewport (320x240) build. Game text must come from the donor ROM, so the
committed files hold no decoded strings: each checkpoint's window text and
TEXT lines are a `sha256:` digest, and enemy names in ENEMIES are hashed per
enemy (HP stays readable). STATE, PARTY (character names included), `until`
results and ERR lines stay plain. On any checkout with `playtest/saves/`:

```
uv run --no-sync python playtest/regress.py check --text --bin port/unix/build-wide/earthbound
```

`check NAME` (default `main`) prefers full goldens in `playtest/out/regress/golden/NAME`
(with screenshots and the actual lines) and falls back to the tracked hashed
goldens; `--text` forces the tracked ones. Frame numbers are never compared. A
hash mismatch names the checkpoint whose text changed; to see the lines,
record full goldens from a known-good build
(`regress.py record main --bin GOOD -k CASE`) and rerun `check main -k CASE`
with the new build. When a change is meant to alter a case, re-record and
commit with `regress.py record main --git --bin BIN` (`-k CASE` rewrites just
that case) and explain each changed checkpoint in the commit message.

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

## Lost-frame check

A helper that runs a window tick inline (`close_window`, `set_window_title`'s
waits, ...) owes the frame the original presents there; the mode step claims it
with `take_unpresented_ticks()` and pushes that many BATTLE_WAIT frames. A count
nobody claims is a frame the port runs and throws away, which autodiff only sees
when an input or a RAND lands on it. A build with `-DEB_LOST_FRAME_CHECK=ON`
(on by default in port/unix Debug builds; src/core/lost_frame_check.h, compiled
out otherwise) accounts every frame and prints a line for each one that lost
frames:

```
LOST-FRAME frame=1378 lost=2 owed=2 claimed=0 work=1 modes=61.6>47.3>45.8>44.0 events=[VV D2@battle.c:2965 W]
```

`owed` ticks (T) and bare waits (V), how many a take `claimed` (C), the frame's own
`work` (W = a window/render tick, P = a BATTLE_WAIT present), and each
`clear_unpresented_ticks()` that dropped a count (D, with its file:line). `modes` is
the stack at the first owed tick, `GameMode.byte0` (usually the phase) from the root;
the harness prints the mode names. Only one frame is presented, so a frame with
unclaimed ticks loses `unclaimed + work - 1`; 1 unclaimed tick and no other work is
the step's own frame and isn't counted. At exit the game prints `lost-frame check: N events`.

`regress.py` (`compare`, `check`, `record`) and `autodiff.py` fail a run with any
LOST-FRAME line and list them (`LOST FRAMES`), even when every checkpoint matches.
`autodiff.py --port-only` runs only the port side of the suite for this (the 112
scenarios in about 30 s at `-j 8`). Before committing a change to a mode step or a
window helper, run both against a check build:

```
cmake -S port/unix -B port/unix/build-lfc -DCMAKE_BUILD_TYPE=Release -DEB_VIEWPORT_WIDTH=320 -DEB_VIEWPORT_HEIGHT=240 -DEB_LOST_FRAME_CHECK=ON
cmake -S port/unix -B port/unix/build-lfc-native -DCMAKE_BUILD_TYPE=Release -DEB_LOST_FRAME_CHECK=ON
uv run --no-sync python playtest/regress.py compare OLD_BIN port/unix/build-lfc/earthbound
uv run --no-sync python playtest/autodiff.py playtest/autodiff/*.play --bin port/unix/build-lfc-native/earthbound --port-only -j 8
```

A new event is a dropped frame until shown otherwise: check the original in the emu
and fix the site (docs/plans/audit-unpresented-ticks.md has the 7 found so far). A
helper whose tick the asm really doesn't present must not count it as owed (say so
at the site, citing the asm); there's no allowlist.

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
  `give` (first empty slot), `rng`, `ehp`, `hpt` and `debug on|off` read/poke WRAM directly. `aux` keys
  don't exist (ERR).
- `snap`/`restore` are core savestates in `snaps/` (they work across
  processes, too).
- Frame counts are SNES frames and the original's intro is longer, so `frame=`
  is not comparable with the port. RNG and lag differ too: compare scenes,
  not frames.

## Logic-frame diff against the original (autodiff)

```
uv run --no-sync python playtest/autodiff.py playtest/saves/05-Twoson.srm SCENARIO.play [...]
```

`autodiff.py` boots the save on the port (`--bin`, default
`port/unix/build-native/earthbound`: use a 256x224 build, so the enemy spawns
match) and on the original (emu.py `--count-frames`), runs the scenario's setup
lines on each (`port:`/`emu:` prefixes pick a side), seeds `rng` on both, then
plays the body after a `---` line (`N BUTTONS`, `wait N`, `press` only) one game
frame at a time. After each frame both sides print `state`, `digest` (event
flags, items, `bgm`, every entity slot, open windows, focus) and `rng`, and it
reports, per field, the first frame where they differ and how many frames do.

Game frames, not wall frames: the emu's `lstep BUTTONS` holds the pad until
the next game frame completes. `--count-frames` patches WAIT_UNTIL_NEXT_FRAME
to count its returns in `FRAME_COUNTER` (and the NMI no longer increments it),
so lag frames (CPU still busy at the NMI) shift neither the inputs nor that
counter's phase, which picks the HP/PP roller's member as in the port (one count
per presented frame). A frame boundary that falls on the wait's entry counts as
parked; one in its tail, after the count, is still the next frame running. A frame that completes inside a lag frame
can't be sampled: the report lists those as merged, and the frame after a lag
frame reads the same pad as the one before. Under sustained CPU load (every frame a little
over budget, so the logic never parks at a frame boundary) an lstep stops once the frame after
its target has completed and prints `LSTEP N mid`: that sample caught a later frame part-way,
so autodiff drops it (merged). Pads still reach the game a frame late there, so change
direction outside such a stretch (runaway-bus.play). `input` is ignored by default (the
2 sides define it differently). The scripts and logs land in
`playtest/out/autodiff/`; rerun a side with `--input NAME.port.play` / `emu.py
--count-frames --input NAME.emu.play`.

The suite in `playtest/autodiff/` (one scenario per file, `# save: NN-Name`
header, optional `# ignore: REGEX [A[-B]]` lines for fields that can't be aligned, on
logic frames A to B only when given (keep masks to the smallest field and frame range,
with a comment saying why), and `# lag: K` lines for an emu logic frame the
original spends on CPU time alone, compared with the port shifted by one) runs with `autodiff.py playtest/autodiff/*.play`
(`-j N` scenarios at once, a few seconds each). Before reaching for a mask, align
the setup: state that differs at frame 0 comes from the 2 boots reaching the seed in
different frames, and a `teleport` to the save's own spot (pos and dir from STATE)
clears the enemies the save loaded and respawns the NPCs (escargo.play, poison-walk.play).
Timers that start before the teleport keep the boots' phase difference, since the emu's
setup waits are wall frames and lag frames there count no logic frame: EVENT_1's sunstroke
check (every 1200 frames from the file load) lands a few frames apart on sand, and a `port: wait
N` before the teleport moves it (desert-walk.play); a wanderer spawned by a lagging teleport is a
frame behind in the original, and an `emu: wait 1` after it lines it up (magic-cake.play).

Writing a long scenario (a battle with menus, a cutscene with prompts, a walk): the body takes
fixed-length inputs only, so plan it on the port with `until` commands and record the pads.
Run the scenario's setup on the port (a `pt.py` session or a `--input` script), then
`rng 4660 22136; fc 0; padlog FILE` where autodiff seeds, then the `until input ... advance` /
walking steps (in a session, adaptive moves are fine: the game waits between batches, but don't
`snap`/`restore` after `padlog`, since a capture free-runs frames), `padlog off`; `uv run
--no-sync python playtest/padlog_body.py FILE` prints the body as `N BUTTONS` runs that replay
the port run frame for frame (giygas-pray.play, paula-farewell.play).

Read a report with care: a first divergence of `idle` alone, 1 frame, at the end
of a fade-in near a lag-merged frame is the original's fade running on NMIs, which
lag frames shift against logic frames either way: a long load before FADE_IN takes
the NMI before the call (the original ends a frame later: townmap, door arrivals),
and lag frames during the fade give it extra steps (a frame earlier: goods-give).
Both sides read the brightness the logic frame ended with (the port's
`g_inidisp_logic_end`, before host_process_frame's NMI-equivalent fade step), so
fades without lag match frame for frame.

File select, naming and new games: `# save: blank` boots an empty 8 KB SRAM
(no save files), and `# boot: menu` replaces the boot that loads slot 1 with one
that stops at the file select menu's first input wait (port `until in:FILE_MENU
3000 START*`, emu the same through its `FILE_MENU` alias for FILE_SELECT_INIT, then
`until input` on both); the body then picks the slot, opens Copy / Set Up, starts a
new game, names the party (new-game.play, naming-keys.play, file-select.play). When the original leaves the file menu
it loads the overworld inside lag frames, and the first emu sample there can land
part-way through the load (still in FILE_SELECT_INIT, before the party or the map
exists) without an `LSTEP mid` mark: a `# skip: K` line drops frame K from the
comparison.

Power-on: `# boot: power` runs no boot at all, so the body starts at RESET (the logos, the
gas station, the title, attract mode; boot-attract.play, boot-title-skip.play,
title-sequence.play, gas-station-skip.play, attract-skip.play). Nothing is seeded: RESET
seeds the RNG with $56781234 (the default `--rng`) and clears FRAME_COUNTER on both sides.
They all run `# overclock: max` (the gas station's battle BG overruns every frame) and skip
frames 1-2 (the emu is still in GAME_INIT, before INIT_ENTITY_SYSTEM). `# compare: bright`
adds the brightness each logic frame ended with (`frac`) to the compared fields, for fade
timing. Lag shows up in 2 more ways there:

- A port frame the original spends inside a load: each attract scene's TELEPORT_TO
  (DISABLED_TRANSITIONS) arms FADE_OUT(1,1) and LOAD_MAP_AT_POSITION waits for it; the
  original's map load (40-odd lag frames) finishes the fade first, the port's instant load
  waits its 2 frames. An `# extra: K [K ...]` line (the mirror of `# lag:`) gives such port
  frames no emu counterpart: the emu doesn't get their pads and later emu frames compare with
  the port's shifted by one each.
- An NMI fade that runs on during a load: the quick title's FADE_IN steps a logic frame later
  in the original (its graphics load eats the NMI after the call; in NMIs both step at the
  2nd after FADE_IN, as 6e0e32bd made the port do), and FILE_SELECT_INIT's FADE_IN(1,1) is 2-3
  steps ahead in the original after RUN_FILE_MENU's load lag, so FILE_SELECT_MENU's @WAIT_FADE
  (counted as frames on both sides) ends 4 frames earlier there: `# ignore: bright` over the
  ramps and 4 `# extra:` frames at the end of the wait (every scenario that reaches the file
  select from the title).

Places that "couldn't be aligned" before, and why:

- **Moonside**: the original's logic overruns every frame there, so
  WAIT_UNTIL_NEXT_FRAME returns at once and no frame boundary ever falls on a
  wall-frame boundary; every lstep ends `LSTEP N mid` and nothing is compared.
  `# overclock: max` runs the emu with snes9x's "Reduce Slowdown" CPU overclock
  (emu.py `--overclock`), so each frame fits and parks as in the port (which has
  no lag); moonside-walk.play aligns frame for frame, and moonside-battle.play a street
  battle on Auto Fight.
- **Scaraba desert, the Winters lab walk, the Sky Runner flight**: lag frames
  every few frames (many enemies / a long CPU-heavy flight with TELEPORTs). A pad
  change next to a lag frame reaches the original a frame late, and enemy spawns
  before the seed follow the setup's lag; with `# overclock: max` both go away
  (scaraba-battle.play, skyrunner.play: the whole flight, 11000 frames). Each
  TELEPORT still needs its `# skip:` / `# lag:` pair, 20 frames apart.
- **Magicant**: no lag problem; outdoors the wandering NPCs' timers differ before
  the seed, so their RAND calls shift the RNG for everything after. Start where no
  NPC wanders: the pink maze (magicant-battle.play), or the save point's hut with no
  setup, so the town's wanderers spawn after the seed (magicant-town.play).

- **The Giygas prayers** (giygas-pray.play): each prayer's vignette is a TELEPORT (its
  load needs a `# skip:` / `# lag:` pair like skyrunner's), and each return to the battle is
  LOAD_BATTLE_SCENE's FADE_IN after the load lag, which ends a logic frame later in the
  original; that extra WAIT_FOR_FADE_WITH_TICK frame ticks a window, so RAND is one call
  ahead from then on and the next turn's rolls differ. A `# lag:` at the fade's end lines the
  frames up and a `# reseed: K` there (autodiff.py seeds both sides again after frame K) puts
  the RNGs back together before the next turn; the reseed hides that one RAND call and
  nothing else (the rng ignore covers the few frames before it). The battle runs overclocked.
  The scenario stops at the battle's end: the meters roll there differently in the 2 games
  (lag-merged frames), so the original's @WAIT_HPPP_STABLE ticks once more and the rest is a
  frame off; paula-farewell.play compares the post-battle farewells (after the debug win).
- **The ending**: overclocked, the emu hangs in the photo album (the NMI's DMA queue loop
  re-enters until the stack runs away, `regs` shows IRQ_ENABLED+EF with S falling), so
  ending.play turns the overclock off at Mom's (`# overclock-off: K`, emu.py `overclock
  disabled`), after the lag-heavy Onett walk; the album and the cast roll then line up with
  skip/lag pairs at their loads. The staff credits can't be compared: CREDITS_SCROLL_FRAME
  scrolls on the IRQ (every wall frame, lag frames too) and each photo waits for the scroll, so
  in logic frames the photos land wherever the original's map loads put them (each load's busy
  wait on the fade is one lag-merged logic frame there, a frame per NMI in the port), and the
  entities differ for most of the roll. ending.play stops at the credits; the-end.play plays them
  in its setup by events (`until idle`) and compares the bedroom to THE END.

The overclock is not neutral elsewhere: it changes how many frames the setup's
`until`s take, so NPC phases and spawns before the seed differ from a plain run
(magicant with it diverges at an NPC). Use it only where the plain emu can't be
sampled.

An RNG state one call apart for a few frames (`WINDOW_TICK` calls RAND on every
tick, so the RNG counts window ticks) usually means the port ran a tick inline
without presenting its frame: a `CLOSE_WINDOW` or `SET_WINDOW_TITLE` wait that a
mode-step dropped. Check the emu's call chain (`modes=`) frame by frame around it
(`N -` wall steps with `state`/`rng`) to find the missing frame. When the emu merges
the frames around it (lag), rerun the scenario with `# overclock: max`: the original
then parks at every frame and each one is sampled (bicycle-dismount.play's 2 missing
frames showed up that way).

## Source coverage (what the suite never runs)

```
uv run --no-sync python playtest/coverage.py build    # port/unix/build-cov, Homebrew clang
uv run --no-sync python playtest/coverage.py run -j 6 # autodiff suite + regress cases
uv run --no-sync python playtest/coverage.py report --md docs/plans/coverage-autodiff.md
```

`coverage.py` builds an instrumented 256x224 port (`-fprofile-instr-generate
-fcoverage-mapping`, needs `brew install llvm`), plays every autodiff scenario (both sides) and
records the regress cases with it, then merges the profiles and lists, per `src/` subsystem,
the executed functions and lines, the game functions never run (debug, platform, selftest and
tool code left out) and the barely-run ones. Code the suite never reaches is where new
scenarios find bugs: write one that gets there (`padlog` for a battle or a menu walk).

## Implementation

The command player is `port/unix/platform/sdl2_input_script.c` (`--input FILE`
for batch, `--session FIFO` for interactive). Window text is captured as it is
printed through `text_trace_hook` (src/game/text.h), which only the desktop
harness sets.
