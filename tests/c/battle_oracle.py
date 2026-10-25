"""Record battle-math test cases from the ORIGINAL game, for tests/c/battle_math_test.c.

The original ROM runs in the playtest emulator (``playtest/emu.py``, snes9x
libretro core). Each case boots once to the overworld, then for every case
restores that state, writes the inputs into WRAM (``RAND_A``/``RAND_B``, the
battlers, the party characters, event flags), calls the asm routine itself
through a small 65816 stub, and reads back the return value (A) and the same
WRAM blocks. The C test feeds the identical inputs to the port's function and
compares. Nothing here is hand-typed ROM data: inputs come from a seeded
Python RNG (or from the save's own party), outputs from the ROM's code.

Usage (needs ``make``, ``playtest/emu/build.sh``, the donor ROM and saves)::

    uv run --no-sync python tests/c/battle_oracle.py            # writes tests/c/battle_cases.txt
    uv run --no-sync python tests/c/battle_oracle.py --out FILE --save playtest/saves/26-Dalaam.srm

Calling a routine: the asm functions are near (RTS) or far (RTL) per
``BEGIN_C_FUNCTION``/``BEGIN_C_FUNCTION_FAR``. A near one is entered with a
fake return address that points at an RTL byte inside its own bank, under a
long return to the stub, so its RTS lands on that RTL and comes back. The first
3 16-bit parameters go in A, X, Y (the VUCC convention); the stub runs while the
CPU idles in the main loop's ``WAIT_UNTIL_NEXT_FRAME`` (as emu.py's teleport).

Output format (a ``battlers_table ADDR`` header, then one block per case, all
numbers hex)::

    case NAME
    regs A X Y                   # register parameters
    rng A B                      # RAND_A, RAND_B before the call
    atk I / tgt I                # CURRENT_ATTACKER / CURRENT_TARGET battler index
    battler I HEX78              # battler I (78 bytes) before
    char I HEX95                 # PARTY_CHARACTERS[I] (95 bytes) before
    flag N V                     # event flag N (1-based)
    frames N                     # frames the call ran for (> 1: it displayed text)
    ret V                        # A after the call (only for routines that return a value)
    rng_out A B
    battler_out I [OFF=VAL ...]  # the bytes of battler I that changed
    char_out I [OFF=VAL ...]
    end

The scene cases (``record_scenes``) add named globals and the game state::

    let ID HEX                   # a long blob, used later as @ID (the save's chars, flags, ...)
    var NAME HEX                 # WRAM symbol NAME before the call (little-endian bytes)
    game_state HEX               # GAME_STATE (473 bytes) before
    var_out NAME HEX             # NAME after
    game_state_out [OFF=VAL ...]

Scene cases come from several saves (``SCENE_SAVES``): the party is the save's
own, run through BATTLE_INIT_PLAYER_STATS in the ROM, and the enemies are real
ones from BATTLE_INIT_ENEMY_STATS (both recorded as cases themselves), with HP,
statuses and stats varied. They cover the PSI and item action table, PSI
Thunder's and Pray's own target picks, KOs (enemy group deaths, final actions,
party members), CALC_DAMAGE's Giygas redirect and guts save, and CHOOSE_TARGET.
Text control codes do not run in the C test (its texts are popped at once), so
EVENT_FLAGS is an input only and a few actions whose texts move the story are
left out (see the comments at GIYGAS_ACTIONS and Scenes).
"""

import argparse
import random
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "playtest"))

import emu  # noqa: E402
import pt  # noqa: E402

BATTLER_SIZE = emu.BATTLER_SIZE
CHAR_SIZE = emu.CHAR_SIZE
OUT = Path(__file__).resolve().parent / "battle_cases.txt"
STUB = emu.STUB_ADDR
RESULT = STUB + emu.STUB_LIMIT - 4  # 16-bit A after the call
DONE = STUB + emu.STUB_LIMIT - 1

# struct battler offsets (include/structs.asm)
B_CONSCIOUSNESS, B_ALLY_OR_ENEMY, B_NPC_ID = 12, 14, 15
B_AFFLICTIONS, B_OFFENSE, B_DEFENSE, B_SPEED, B_LUCK = 29, 38, 40, 42, 46
B_BASE_OFFENSE, B_BASE_DEFENSE = 50, 51
B_ACTION_ITEM_SLOT, B_CURRENT_ACTION_ARGUMENT = 7, 8
C_ITEMS = 35  # char_struct::items
B_SPRITE_X = 68
B_ID, B_CURRENT_ACTION, B_ROW, B_HP, B_HP_TARGET, B_HP_MAX = 0, 4, 16, 17, 19, 21
B_GUARDING, B_SHIELD_HP, B_GUTS = 36, 37, 44
B_PP, B_PP_TARGET, B_PP_MAX, B_VITALITY, B_HYPNOSIS_RESIST = 23, 25, 27, 48, 60
ACTION_TYPE_PHYSICAL, ACTION_TYPE_PSI = 1, 3  # include/constants/battle.asm ACTION_TYPE
ACTION_COUNT = 318  # BATTLE_ACTION_TABLE entries (12 bytes, D57B68..PSI_ABILITY_TABLE)
# (group, status) pairs the attack paths test: paralyzed, nauseous, asleep, crying,
# immobilized, solidified, strange, and the 4 shields (include/constants/battle.asm STATUS_*)
ATTACK_STATUSES = [(0, 3), (0, 4), (2, 1), (2, 2), (2, 3), (2, 4), (3, 1), (6, 1), (6, 2), (6, 3), (6, 4)]
AFFLICTION_GROUPS = 7
EVENT_FLAG_BYTES = 128  # EVENT_FLAG_COUNT / 8 (include/enums.asm)
GAME_STATE_SIZE = 473  # .SIZEOF(game_state) (src/game/game_state.h ASSERT_STRUCT_SIZE)
GS_PARTY_MEMBERS = 122  # game_state::party_members (include/structs.asm)
FIRST_ENEMY = 8  # enemies occupy battlers 8.. (BATTLE_INIT: battlers_table[8 + i])
FLG_WIN_OSCAR = 74  # include/constants/event_flags.asm (also inventory.c)


def asm_info(label):
    """Return (far, returns) for the VUCC routine ``label``.

    ``far``: BEGIN_C_FUNCTION_FAR (RTL) rather than near (RTS); ``returns``: it
    declares a return value (STACK_RESERVE_RETURN_*).
    """
    pat = re.compile(rf"^{re.escape(label)}:\s*$", re.M)
    for f in sorted((ROOT / "asm").rglob("*.asm"), key=lambda p: "-jp" in p.name):
        text = f.read_text(errors="replace")
        m = pat.search(text)
        if not m:
            continue
        far = None
        for line in text[m.end() :].splitlines():
            line = line.split(";")[0].strip()
            if not line:
                continue
            if far is None:
                if not line.startswith("BEGIN_C_FUNCTION"):
                    raise RuntimeError(f"{label}: no BEGIN_C_FUNCTION after the label ({line!r})")
                far = line.startswith("BEGIN_C_FUNCTION_FAR")
            elif line.startswith("STACK_RESERVE_RETURN"):
                return far, True
            elif line.startswith("END_STACK_VARS") or not line.startswith("STACK_RESERVE"):
                return far, False
        return far, False
    raise RuntimeError(f"{label}: label not found in asm/")


class Oracle:
    """Calls asm routines in the original game running in emu.py's snes9x core."""

    def __init__(self, save):
        core = emu.Core(emu.rom_path())
        data = Path(save).read_bytes()
        emu.C.memmove(core.sram, data, min(len(data), len(core.sram)))
        self.core = core
        self.g = emu.Game(core, emu.load_symbols())
        self.player = emu.Player(self.g, ROOT / "playtest" / "out")
        for line in pt.EMU_BOOT.split(";"):
            self.player.run_line(line.strip())
        if self.g.not_idle_reason():
            sys.exit(f"boot did not reach the overworld ({self.g.not_idle_reason()})")
        wait = self.g.sym("WAIT_UNTIL_NEXT_FRAME")
        wait_end = self.g.function_end(wait)
        for _ in range(60):
            state = core.serialize()
            regs = emu.read_regs(state)
            self.ret = (regs["PB"] << 16) | regs["PC"]
            if wait <= self.ret < wait_end:
                break
            self.player.step(0)
        else:
            sys.exit("the CPU never reached the frame wait")
        # WINDOW_TICK starts with JSL RAND (the port's window_tick does the same). The
        # texts the attack routines print tick the window once per frame, which would
        # interleave frame-count-dependent RAND calls with the math under test; the C
        # test runs no frames. NOP the JSL out (snes9x patches the ROM byte as a cheat).
        jsl = self.g.sym("WINDOW_TICK") + 2
        if self.g.rom8(jsl) != 0x22 or self.g.rom8(jsl + 1) | self.g.rom8(jsl + 2) << 8 | self.g.rom8(
            jsl + 3
        ) << 16 != (self.g.sym("RAND")):
            sys.exit("WINDOW_TICK no longer starts with REP; JSL RAND")
        core.lib.retro_cheat_set.argtypes = [emu.C.c_uint, emu.C.c_bool, emu.C.c_char_p]
        code = "+".join(f"{jsl + k:06X}:EA" for k in range(4))
        core.lib.retro_cheat_set(0, True, code.encode())
        # Freeze every entity: the text a routine prints runs frames, and the
        # NPCs' actionscripts would call RAND in them (the port's battle texts don't)
        hi = self.g.sym("ENTITY_TICK_CALLBACK_HIGH")
        for slot in range(emu.MAX_ENTITIES):
            v = self.g.w16(hi + slot * 2)
            self.g.put16(hi + slot * 2, v | emu.OBJECT_TICK_DISABLED | emu.OBJECT_MOVE_DISABLED)
        self.base = core.serialize()
        self.bt = self.g.sym("BATTLERS_TABLE")
        self.info = {}
        self.rtl = {}

    def battler_addr(self, i):
        return self.bt + i * BATTLER_SIZE

    def rtl_in_bank(self, bank):
        """A ROM address in ``bank`` holding an RTL ($6B) byte."""
        if bank not in self.rtl:
            off = (bank - 0xC0) * 0x10000
            i = self.core.rom.index(b"\x6b", off, off + 0x10000)
            self.rtl[bank] = 0xC00000 + i
        return self.rtl[bank]

    def stub(self, label, a, x, y):
        def long(v):
            return [v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF]

        def w(v):
            return [v & 0xFF, (v >> 8) & 0xFF]

        if label not in self.info:
            self.info[label] = asm_info(label)
        fn = self.g.sym(label)
        code = [0x08, 0x8B, 0x0B, 0xC2, 0x30]  # PHP; PHB; PHD; REP #$30
        back = STUB + 0  # patched below
        tail = []
        if self.info[label][0]:
            call = [0xA9, *w(a), 0xA2, *w(x), 0xA0, *w(y), 0x22, *long(fn)]  # LDA/LDX/LDY; JSL
        else:
            # PHK (our bank) + PEA ret-1 = long return; PEA rtl-1 = the near return
            call = [0x4B, 0xF4, 0, 0, 0xF4, *w(self.rtl_in_bank(fn >> 16) - 1)]
            call += [0xA9, *w(a), 0xA2, *w(x), 0xA0, *w(y), 0x5C, *long(fn)]  # LDA/LDX/LDY; JML
        back = STUB + len(code) + len(call)
        if not self.info[label][0]:
            call[2:4] = w(back - 1)
        tail = [0xC2, 0x30, 0x8F, *long(RESULT)]  # REP #$30; STA RESULT
        tail += [0xE2, 0x20, 0xA9, 0x01, 0x8F, *long(DONE)]  # SEP #$20; LDA #1; STA DONE
        tail += [0x2B, 0xAB, 0x28, 0x5C, *long(self.ret)]  # PLD; PLB; PLP; JML ret
        out = code + call + tail
        if len(out) >= emu.STUB_LIMIT - 4:
            raise RuntimeError("stub too long")
        return out

    def call(
        self,
        label,
        a=0,
        x=0,
        y=0,
        rng=(0, 0),
        atk=None,
        tgt=None,
        battlers=None,
        chars=None,
        flags=None,
        func=None,
        wram=None,
        game_state=None,
    ):
        """Run ``label`` once from the boot state; return (A, rng_out, battlers_out, chars_out).

        ``wram`` ({SYMBOL: bytes}) and ``game_state`` (bytes) are written before the
        call too; their values after it are left in ``self.wram_out`` / ``self.gs_out``.
        """
        g = self.g
        self.core.unserialize(self.base)
        g.frame_changed()
        g.put16(g.sym("RAND_A"), rng[0])
        g.put16(g.sym("RAND_B"), rng[1])
        for i, blob in (battlers or {}).items():
            for k, b in enumerate(blob):
                g.put8(self.battler_addr(i) + k, b)
        for i, blob in (chars or {}).items():
            for k, b in enumerate(blob):
                g.put8(g.char(i + 1) + k, b)
        for n, v in (flags or {}).items():
            g.set_flag(n, v)
        for name, blob in (wram or {}).items():
            for k, b in enumerate(blob):
                g.put8(g.sym(name) + k, b)
        if game_state is not None:
            for k, b in enumerate(game_state):
                g.put8(g.sym("GAME_STATE") + k, b)
        if atk is not None:
            g.put16(g.sym("CURRENT_ATTACKER"), self.battler_addr(atk) & 0xFFFF)
        if tgt is not None:
            g.put16(g.sym("CURRENT_TARGET"), self.battler_addr(tgt) & 0xFFFF)
        for i, b in enumerate(self.stub(label, a, x, y)):
            g.put8(STUB + i, b)
        g.put8(DONE, 0)
        state = self.core.serialize()
        self.core.unserialize(emu.write_regs(state, {"PB": STUB >> 16, "PC": STUB & 0xFFFF}))
        # Battle texts open a window and may wait at a prompt: tap A until the routine returns
        a_button = 1 << emu.BUTTON_IDS["A"]
        for n in range(3000):
            self.core.run(a_button if n % 4 == 3 else 0)
            if g.w8(DONE):
                break
        else:
            raise TimeoutError(f"{label} did not return within 3000 frames")
        self.frames = n + 1
        ret = g.w16(RESULT)
        rng_out = (g.w16(g.sym("RAND_A")), g.w16(g.sym("RAND_B")))
        b_out = {i: bytes(g.w8(self.battler_addr(i) + k) for k in range(BATTLER_SIZE)) for i in (battlers or {})}
        c_out = {i: bytes(g.w8(g.char(i + 1) + k) for k in range(CHAR_SIZE)) for i in (chars or {})}
        # EVENT_FLAGS is an input only: enemy death texts set flags with text
        # control codes, and the C test pops its texts without running them
        self.wram_out = {
            n: bytes(g.w8(g.sym(n) + k) for k in range(len(b))) for n, b in (wram or {}).items() if n != "EVENT_FLAGS"
        }
        self.gs_out = None
        if game_state is not None:
            self.gs_out = bytes(g.w8(g.sym("GAME_STATE") + k) for k in range(len(game_state)))
        return ret, rng_out, b_out, c_out

    def game_state(self):
        """The save's GAME_STATE (473 bytes) in the boot state."""
        self.core.unserialize(self.base)
        return bytes(self.g.w8(self.g.sym("GAME_STATE") + k) for k in range(GAME_STATE_SIZE))

    def party_char(self, i):
        """The save's own PARTY_CHARACTERS[i] (95 bytes) in the boot state."""
        self.core.unserialize(self.base)
        return bytes(self.g.w8(self.g.char(i + 1) + k) for k in range(CHAR_SIZE))


class Recorder:
    def __init__(self, oracle, lets=None):
        self.o = oracle
        self.lets = lets  # {hex: id} shared by every Recorder writing one file, or None
        self.lines = []
        self.count = 0
        self.skipped = 0

    def blob(self, data):
        """``data`` as hex, or as ``@ID`` of a ``let ID HEX`` line for a long blob that repeats."""
        h = bytes(data).hex()
        if self.lets is None or len(h) <= 2 * BATTLER_SIZE:  # battlers vary per case: keep inline
            return h
        if h not in self.lets:
            self.lets[h] = len(self.lets)
            self.lines.append(f"let {self.lets[h]:x} {h}")
        return f"@{self.lets[h]:x}"

    def case(self, name, label, ret_mask=0xFFFF, returns=None, **kw):
        """Run one case and append its block.

        ``returns`` overrides the asm's STACK_RESERVE_RETURN declaration (some
        routines return in A without declaring it).
        """
        try:
            ret, rng_out, b_out, c_out = self.o.call(label, **kw)
        except TimeoutError:
            self.skipped += 1
            return None, None, {}, {}
        a, x, y = kw.get("a", 0), kw.get("x", 0), kw.get("y", 0)
        out = self.lines
        out.append(f"case {name}")
        out.append(f"regs {a:x} {x:x} {y:x}")
        if kw.get("func") is not None:
            out.append(f"func {kw['func']:x}")
        out.append(f"rng {kw['rng'][0]:x} {kw['rng'][1]:x}")
        if kw.get("atk") is not None:
            out.append(f"atk {kw['atk']:x}")
        if kw.get("tgt") is not None:
            out.append(f"tgt {kw['tgt']:x}")
        pre = []  # blob lines go after the let lines they may add
        for i, blob in (kw.get("battlers") or {}).items():
            pre.append(f"battler {i:x} {self.blob(blob)}")
        for i, blob in (kw.get("chars") or {}).items():
            pre.append(f"char {i:x} {self.blob(blob)}")
        for n, v in (kw.get("flags") or {}).items():
            pre.append(f"flag {n:x} {v:x}")
        for n, blob in (kw.get("wram") or {}).items():
            pre.append(f"var {n} {self.blob(blob)}")
        if kw.get("game_state") is not None:
            pre.append(f"game_state {self.blob(kw['game_state'])}")
        out += pre
        out.append(f"frames {self.o.frames:x}")
        if self.o.info[label][1] if returns is None else returns:
            out.append(f"ret {ret & ret_mask:x}")
        out.append(f"rng_out {rng_out[0]:x} {rng_out[1]:x}")
        for i, blob in b_out.items():
            out.append(f"battler_out {i:x}{byte_changes(kw['battlers'][i], blob)}")
        for i, blob in c_out.items():
            out.append(f"char_out {i:x}{byte_changes(kw['chars'][i], blob)}")
        for n, blob in self.o.wram_out.items():
            out.append(f"var_out {n} {self.blob(blob)}")
        if self.o.gs_out is not None:
            out.append(f"game_state_out{byte_changes(kw['game_state'], self.o.gs_out)}")
        out.append("end")
        self.count += 1
        return ret, rng_out, b_out, c_out


def byte_changes(before, after):
    """`` OFF=VAL`` (hex) for every byte of ``after`` that differs from ``before``."""
    return "".join(f" {k:x}={b:02x}" for k, (a, b) in enumerate(zip(before, after, strict=True)) if a != b)


def rand_battler(r, fields):
    """A random 78-byte battler with ``fields`` ({offset: (value, size)}) set."""
    b = bytearray(r.randrange(256) for _ in range(BATTLER_SIZE))
    for off, (v, size) in fields.items():
        b[off : off + size] = (v & ((1 << (8 * size)) - 1)).to_bytes(size, "little")
    return bytes(b)


def record_all(o, r):
    rec = Recorder(o)

    def seed():
        return (r.randrange(0x10000), r.randrange(0x10000))

    # --- RNG-driven success checks
    for _ in range(40):
        rec.case("SUCCESS_255", "SUCCESS_255", a=r.randrange(0, 300), rng=seed())
    for _ in range(40):
        rec.case("SUCCESS_500", "SUCCESS_500", a=r.choice([0, 1, 25, 250, 499, 500, r.randrange(0, 700)]), rng=seed())
    for _ in range(40):
        atk = rand_battler(r, {B_SPEED: (r.randrange(0, 600), 2)})
        tgt = rand_battler(r, {B_SPEED: (r.randrange(0, 400), 2)})
        rec.case(
            "SUCCESS_SPEED",
            "SUCCESS_SPEED",
            a=r.choice([100, 250, 500, r.randrange(1, 1000)]),
            rng=seed(),
            atk=0,
            tgt=9,
            battlers={0: atk, 9: tgt},
        )
    for label in ("SUCCESS_LUCK40", "SUCCESS_LUCK80"):
        for _ in range(30):
            tgt = rand_battler(r, {B_LUCK: (r.randrange(0, 120), 2)})
            rec.case(label, label, rng=seed(), tgt=2, battlers={2: tgt})

    # --- dodge: statuses that block it, then speed
    for _ in range(60):
        f = {
            B_SPEED: (r.randrange(0, 300), 2),
            B_AFFLICTIONS + 0: (r.choice([0, 0, 0, 3, 5]), 1),
            B_AFFLICTIONS + 2: (r.choice([0, 0, 0, 1, 2, 3, 4]), 1),
        }
        tgt = rand_battler(r, f)
        atk = rand_battler(r, {B_SPEED: (r.randrange(0, 300), 2)})
        rec.case("DETERMINE_DODGE", "DETERMINE_DODGE", rng=seed(), atk=10, tgt=1, battlers={10: atk, 1: tgt})

    # --- damage variance
    for label in ("TWENTY_FIVE_PERCENT_VARIANCE", "FIFTY_PERCENT_VARIANCE"):
        for _ in range(60):
            v = r.choice([0, 1, 2, 10, r.randrange(0, 200), r.randrange(0, 2000), r.randrange(0, 0x10000)])
            rec.case(label, label, a=v, rng=seed())

    # --- PSI resistance modifiers (8-bit return in A; INIT_ENEMY_STATS stores it)
    for label in ("CALC_PSI_DMG_MODIFIERS", "CALC_PSI_RES_MODIFIERS"):
        for lvl in (*range(6), 0x80, 0xFF):
            rec.case(label, label, ret_mask=0xFF, returns=True, a=lvl, rng=seed())

    # --- offense/defense up/down (A = the battler)
    for label in (
        "INCREASE_OFFENSE_16TH",
        "HEXADECIMATE_OFFENSE",
        "INCREASE_DEFENSE_16TH",
        "HEXADECIMATE_DEFENSE",
    ):
        for _ in range(20):
            f = {
                B_OFFENSE: (r.randrange(0, 400), 2),
                B_DEFENSE: (r.randrange(0, 400), 2),
                B_BASE_OFFENSE: (r.randrange(0, 256), 1),
                B_BASE_DEFENSE: (r.randrange(0, 256), 1),
            }
            i = r.randrange(0, 32)
            rec.case(label, label, a=o.battler_addr(i) & 0xFFFF, rng=seed(), battlers={i: rand_battler(r, f)})

    # --- status ailments
    for _ in range(50):
        group = r.randrange(0, AFFLICTION_GROUPS)
        f = {B_NPC_ID: (r.choice([0, 0, 0, 1]), 1)}
        for k in range(AFFLICTION_GROUPS):
            f[B_AFFLICTIONS + k] = (r.choice([0, 0, 1, 2, 3, 4, 5, 6, 7]), 1)
        i = r.randrange(0, 32)
        rec.case(
            "INFLICT_STATUS_BATTLE",
            "INFLICT_STATUS_BATTLE",
            a=o.battler_addr(i) & 0xFFFF,
            x=group,
            y=r.randrange(1, 8),
            rng=seed(),
            battlers={i: rand_battler(r, f)},
        )

    # --- level-up stat gains from the save's own party (equipment and all)
    for _ in range(24):
        c = r.randrange(0, 4)
        rec.case(
            "LEVEL_UP_CHAR",
            "LEVEL_UP_CHAR",
            a=c + 1,
            x=0,  # play_sound = 0: no text
            rng=seed(),
            chars={c: o.party_char(c)},
            flags={FLG_WIN_OSCAR: r.randrange(2)},
        )
    for _ in range(24):
        c = r.randrange(0, 4)
        rec.case(
            "RESET_CHAR_LEVEL_ONE",
            "RESET_CHAR_LEVEL_ONE",
            a=c + 1,
            x=r.randrange(1, 100),
            y=r.randrange(2),
            rng=seed(),
            chars={c: o.party_char(c)},
            flags={FLG_WIN_OSCAR: r.randrange(2)},
        )

    # --- whole attacks and the calc pipeline (texts are displayed, so these run for a few frames)
    def fighter(ally, c=0):
        f = {
            B_CONSCIOUSNESS: (1, 1),
            B_ALLY_OR_ENEMY: (0 if ally else 1, 1),
            B_NPC_ID: (0, 1),
            B_ID: ((c + 1) if ally else r.choice([*range(1, 90), *range(100, 160)]), 2),
            B_ROW: (c if ally else r.randrange(2), 1),
            B_CURRENT_ACTION: (r.randrange(0, ACTION_COUNT), 2),
            B_HP: (r.randrange(1500, 3000), 2),
            B_HP_TARGET: (r.randrange(1500, 3000), 2),
            B_HP_MAX: (3000, 2),
            B_GUARDING: (r.choice([0, 0, 0, 1]), 1),
            B_SHIELD_HP: (r.randrange(1, 4), 1),
            B_OFFENSE: (r.randrange(0, 250), 2),
            B_DEFENSE: (r.randrange(0, 250), 2),
            B_SPEED: (r.randrange(0, 250), 2),
            B_GUTS: (r.choice([0, 10, 25, 50, 100, 250, 500]), 2),
            B_LUCK: (r.randrange(0, 100), 2),
        }
        for k in range(AFFLICTION_GROUPS):
            f[B_AFFLICTIONS + k] = (0, 1)
        if r.random() < 0.5:
            k, v = r.choice(ATTACK_STATUSES)
            f[B_AFFLICTIONS + k] = (v, 1)
        return rand_battler(r, f)

    # Action ids by type, to pick an attacker's current_action (the choice of input only)
    table = o.g.sym("BATTLE_ACTION_TABLE")
    by_type = {}
    for act in range(ACTION_COUNT):
        by_type.setdefault(o.g.rom8(table + act * 12 + 2), []).append(act)

    def set_fields(blob, fields):
        b = bytearray(blob)
        for off, (v, size) in fields.items():
            b[off : off + size] = v.to_bytes(size, "little")
        return bytes(b)

    def duel(attacker_ally, target_ally, atk_type=None, tgt_status=None):
        ca, ct = r.randrange(4), r.randrange(4)
        if attacker_ally and target_ally and ca == ct:
            ct = (ca + 1) % 4
        ia = ca if attacker_ally else 8 + r.randrange(4)
        it = ct if target_ally else 12 + r.randrange(4)
        battlers = {ia: fighter(attacker_ally, ca), it: fighter(target_ally, ct)}
        if atk_type is not None:
            battlers[ia] = set_fields(battlers[ia], {B_CURRENT_ACTION: (r.choice(by_type[atk_type]), 2)})
        if tgt_status is not None:
            group, value = tgt_status
            battlers[it] = set_fields(battlers[it], {B_AFFLICTIONS + group: (value, 1)})
        chars = {}
        if attacker_ally:
            chars[ca] = o.party_char(ca)
        if target_ally:
            chars[ct] = o.party_char(ct)
        return {"atk": ia, "tgt": it, "battlers": battlers, "chars": chars}

    attacks = [
        ("BTLACT_BASH", 40),
        ("BTLACT_SHOOT", 15),
        ("BTLACT_LEVEL_1_ATK", 10),
        ("BTLACT_LEVEL_3_ATK", 10),
        ("BTLACT_LEVEL_4_ATK", 10),
    ]
    for label, n in attacks:
        for i in range(n):
            # mostly party vs enemy, some enemy vs party
            kw = duel(attacker_ally=i % 4 != 3, target_ally=i % 4 == 3)
            rec.case(label, label, func=o.g.sym(label), rng=seed(), **kw)
    # status ailments and PSI: the inflicting actions (resist checks, luck rolls, texts)
    status_actions = [
        "BTLACT_LEVEL_2_ATK_POISON",
        "BTLACT_LVL_2_ATK_DIAMONDIZE",
        "BTLACT_DOUBLE_BASH",
        "BTLACT_INFLICT_POISON",
        "BTLACT_CRYING",
        "BTLACT_COLD",
        "BTLACT_IMMOBILIZE",
        "BTLACT_INFLICT_SOLIDIFICATION",
        "BTLACT_DIAMONDIZE",
        "BTLACT_FEELSTRANGE",
        "BTLACT_DISTRACT",
        "BTLACT_PARALYZE",
        "BTLACT_POISON",
        "BTLACT_POSSESS",
        "BTLACT_MUSHROOMIZE",
        "BTLACT_NAUSEATE",
        "BTLACT_SOLIDIFY",
        "BTLACT_SOLIDIFY_2",
        "BTLACT_SNAKE",
        "BTLACT_MUMMY_WRAP",
        "BTLACT_PSI_FIRE_A",
        "BTLACT_PSI_FREEZE_A",
        "BTLACT_PSI_FLASH_A",
        "BTLACT_PSI_ROCKIN_A",
        "BTLACT_HYPNOSIS_A",
        "BTLACT_PARALYSIS_A",
        "BTLACT_BRAINSHOCK_A",
        "BTLACT_SHIELD_KILLER",
        "BTLACT_REDUCEOFF",
        "BTLACT_CUTGUTS",
        "BTLACT_NEUTRALIZE",
        "BTLACT_HP_SUCKER",
    ]
    for label in status_actions:
        for i in range(6):
            kw = duel(attacker_ally=i % 2 == 0, target_ally=i % 2 == 1)
            rec.case(label, label, func=o.g.sym(label), rng=seed(), **kw)
    shield_statuses = [(6, 1), (6, 2), (6, 3), (6, 4), (6, 0)]
    targeted = {
        "PSI_SHIELD_NULLIFY": lambda i: (
            {"atk_type": r.choice([ACTION_TYPE_PSI, ACTION_TYPE_PHYSICAL])} | {"tgt_status": shield_statuses[i % 5]}
        ),
        "WEAKEN_SHIELD": lambda i: {"tgt_status": shield_statuses[i % 5]},
        "HEAL_STRANGENESS": lambda i: {"tgt_status": (3, 1 if i % 4 else 0)},
        "FAIL_ATTACK_ON_NPCS": lambda i: {},
    }
    for label, extra in targeted.items():
        for i in range(10):
            kw = duel(attacker_ally=i % 2 == 0, target_ally=i % 2 == 1, **extra(i))
            if label == "FAIL_ATTACK_ON_NPCS" and i % 3 == 0:
                blob = bytearray(kw["battlers"][kw["tgt"]])
                blob[B_NPC_ID] = 1
                kw["battlers"][kw["tgt"]] = bytes(blob)
            rec.case(label, label, rng=seed(), **kw)
    for i in range(30):
        kw = duel(attacker_ally=i % 3 != 2, target_ally=i % 3 == 2)
        rec.case("MISS_CALC", "MISS_CALC", a=r.randrange(2), rng=seed(), **kw)
    for i in range(30):
        kw = duel(attacker_ally=i % 3 != 2, target_ally=i % 3 == 2)
        rec.case("SMAAAASH", "SMAAAASH", rng=seed(), **kw)
    for i in range(30):
        kw = duel(attacker_ally=i % 3 != 2, target_ally=i % 3 == 2)
        dmg = r.choice([0, 1, 2, r.randrange(0, 300), r.randrange(0x8000, 0x10000)])
        rec.case(
            "CALC_RESIST_DAMAGE",
            "CALC_RESIST_DAMAGE",
            a=dmg,
            x=r.choice([0xFF, 0xFF, 0, 13, 128, 200]),
            rng=seed(),
            **kw,
        )
    # physical hits on shields (Power shield reflects) and on sleepers (wake-up roll)
    for i in range(20):
        status = [(6, 3), (6, 4), (2, 1), (6, 3)][i % 4]
        kw = duel(attacker_ally=i % 2 == 0, target_ally=i % 2 == 1, atk_type=ACTION_TYPE_PHYSICAL, tgt_status=status)
        dmg = r.randrange(1, 300)
        rec.case("CALC_RESIST_DAMAGE", "CALC_RESIST_DAMAGE", a=dmg, x=0xFF, rng=seed(), **kw)
    return rec


# Enemy ids for random scenes (1..GIYGAS_6 except the Giygas forms, which the
# Giygas cases set up on purpose). include/constants/enemies.asm
GIYGAS_IDS = (34, 218, 219, 220, 221, 229)
GIYGAS_2 = 218
# The bears are party NPCs fighting as battlers: their KO text removes the bear
# and its item from the party with text control codes, which the C test skips.
TEDDY_BEAR, SUPER_PLUSH_BEAR = 167, 168
ENEMY_IDS = [e for e in range(1, 230) if e not in (*GIYGAS_IDS, TEDDY_BEAR, SUPER_PLUSH_BEAR)]
ENEMY_DATA_SIZE, ED_FINAL_ACTION = 94, 78  # struct enemy_data (src/game/battle.h EnemyData)

# Single-target actions: (label, target side) where side "foe" is the side opposite
# the attacker and "ally" its own side (heals, shields, buffs, items used on a member).
FOE_ACTIONS = [
    *(f"BTLACT_PSI_{p}_{lv}" for p in ("ROCKIN", "FIRE", "FREEZE", "FLASH") for lv in "BGO"),
    "BTLACT_PSI_STARSTORM_A",
    "BTLACT_PSI_STARSTORM_O",
    "BTLACT_SPY",
    "BTLACT_STEAL",
    "BTLACT_CRYING2",
    "BTLACT_REDUCEPP",
    "BTLACT_REDUCEOFFDEF",
    "BTLACT_350_FIRE_DAMAGE",
    "BTLACT_DEFENSE_DOWN_A",
    "BTLACT_MAGNET_A",
    "BTLACT_MAGNET_O",
    "BTLACT_COUNTER_PSI",
    "BTLACT_HUNGRY_HP_SUCKER",
    "BTLACT_BOTTLE_ROCKET",
    "BTLACT_BIG_BOTTLE_ROCKET",
    "BTLACT_MULTI_BOTTLE_ROCKET",
    "BTLACT_HANDBAG_STRAP",
    "BTLACT_BOMB",
    "BTLACT_SUPER_BOMB",
    "BTLACT_YOGURT_DISPENSER",
    "BTLACT_BAG_OF_DRAGONITE",
    "BTLACT_INSECTICIDE_SPRAY",
    "BTLACT_XTERMINATOR_SPRAY",
    "BTLACT_RUST_PROMOTER",
    "BTLACT_RUST_PROMOTER_DX",
    "BTLACT_FLY_HONEY",
    "BTLACT_BASH",
    "BTLACT_LEVEL_2_ATK_POISON",
    "BTLACT_PSI_FIRE_A",
]
ALLY_ACTIONS = [
    *(f"BTLACT_{p}_{lv}" for p in ("LIFEUP", "HEALING") for lv in "ABGO"),
    "BTLACT_SHIELD_A",
    "BTLACT_SHIELD_B",
    "BTLACT_PSI_SHIELD_A",
    "BTLACT_PSI_SHIELD_B",
    "BTLACT_OFFENSE_UP_A",
    "BTLACT_HP_RECOVERY_1D4",
    "BTLACT_HP_RECOVERY_10",
    "BTLACT_HP_RECOVERY_50",
    "BTLACT_HP_RECOVERY_100",
    "BTLACT_HP_RECOVERY_200",
    "BTLACT_HP_RECOVERY_300",
    "BTLACT_HP_RECOVERY_10000",
    "BTLACT_PP_RECOVERY_20",
    "BTLACT_PP_RECOVERY_80",
    "BTLACT_IQ_UP_1D4",
    "BTLACT_GUTS_UP_1D4",
    "BTLACT_SPEED_UP_1D4",
    "BTLACT_VITALITY_UP_1D4",
    "BTLACT_LUCK_UP_1D4",
    "BTLACT_RANDOM_STAT_UP_1D4",
    "BTLACT_SUDDEN_GUTS_PILL",
    "BTLACT_DEFENSE_SPRAY",
    "BTLACT_DEFENSE_SHOWER",
]
# Multi-target actions that pick their own targets from BATTLER_TARGET_FLAGS
MULTI_ACTIONS = [*(f"BTLACT_PSI_THUNDER_{lv}" for lv in "ABGO"), "BTLACT_FREEZETIME", "BTLACT_PRAY"]
# Statuses a scene battler may start with: (group, value), include/constants/battle.asm
SCENE_STATUSES = [
    (0, 1),  # unconscious
    (0, 2),  # diamondized
    (0, 3),  # numb
    (0, 5),  # poisoned
    (1, 1),  # mushroomized
    (2, 1),  # asleep
    (2, 2),  # crying
    (3, 1),  # strange
    (4, 1),  # can't concentrate
    (6, 1),  # shield
    (6, 2),  # shield
    (6, 3),  # psi shield
    (6, 4),  # psi shield power
]


# Globals every scene case starts at 0 and compares afterwards (2 bytes unless listed)
TRACKED = {
    "BATTLER_TARGET_FLAGS": 4,
    "GIYGAS_PHASE": 2,
    "MIRROR_ENEMY": 2,
    "MIRROR_TURN_TIMER": 2,
    "SPECIAL_DEFEAT": 2,
    "ENEMY_PERFORMING_FINAL_ATTACK": 2,
    "SKIP_DEATH_TEXT_AND_CLEANUP": 2,
    "SHIELD_HAS_NULLIFIED_DAMAGE": 2,
    "DAMAGE_IS_REFLECTED": 2,
    "IS_SMAAAAASH_ATTACK": 2,
    "ITEM_DROPPED": 2,
    "BATTLE_MODE_FLAG": 2,
    "BATTLE_EXP_SCRATCH": 4,
    "BATTLE_MONEY_SCRATCH": 2,
}
# Actions with special effects (transformations, summons, prayers, escapes)
SPECIAL_ACTIONS = [
    "BTLACT_MIRROR",
    "BTLACT_SOW_SEEDS",
    "BTLACT_CALL_FOR_HELP",
    "BTLACT_ENEMYEXTEND",
    "BTLACT_RAINBOW_OF_COLOURS",
    "BTLACT_CLUMSYDEATH",
    "BTLACT_POSSESS",
    "BTLACT_DIAMONDIZE",
    "BTLACT_LVL_2_ATK_DIAMONDIZE",
]
# (BTLACT_GIYGAS_PRAYER_1..9 are left out: their cutscene texts run text scripts that
# move the party, and their fades tick the windows, whose RAND the oracle NOPs.)
GIYGAS_ACTIONS = ["BTLACT_POKEY_SPEECH", "BTLACT_POKEY_SPEECH_2"]


class Scenes:
    """Battles built from a save's own party and real enemies.

    The battlers come from BATTLE_INIT_PLAYER_STATS / BATTLE_INIT_ENEMY_STATS run
    in the ROM, and those calls are recorded as cases too.
    """

    def __init__(self, o, rec, r):
        self.o, self.rec, self.r = o, rec, r
        self.gs = o.game_state()
        self.party = [c for c in self.gs[GS_PARTY_MEMBERS : GS_PARTY_MEMBERS + 6] if 1 <= c <= 4]
        self.chars = {c - 1: o.party_char(c - 1) for c in range(1, 5)}
        o.core.unserialize(o.base)
        flags = o.g.sym("EVENT_FLAGS")
        self.flags = bytes(o.g.w8(flags + k) for k in range(EVENT_FLAG_BYTES))
        self.players = {}
        for slot, c in enumerate(self.party):
            _, _, b_out, _ = rec.case(
                "BATTLE_INIT_PLAYER_STATS",
                "BATTLE_INIT_PLAYER_STATS",
                a=c,
                x=o.battler_addr(slot) & 0xFFFF,
                rng=self.seed(),
                battlers={slot: bytes(BATTLER_SIZE)},
                chars=dict(self.chars),
                game_state=self.gs,
            )
            self.players[slot] = b_out[slot]
        self.enemies = {}
        table = o.g.sym("BATTLE_ACTION_TABLE")
        # Master Barf's final action adds Poo to the party (ADD_CHAR_TO_PARTY spawns
        # his overworld entity, and the texts teach Starstorm), which depends on
        # overworld state the C test has no copy of: leave those enemies out.
        barf = o.g.sym("BTLACT_MASTERBARFDEATH")
        config = o.g.sym("ENEMY_CONFIGURATION_TABLE")

        def final_fn(eid):
            act = o.g.rom8(config + eid * ENEMY_DATA_SIZE + ED_FINAL_ACTION)
            act |= o.g.rom8(config + eid * ENEMY_DATA_SIZE + ED_FINAL_ACTION + 1) << 8
            e = table + act * 12 + 8
            return o.g.rom8(e) | o.g.rom8(e + 1) << 8 | o.g.rom8(e + 2) << 16

        self.enemy_ids = [e for e in ENEMY_IDS if final_fn(e) != barf]
        self.action_of = {}
        for act in range(ACTION_COUNT):
            fn = o.g.rom8(table + act * 12 + 8) | o.g.rom8(table + act * 12 + 9) << 8
            fn |= o.g.rom8(table + act * 12 + 10) << 16
            self.action_of.setdefault(fn, act)

    def seed(self):
        return (self.r.randrange(0x10000), self.r.randrange(0x10000))

    def enemy(self, eid, idx):
        if eid not in self.enemies:
            _, _, b_out, _ = self.rec.case(
                "BATTLE_INIT_ENEMY_STATS",
                "BATTLE_INIT_ENEMY_STATS",
                a=eid,
                x=self.o.battler_addr(idx) & 0xFFFF,
                rng=self.seed(),
                battlers={idx: bytes(BATTLER_SIZE)},
                game_state=self.gs,
            )
            self.enemies[eid] = b_out[idx]
        return self.enemies[eid]

    def build(self, n_enemies=None, enemy_ids=None, statuses=0.3, low_hp=0.3):
        """A battle: the party in battlers 0.., ``n_enemies`` enemies from 8."""
        r = self.r
        battlers = {}
        for slot, blob in self.players.items():
            battlers[slot] = self.vary(blob, statuses, low_hp)
        n = n_enemies or r.randrange(1, 5)
        ids = enemy_ids or [r.choice(self.enemy_ids) for _ in range(n)]
        for k, eid in enumerate(ids):
            blob = bytearray(self.enemy(eid, FIRST_ENEMY + k))
            blob[B_ROW] = 0 if r.random() < 0.7 else 1
            blob[B_SPRITE_X] = 48 + 40 * k  # laid out left to right (SORT_BATTLERS_INTO_ROWS orders by x)
            battlers[FIRST_ENEMY + k] = self.vary(bytes(blob), statuses, low_hp)
        return battlers

    def vary(self, blob, statuses, low_hp):
        r = self.r
        b = bytearray(blob)
        hp_max = int.from_bytes(b[B_HP_MAX : B_HP_MAX + 2], "little") or 1
        hp = r.randrange(1, min(hp_max, 30) + 1) if r.random() < low_hp else r.randrange(1, hp_max + 1)
        b[B_HP : B_HP + 2] = hp.to_bytes(2, "little")
        b[B_HP_TARGET : B_HP_TARGET + 2] = hp.to_bytes(2, "little")
        for _ in range(2):
            if r.random() < statuses:
                group, value = r.choice(SCENE_STATUSES)
                b[B_AFFLICTIONS + group] = value
        if r.random() < 0.3:  # odd stats: the edges of the formulas
            for off in (B_OFFENSE, B_DEFENSE, B_SPEED, B_GUTS, B_LUCK):
                v = r.choice([0, 1, 2, 255, 256, 999, r.randrange(0, 1000)])
                b[off : off + 2] = v.to_bytes(2, "little")
            for off in range(B_VITALITY, B_HYPNOSIS_RESIST + 1):
                b[off] = r.choice([0, 1, 255, r.randrange(256)])
            b[B_GUARDING] = r.choice([0, 1])
            b[B_SHIELD_HP] = r.randrange(0, 4)
        if r.random() < 0.1:  # any bytes from guarding to the resistances
            for off in range(B_GUARDING, B_HYPNOSIS_RESIST + 1):
                b[off] = r.randrange(256)
        if r.random() < 0.2:
            pp_max = int.from_bytes(b[B_PP_MAX : B_PP_MAX + 2], "little")
            pp = r.randrange(0, pp_max + 1)
            b[B_PP : B_PP + 2] = pp.to_bytes(2, "little")
            b[B_PP_TARGET : B_PP_TARGET + 2] = pp.to_bytes(2, "little")
        return bytes(b)

    def case(self, label, battlers, atk, tgt, wram=None, **kw):
        fn = self.o.g.sym(label)
        b = bytearray(battlers[atk])
        if fn in self.action_of:
            b[B_CURRENT_ACTION : B_CURRENT_ACTION + 2] = self.action_of[fn].to_bytes(2, "little")
        battlers = dict(battlers)
        battlers[atk] = bytes(b)
        return self.rec.case(
            label,
            label,
            func=fn if label.startswith("BTLACT_") else None,
            rng=self.seed(),
            atk=atk,
            tgt=tgt,
            battlers=battlers,
            chars=dict(self.chars),
            game_state=self.gs,
            wram={"EVENT_FLAGS": self.flags, **{n: bytes(k) for n, k in TRACKED.items()}, **(wram or {})},
            **kw,
        )

    def sides(self, battlers):
        allies = [i for i in battlers if i < FIRST_ENEMY]
        foes = [i for i in battlers if i >= FIRST_ENEMY]
        return allies, foes


def target_flags(indices):
    v = 0
    for i in indices:
        v |= 1 << i
    return {"BATTLER_TARGET_FLAGS": v.to_bytes(4, "little")}


def record_scenes(o, rec, r, per_action=3):
    """Cases built from the save's party and real enemies (see Scenes)."""
    sc = Scenes(o, rec, r)
    if not sc.party:
        return
    for _ in range(12):  # more enemy inits (their RNG-rolled fields)
        sc.enemy(r.choice(sc.enemy_ids), FIRST_ENEMY)
        sc.enemies.clear()

    def pick(battlers, label_side):
        allies, foes = sc.sides(battlers)
        party_turn = r.random() < 0.6
        atk = r.choice(allies if party_turn else foes)
        same = allies if party_turn else foes
        other = foes if party_turn else allies
        tgt = r.choice(same if label_side == "ally" else other)
        return atk, tgt, same, other

    for label in FOE_ACTIONS:
        for _ in range(per_action):
            battlers = sc.build()
            atk, tgt, _, _ = pick(battlers, "foe")
            sc.case(label, battlers, atk, tgt)
    for label in ALLY_ACTIONS:
        for _ in range(per_action):
            battlers = sc.build()
            atk, tgt, _, _ = pick(battlers, "ally")
            sc.case(label, battlers, atk, tgt)
    for label in MULTI_ACTIONS:
        for _ in range(per_action + 2):
            battlers = sc.build(n_enemies=r.randrange(1, 5))
            atk, tgt, same, other = pick(battlers, "foe")
            sc.case(label, battlers, atk, tgt, wram=target_flags(other))
    for label in SPECIAL_ACTIONS:
        for _ in range(per_action + 2):
            battlers = sc.build()
            atk, tgt, same, other = pick(battlers, "foe")
            b = bytearray(battlers[atk])
            b[B_CURRENT_ACTION_ARGUMENT] = r.randrange(0, 16)  # item slot / argument
            battlers[atk] = bytes(b)
            sc.case(label, battlers, atk, tgt, wram=target_flags([tgt]))
    # Equipment changes in battle (a party member; the argument is the item slot)
    for label in ("BTLACT_SWITCH_WEAPONS", "BTLACT_SWITCH_ARMOR"):
        for _ in range(per_action + 2):
            battlers = sc.build()
            allies, foes = sc.sides(battlers)
            atk = r.choice(allies)
            b = bytearray(battlers[atk])
            items = sc.chars[b[B_ID] - 1][C_ITEMS : C_ITEMS + 14]
            slots = [k + 1 for k, it in enumerate(items) if it] or [1]
            slot = r.choice(slots)
            b[B_ACTION_ITEM_SLOT] = slot  # the menu's choice: a filled slot and its item
            b[B_CURRENT_ACTION_ARGUMENT] = items[slot - 1]
            battlers[atk] = bytes(b)
            sc.case(label, battlers, atk, atk)
    # The Giygas fight: prayers and Pokey's speeches move GIYGAS_PHASE on
    for label in GIYGAS_ACTIONS:
        for _ in range(per_action):
            battlers = sc.build(enemy_ids=[r.choice(GIYGAS_IDS[1:])], statuses=0.1)
            allies, foes = sc.sides(battlers)
            phase = {"GIYGAS_PHASE": r.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]).to_bytes(2, "little")}
            sc.case(label, battlers, r.choice(allies), foes[0], wram=phase)
    # CALC_DAMAGE: mortal damage and the guts save on party members, the Giygas
    # redirect (a hit on Giygas lands on a random conscious member), the final-attack
    # survival rule; A = target, X = damage
    for i in range(per_action * 6):
        giygas = i % 3 == 0
        battlers = sc.build(enemy_ids=[GIYGAS_2] if giygas else None, low_hp=0.6)
        allies, foes = sc.sides(battlers)
        if giygas:
            atk, tgt = r.choice(allies), foes[0]
        else:
            atk, tgt, _, _ = pick(battlers, "foe")
        dmg = r.choice([1, 5, 30, 100, 400, r.randrange(1, 1000)])
        final = {"ENEMY_PERFORMING_FINAL_ATTACK": bytes([r.random() < 0.3, 0])}
        sc.case("CALC_DAMAGE", battlers, atk, tgt, a=o.battler_addr(tgt) & 0xFFFF, x=dmg, wram=final)
    # CALC_RESIST_DAMAGE with KOs: low-HP targets on both sides
    for _ in range(per_action * 4):
        battlers = sc.build(low_hp=0.9)
        atk, tgt, _, _ = pick(battlers, "foe")
        dmg = r.choice([5, 30, 100, r.randrange(1, 500)])
        sc.case("CALC_RESIST_DAMAGE", battlers, atk, tgt, a=dmg, x=0xFF)
    # CHOOSE_TARGET: the AI's target pick for the attacker's current action (A =
    # attacker); FRONT/BACK_ROW_BATTLERS list the enemies by row
    for i in range(per_action * 5):
        battlers = sc.build()
        allies, foes = sc.sides(battlers)
        atk = r.choice(foes if i % 4 else allies)
        b = bytearray(battlers[atk])
        b[B_CURRENT_ACTION : B_CURRENT_ACTION + 2] = r.randrange(0, ACTION_COUNT).to_bytes(2, "little")
        battlers[atk] = bytes(b)
        front = [k for k in foes if battlers[k][B_ROW] == 0]
        back = [k for k in foes if battlers[k][B_ROW] != 0]
        rows = {
            "FRONT_ROW_BATTLERS": bytes(front + [0] * (8 - len(front))),
            "NUM_BATTLERS_IN_FRONT_ROW": len(front).to_bytes(2, "little"),
            "BACK_ROW_BATTLERS": bytes(back + [0] * (8 - len(back))),
            "NUM_BATTLERS_IN_BACK_ROW": len(back).to_bytes(2, "little"),
            "BATTLER_TARGET_FLAGS": bytes(4),
        }
        rec.case(
            "CHOOSE_TARGET",
            "CHOOSE_TARGET",
            a=o.battler_addr(atk) & 0xFFFF,
            rng=sc.seed(),
            battlers=battlers,
            chars=dict(sc.chars),
            game_state=sc.gs,
            wram={"EVENT_FLAGS": sc.flags, **rows},
        )


# Saves the scene cases are recorded from (party size and enemy levels vary)
SCENE_SAVES = ["05-Twoson", "12-Zombies", "26-Dalaam", "36-CavePast"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--save", default=str(ROOT / "playtest" / "saves" / "26-Dalaam.srm"))
    ap.add_argument("--scene-saves", nargs="*", default=SCENE_SAVES, help="saves for the scene cases")
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--seed", type=int, default=2)
    ap.add_argument("--per-action", type=int, default=3, help="scene cases per action")
    ap.add_argument("--scenes-only", action="store_true", help="skip the fixed-input cases (for debugging)")
    a = ap.parse_args()
    t0 = time.time()
    lines, count, skipped, lets = [], 0, 0, {}
    o = Oracle(a.save)
    bt = o.bt
    if not a.scenes_only:
        rec = record_all(o, random.Random(a.seed))  # noqa: S311 (test inputs)
        lines += rec.lines
        count += rec.count
    for k, name in enumerate(a.scene_saves):
        save = ROOT / "playtest" / "saves" / f"{name}.srm"
        o = Oracle(save)
        rec = Recorder(o, lets)
        record_scenes(o, rec, random.Random(a.seed * 1000 + k), a.per_action)  # noqa: S311 (test inputs)
        lines += [f"# scenes from save {save.name}", *rec.lines]
        count += rec.count
        skipped += rec.skipped
    header = [
        "# Battle-math cases recorded from the original ROM by tests/c/battle_oracle.py",
        f"# (save {Path(a.save).name}, seed {a.seed}; scenes from {', '.join(a.scene_saves)}).",
        "# Regenerate with that script; do not edit by hand.",
        f"battlers_table {bt & 0xFFFF:x}",
    ]
    Path(a.out).write_text("\n".join(header + lines) + "\n")
    print(f"{count} cases ({skipped} skipped: never returned) -> {a.out} in {time.time() - t0:.1f}s")


if __name__ == "__main__":
    main()
