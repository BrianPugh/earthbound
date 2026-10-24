"""Reference SNES emulator for the playtest harness: run the ORIGINAL game.

A drop-in stand-in for the C port binary in playtest sessions. It loads the
snes9x libretro core (built by ``playtest/emu/build.sh``) with ctypes, boots the
retail ROM with a playtest ``.srm`` in SRAM, and plays the same command
language as ``port/unix/platform/sdl2_input_script.c``, printing the same kind
of lines (STATE, PARTY, SHOT, UNTIL, SYNC, ...). ``pt.py start NAME --emu``
runs it in place of ``$EB_BIN``; it accepts the binary's flags::

    python playtest/emu.py --headless --save save.srm --session cmd.fifo
    python playtest/emu.py --headless --save save.srm --input script.play \
        [--record-video FIFO] [--record-audio FILE] [--record-paused]

Game state is read from WRAM at the addresses in ``build/earthbound.map``
(exported symbols) plus the top-level labels of the ``build/US/*.lst``
listings (for code addresses), so ``make`` must have been run. The original has
no mode stack: ``modes=`` in STATE is the asm call chain recovered from the
65816 hardware stack (JSL/JSR return addresses), and ``idle``/``input`` are
derived from it (the overworld ``MAIN_LOOP`` frame wait, or one of the
input-wait loops that tick ``UPDATE_HPPP_METER_AND_RENDER``).

``teleport``/``warp`` run the asm's own ``INITIALIZE_MAP`` (as the PSI Teleport
arrival does) by diverting the CPU, while it idles in the main loop's frame
wait, through a small 65816 stub written to unused WRAM.
"""

import argparse
import bisect
import contextlib
import ctypes as C  # noqa: N812
import json
import os
import re
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CORE_DIR = HERE / "emu" / "build"
MAP_FILE = ROOT / "build" / "earthbound.map"
LST_DIR = ROOT / "build" / "US"
SYM_CACHE = HERE / "out" / "emu-symbols.json"

WIDTH, HEIGHT = 256, 224
AUDIO_RATE = 32040

# libretro joypad ids, by the script's button names
BUTTON_IDS = {
    "B": 0,
    "Y": 1,
    "SELECT": 2,
    "START": 3,
    "UP": 4,
    "DOWN": 5,
    "LEFT": 6,
    "RIGHT": 7,
    "A": 8,
    "X": 9,
    "L": 10,
    "R": 11,
}
DIR_NAMES = ("UP", "UP_RIGHT", "RIGHT", "DOWN_RIGHT", "DOWN", "DOWN_LEFT", "LEFT", "UP_LEFT")

# Struct layouts (include/structs.asm, USA)
GS_MONEY = 60
GS_BANK_BALANCE = 64
GS_PARTY_STATUS = 75
GS_PARTY_MEMBERS = 122
GS_LEADER_X = 130
GS_LEADER_Y = 134
GS_LEADER_DIR = 138
GS_TRODDEN_TILE = 140
GS_WALKING_STYLE = 142
GS_CURRENT_PARTY_MEMBERS = 148
GS_PARTY_ORDER = 150
GS_PARTY_ENTITY_SLOTS = 162
GS_PARTY_COUNT = 174
GS_CAMERA_MODE = 176
CHAR_SIZE = 95
CH_NAME, CH_LEVEL, CH_MAX_HP, CH_MAX_PP, CH_AFFLICTIONS, CH_ITEMS = 0, 5, 10, 12, 14, 35
CH_EXP = 6
CH_HP, CH_HP_TARGET, CH_PP, CH_PP_TARGET = 69, 71, 75, 77
WINDOW_STATS_SIZE = 82
WS_ID, WS_X, WS_Y, WS_W, WS_H, WS_TEXT_X, WS_TEXT_Y = 4, 6, 8, 10, 12, 14, 16
WS_CURRENT_OPTION, WS_OPTION_COUNT, WS_TITLE = 43, 45, 60
MENU_OPTION_SIZE = 45
MO_NEXT, MO_TEXT_X, MO_TEXT_Y, MO_LABEL = 2, 8, 10, 19
WINDOW_MAX = 53
BATTLER_SIZE = 78
BT_ID, BT_CONSCIOUSNESS, BT_ALLY_OR_ENEMY, BT_NPC_ID, BT_ROW = 0, 12, 14, 15, 16
BT_HP, BT_HP_TARGET, BT_HP_MAX, BT_PP, BT_PP_TARGET = 17, 19, 21, 23, 25
FIRST_ENEMY_INDEX, BATTLER_COUNT = 8, 32
ENEMY_CONFIG_SIZE, ENEMY_NAME, ENEMY_NAME_LEN = 94, 1, 25
MAX_ENTITIES, PARTY_LEADER_ENTITY_INDEX = 30, 24
OBJECT_TICK_DISABLED, OBJECT_MOVE_DISABLED = 0x8000, 0x4000
WALKING_STYLE_ESCALATOR, WALKING_STYLE_STAIRS = 12, 13
PSI_TELEPORT_ENTRY_SIZE, PSI_TELEPORT_NAME_LEN = 31, 25
EVENT_FLAG_COUNT = 1024
ITEM_SLOTS = 14

STATUS_NAMES = (
    (None, "unconscious", "diamondized", "paralyzed", "nauseous", "poisoned", "sunstroke", "cold"),
    (None, "mushroomized", "possessed"),
    (None, "asleep", "crying", "immobilized", "solidified"),
    (None, "strange"),
    (None, "cant-concentrate", None, None, "cant-concentrate4"),
    (None, "homesick"),
    (None, "psi-shield-power", "psi-shield", "shield-power", "shield"),
)

# EB text codes 0x50-0xAF (src/game/text.c eb_char_to_ascii)
EB_CHARS = " !&{$%}'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ~^[]#_abcdefghijklmnopqrstuvwxyz?|???"

# The input-wait loops: their JSL UPDATE_HPPP_METER_AND_RENDER return sites mean
# "a menu / prompt waits for a button" (TEXT_SPEED_DELAY etc. are excluded)
INPUT_WAIT_FUNCS = {
    "CC_13_14": "TEXT_PROMPT",
    "SELECTION_MENU": "SELECTION_MENU",
    "NUM_SELECT_PROMPT": "NUMBER_SELECT",
    "CHAR_SELECT_PROMPT": "CHAR_SELECT",
    "PARTY_CHARACTER_SELECTOR": "CHAR_SELECT",
    "SELECT_BATTLE_TARGET": "BATTLE_ENEMY_SELECT",
    "SELECT_BATTLE_ROW": "BATTLE_ROW_SELECT",
}
# MAIN_LOOP's per-frame calls (asm/system/main.asm @LOOP_BEGIN)
MAIN_LOOP_FRAME_CALLS = (
    "OAM_CLEAR",
    "RUN_ACTIONSCRIPT_FRAME",
    "UPDATE_SCREEN",
    "UPDATE_SWIRL_EFFECT",
    "WAIT_UNTIL_NEXT_FRAME",
)
# C-port mode names accepted by in:/out:, as asm functions on the call chain
MODE_ALIASES = {
    "TEXT_PROMPT": ("CC_13_14",),
    "SELECTION_MENU": ("SELECTION_MENU",),
    "NUMBER_SELECT": ("NUM_SELECT_PROMPT",),
    "CHAR_SELECT": ("CHAR_SELECT_PROMPT", "PARTY_CHARACTER_SELECTOR"),
    "BATTLE_ENEMY_SELECT": ("SELECT_BATTLE_TARGET",),
    "BATTLE_ROW_SELECT": ("SELECT_BATTLE_ROW",),
    "OVERWORLD": ("MAIN_LOOP",),
}

# Unused WRAM for the teleport stub: the bottom of the 255-byte hardware stack
# (STACK_65816_START). The game's call depth stays far above it.
STUB_ADDR = 0x7E1F00
STUB_LIMIT = 0x60


def log(*args):
    """Print a line and flush (the session driver tails stdout)."""
    print(*args, flush=True)


def eb_text(data):
    """Decode EB-encoded bytes (up to a 0 terminator) to ASCII."""
    out = []
    for b in data:
        if b == 0:
            break
        out.append(EB_CHARS[b - 0x50] if 0x50 <= b <= 0xAF else "?")
    return "".join(out).rstrip()


# ---------------------------------------------------------------- symbols


def _cache_key():
    files = [MAP_FILE, *sorted(LST_DIR.glob("*.lst"))]
    return [[str(f), f.stat().st_mtime_ns] for f in files]


def load_symbols():
    """Symbol addresses from the linker map plus the listings' top-level labels.

    Returns
    -------
    dict[str, int]
        Label name -> 24-bit SNES address. Cached in ``playtest/out``.
    """
    if not MAP_FILE.exists():
        sys.exit(f"{MAP_FILE} not found: run `make` first (emu mode reads the asm symbols)")
    key = _cache_key()
    try:
        cached = json.loads(SYM_CACHE.read_text())
        if cached["key"] == key:
            return cached["symbols"]
    except (OSError, ValueError, KeyError):
        pass

    text = MAP_FILE.read_text()
    syms = {}
    segs = {}
    seg_part = text.split("Segment list:", 1)[1].split("Exports list", 1)[0]
    for m in re.finditer(r"^(\w+)\s+([0-9A-F]{6})\s+([0-9A-F]{6})", seg_part, re.M):
        segs[m.group(1)] = int(m.group(2), 16)
    exports = text.split("Exports list by name:", 1)[1].split("Exports list by value:", 1)[0]
    for m in re.finditer(r"(\S+)\s+([0-9A-F]{6}) R\w\w", exports):
        syms[m.group(1)] = int(m.group(2), 16)

    label = re.compile(r"([A-Za-z_][A-Za-z0-9_]*):")
    segment = re.compile(r'\s*\.SEGMENT\s+"(\w+)"')
    for lst in sorted(LST_DIR.glob("*.lst")):
        seg = None
        with lst.open(errors="replace") as f:
            for ln in f:
                if len(ln) < 25 or ln[6] != "r":
                    continue
                src = ln[24:]
                m = segment.match(src)
                if m:
                    seg = m.group(1)
                    continue
                if seg is None or not seg.startswith("BANK") or seg not in segs:
                    continue
                m = label.match(src)
                if m and m.group(1) not in syms:
                    syms[m.group(1)] = segs[seg] + int(ln[:6], 16)
    SYM_CACHE.parent.mkdir(parents=True, exist_ok=True)
    SYM_CACHE.write_text(json.dumps({"key": key, "symbols": syms}))
    return syms


# ---------------------------------------------------------------- libretro


class GameInfo(C.Structure):
    """``struct retro_game_info``."""

    _fields_ = [("path", C.c_char_p), ("data", C.c_void_p), ("size", C.c_size_t), ("meta", C.c_char_p)]


ENV_CB = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VIDEO_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUDIO_CB = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDIO_BATCH_CB = C.CFUNCTYPE(C.c_size_t, C.POINTER(C.c_int16), C.c_size_t)
POLL_CB = C.CFUNCTYPE(None)
INPUT_CB = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)

RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_SYSTEM_RAM = 0, 2
ENV_GET_CAN_DUPE, ENV_SET_PIXEL_FORMAT = 3, 10
ENV_GET_SYSTEM_DIRECTORY, ENV_GET_SAVE_DIRECTORY = 9, 31
ENV_GET_INPUT_BITMASKS = 51 | 0x10000
JOYPAD_MASK = 256


def core_path():
    """The libretro core to load (``$EB_EMU_CORE`` or the build.sh output)."""
    env = os.environ.get("EB_EMU_CORE")
    if env:
        return Path(env)
    for ext in ("dylib", "so"):
        p = CORE_DIR / f"snes9x_libretro.{ext}"
        if p.exists():
            return p
    sys.exit(f"no libretro core in {CORE_DIR}: run playtest/emu/build.sh")


def rom_path():
    """The ROM to boot (``$EB_ROM``, else earthbound.sfc or build/earthbound.sfc)."""
    env = os.environ.get("EB_ROM")
    for p in ([Path(env)] if env else []) + [ROOT / "earthbound.sfc", ROOT / "build" / "earthbound.sfc"]:
        if p.exists():
            return p
    sys.exit("no ROM: put the US retail ROM at earthbound.sfc (or set EB_ROM)")


class Core:
    """A libretro core driven one frame at a time.

    Parameters
    ----------
    rom : Path
        The SNES ROM image.
    """

    def __init__(self, rom):
        lib = C.CDLL(str(core_path()))
        self.lib = lib
        lib.retro_load_game.restype = C.c_bool
        lib.retro_get_memory_data.restype = C.c_void_p
        lib.retro_get_memory_size.restype = C.c_size_t
        lib.retro_serialize_size.restype = C.c_size_t
        lib.retro_serialize.restype = C.c_bool
        lib.retro_unserialize.restype = C.c_bool
        self.pad = 0
        self.frame = None  # (bytes, width, height, pitch) of the last rendered frame
        self.audio = []  # s16le stereo chunks since the last take_audio()
        self.record_audio = False
        self._sysdir = C.create_string_buffer(str(CORE_DIR).encode())
        # Keep the callbacks alive: ctypes frees them otherwise
        self._cbs = [
            ENV_CB(self._env),
            VIDEO_CB(self._video),
            AUDIO_CB(self._audio),
            AUDIO_BATCH_CB(self._audio_batch),
            POLL_CB(lambda: None),
            INPUT_CB(self._input),
        ]
        lib.retro_set_environment(self._cbs[0])
        lib.retro_init()
        lib.retro_set_video_refresh(self._cbs[1])
        lib.retro_set_audio_sample(self._cbs[2])
        lib.retro_set_audio_sample_batch(self._cbs[3])
        lib.retro_set_input_poll(self._cbs[4])
        lib.retro_set_input_state(self._cbs[5])
        data = Path(rom).read_bytes()
        self.rom = data
        self._rom_buf = C.create_string_buffer(data, len(data))
        info = GameInfo(str(rom).encode(), C.cast(self._rom_buf, C.c_void_p), len(data), None)
        if not lib.retro_load_game(C.byref(info)):
            sys.exit(f"the core could not load {rom}")
        self.wram = self._memory(RETRO_MEMORY_SYSTEM_RAM)
        self.sram = self._memory(RETRO_MEMORY_SAVE_RAM)

    def _memory(self, kind):
        ptr = self.lib.retro_get_memory_data(kind)
        size = self.lib.retro_get_memory_size(kind)
        return (C.c_uint8 * size).from_address(ptr)

    def _env(self, cmd, data):
        if cmd == ENV_SET_PIXEL_FORMAT:
            return C.cast(data, C.POINTER(C.c_int))[0] == 2  # RGB565
        if cmd == ENV_GET_CAN_DUPE:
            C.cast(data, C.POINTER(C.c_bool))[0] = True
            return True
        if cmd in (ENV_GET_SYSTEM_DIRECTORY, ENV_GET_SAVE_DIRECTORY):
            C.cast(data, C.POINTER(C.c_void_p))[0] = C.cast(self._sysdir, C.c_void_p)
            return True
        return cmd == ENV_GET_INPUT_BITMASKS

    def _video(self, data, w, h, pitch):
        if data:  # NULL = frame duped
            self.frame = (C.string_at(data, pitch * h), w, h, pitch)

    def _audio(self, left, right):
        if self.record_audio:
            self.audio.append(np.array([left, right], dtype=np.int16).tobytes())

    def _audio_batch(self, data, frames):
        if self.record_audio:
            self.audio.append(C.string_at(data, frames * 4))
        return frames

    def _input(self, port, device, index, button):
        if port != 0:
            return 0
        if button == JOYPAD_MASK:
            return self.pad
        return (self.pad >> button) & 1

    def run(self, pad):
        """Run one frame with ``pad`` (libretro joypad bitmask) held."""
        self.pad = pad
        self.lib.retro_run()

    def take_audio(self):
        """Return and clear the audio recorded since the last call."""
        out = b"".join(self.audio)
        self.audio.clear()
        return out

    def rgb(self):
        """The last frame as a ``(H, W, 3)`` uint8 array (256x224)."""
        if self.frame is None:
            return np.zeros((HEIGHT, WIDTH, 3), np.uint8)
        data, w, h, pitch = self.frame
        a = np.frombuffer(data, dtype=np.uint16).reshape(h, pitch // 2)[:, :w]
        if w >= 2 * WIDTH:  # hi-res frame: keep every other column
            a = a[:, ::2]
        if h >= 2 * HEIGHT:
            a = a[::2]
        r = ((a >> 11) & 0x1F).astype(np.uint8)
        g = ((a >> 5) & 0x3F).astype(np.uint8)
        b = (a & 0x1F).astype(np.uint8)
        img = np.dstack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)])
        out = np.zeros((HEIGHT, WIDTH, 3), np.uint8)
        hh, ww = min(HEIGHT, img.shape[0]), min(WIDTH, img.shape[1])
        out[:hh, :ww] = img[:hh, :ww]
        return out

    def serialize(self):
        """The core's savestate (bytes)."""
        n = self.lib.retro_serialize_size()
        buf = C.create_string_buffer(n)
        if not self.lib.retro_serialize(buf, n):
            raise RuntimeError("retro_serialize failed")
        return buf.raw

    def unserialize(self, data):
        """Load a savestate from :meth:`serialize`."""
        buf = C.create_string_buffer(data, len(data))
        if not self.lib.retro_unserialize(buf, len(data)):
            raise RuntimeError("retro_unserialize failed")


# snes9x savestate "REG:" block: PB DB P A D S X Y PC, big-endian
REG_FIELDS = (("PB", 1), ("DB", 1), ("P", 2), ("A", 2), ("D", 2), ("S", 2), ("X", 2), ("Y", 2), ("PC", 2))


def _reg_block(state):
    i = state.find(b"REG:")
    if i < 0:
        raise RuntimeError("no REG block in the savestate")
    return i + 11  # "REG:000016:"


def read_regs(state):
    """Parse the CPU registers out of a snes9x savestate."""
    i = _reg_block(state)
    regs = {}
    for name, size in REG_FIELDS:
        regs[name] = int.from_bytes(state[i : i + size], "big")
        i += size
    return regs


def write_regs(state, regs):
    """Return ``state`` with the CPU registers in ``regs`` replaced."""
    out = bytearray(state)
    i = _reg_block(state)
    for name, size in REG_FIELDS:
        if name in regs:
            out[i : i + size] = regs[name].to_bytes(size, "big")
        i += size
    return bytes(out)


# ---------------------------------------------------------------- the game


class Game:
    """EarthBound knowledge on top of a :class:`Core`: WRAM fields and the asm call chain.

    Parameters
    ----------
    core : Core
        The running core.
    syms : dict[str, int]
        Symbols from :func:`load_symbols`.
    """

    def __init__(self, core, syms):
        self.core = core
        self.syms = syms
        rom_syms = sorted((a, n) for n, a in syms.items() if 0xC00000 <= a < 0x1000000)
        self._fn_addr = [a for a, _ in rom_syms]
        self._fn_name = [n for _, n in rom_syms]
        self.gs = syms["GAME_STATE"]
        self.chars = syms["PARTY_CHARACTERS"]
        hppp = syms["UPDATE_HPPP_METER_AND_RENDER"]
        # Return sites (address of the JSL's last byte, as pushed) of the input-wait loops
        self.input_sites = {}
        for fn, mode in INPUT_WAIT_FUNCS.items():
            for site in self.jsl_sites(fn, hppp):
                self.input_sites[site] = mode
        # The overworld main loop's per-frame calls (the last JSL of each in MAIN_LOOP,
        # i.e. the ones after @LOOP_BEGIN): a frame boundary inside any of them is a
        # plain overworld frame; one inside PROCESS_QUEUED_INTERACTIONS,
        # OPEN_MENU_BUTTON, ... is not
        self.idle_sites = set()
        for fn in MAIN_LOOP_FRAME_CALLS:
            sites = self.jsl_sites("MAIN_LOOP", syms[fn])
            if sites:
                self.idle_sites.add(sites[-1])
        self._chain = None  # cached for the current frame

    # --- memory

    def rom8(self, addr):
        """A ROM byte at SNES address ``addr`` (HiROM)."""
        off = addr - 0xC00000
        return self.core.rom[off] if 0 <= off < len(self.core.rom) else 0

    def w8(self, addr):
        """A WRAM byte (``addr`` is a 7E/7F SNES address or a WRAM offset)."""
        return self.core.wram[addr & 0x1FFFF]

    def w16(self, addr):
        """A little-endian WRAM word."""
        a = addr & 0x1FFFF
        return self.core.wram[a] | (self.core.wram[a + 1] << 8)

    def w32(self, addr):
        """A little-endian WRAM dword."""
        return self.w16(addr) | (self.w16(addr + 2) << 16)

    def put8(self, addr, v):
        """Write a WRAM byte."""
        self.core.wram[addr & 0x1FFFF] = v & 0xFF

    def put16(self, addr, v):
        """Write a WRAM word."""
        self.put8(addr, v)
        self.put8(addr + 1, v >> 8)

    def put32(self, addr, v):
        """Write a WRAM dword."""
        self.put16(addr, v)
        self.put16(addr + 2, v >> 16)

    def sym(self, name):
        """A symbol's address."""
        return self.syms[name]

    def char(self, char_id):
        """WRAM address of party character ``char_id`` (1-6)."""
        return self.chars + (char_id - 1) * CHAR_SIZE

    def function_at(self, addr):
        """(name, start) of the ROM label at or before ``addr``."""
        i = bisect.bisect_right(self._fn_addr, addr) - 1
        if i < 0:
            return "?", 0
        return self._fn_name[i], self._fn_addr[i]

    def function_end(self, start):
        """Address of the next ROM label after ``start``."""
        i = bisect.bisect_right(self._fn_addr, start)
        return self._fn_addr[i] if i < len(self._fn_addr) else start + 0x10000

    def jsl_sites(self, func, target):
        """Return addresses (as pushed: JSL + 3) of ``JSL target`` inside ``func``."""
        start = self.syms.get(func)
        if start is None:
            return []
        end = self.function_end(start)
        rom, off = self.core.rom, start & 0x3FFFFF
        pat = bytes((0x22, target & 0xFF, (target >> 8) & 0xFF, target >> 16))
        out = []
        while True:
            off = rom.find(pat, off, (end & 0x3FFFFF))
            if off < 0:
                return out
            out.append((start & 0xFF0000) | ((off + 3) & 0xFFFF))
            off += 1

    # --- the call chain

    def frame_changed(self):
        """Forget per-frame caches (call after every frame or state load)."""
        self._chain = None

    def regs(self):
        """The CPU registers (from a core savestate)."""
        return read_regs(self.core.serialize())

    def chain(self):
        """The asm call chain, outermost first, recovered from the hardware stack.

        Returns
        -------
        list[tuple[str, int]]
            (caller function name, return site as pushed) per JSL/JSR frame.
        """
        if self._chain is not None:
            return self._chain
        sp = self.regs()["S"]
        top = self.sym("STACK_65816_END") & 0xFFFF
        frames = []
        callee = None  # start of the function the next inner frame's caller must be in
        p = top
        while p > sp + 1:
            # JSL pushes PB, PCH, PCL (PCL lowest); the pushed address is the JSL's
            # last byte, so the opcode is 3 bytes back
            pb, pch, pcl = self.w8(p), self.w8(p - 1), self.w8(p - 2)
            ret = (pb << 16) | (pch << 8) | pcl
            if p - 2 > sp and 0xC0 <= pb <= 0xEF and self.rom8(ret - 3) == 0x22:
                frames.append((self.function_at(ret)[0], ret))
                callee = self.function_at(self.rom8(ret - 2) | (self.rom8(ret - 1) << 8) | (self.rom8(ret) << 16))[1]
                p -= 3
                continue
            # JSR pushes PCH, PCL: accept it only inside the function the enclosing
            # frame called (stale bytes and saved registers rarely pass that)
            if callee is not None:
                ret = (callee & 0xFF0000) | (self.w8(p) << 8) | self.w8(p - 1)
                if self.function_at(ret)[1] == callee:
                    op = self.rom8(ret - 2)
                    if op == 0x20:  # JSR abs
                        frames.append((self.function_at(ret)[0], ret))
                        target = (callee & 0xFF0000) | self.rom8(ret - 1) | (self.rom8(ret) << 8)
                        callee = self.function_at(target)[1]
                        p -= 2
                        continue
                    if op == 0xFC:  # JSR (abs,X): callee unknown
                        frames.append((self.function_at(ret)[0], ret))
                        callee = None
                        p -= 2
                        continue
            p -= 1
        self._chain = frames
        return frames

    def chain_names(self):
        """Function names on the call chain, outermost first."""
        return [n for n, _ in self.chain()]

    def wait_mode(self):
        """Which input-wait loop the CPU is in (C-port mode name), or None."""
        for _, ret in self.chain():
            if ret in self.input_sites:
                return self.input_sites[ret]
        return None

    def in_main_loop_frame(self):
        """The CPU is in one of the overworld main loop's per-frame calls."""
        for name, ret in self.chain():
            if name == "MAIN_LOOP":
                return ret in self.idle_sites
        return False

    # --- state predicates (mirror sdl2_input_script.c not_idle_reason)

    def windows_open(self):
        """Open window ids (OPEN_WINDOW_TABLE entries that are in use)."""
        t = self.sym("OPEN_WINDOW_TABLE")
        return [i for i in range(WINDOW_MAX) if self.w16(t + i * 2) != 0xFFFF]

    def not_idle_reason(self):
        """Why the player can't walk, or None."""
        if not self.in_main_loop_frame() or self.windows_open():
            return "modes"
        if self.w16(self.sym("BATTLE_MODE")) or self.w16(self.sym("BATTLE_SWIRL_COUNTDOWN")):
            return "battle"
        if self.w8(self.sym("ENEMY_HAS_BEEN_TOUCHED")):
            return "battle"
        if self.w16(self.sym("PENDING_INTERACTIONS")):
            return "interaction"
        if self.w16(self.sym("INPUT_DISABLE_FRAME_COUNTER")):
            return "input-disabled"
        hi = self.w16(self.sym("ENTITY_TICK_CALLBACK_HIGH") + PARTY_LEADER_ENTITY_INDEX * 2)
        if hi & (OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED):
            return "leader-frozen"
        ws = self.w16(self.gs + GS_WALKING_STYLE)
        if ws == WALKING_STYLE_ESCALATOR:
            return "escalator"
        if ws == WALKING_STYLE_STAIRS:
            return "stairs"
        if self.w16(self.gs + GS_CAMERA_MODE):
            return "auto-move"
        if self.w16(self.sym("DEMO_FRAMES_LEFT")) or (self.w8(self.sym("PLAYER_MOVEMENT_FLAGS")) & 0x02):
            return "auto-walk"
        inidisp = self.w8(self.sym("INIDISP_MIRROR"))
        if (inidisp & 0x80) or (inidisp & 0x0F) != 0x0F:
            return "fade"
        return None

    def waiting_for_input(self):
        """Idle, or a menu / text prompt / target select waits for a button."""
        return self.not_idle_reason() is None or self.wait_mode() is not None

    def text_prompt_waiting(self):
        """A text prompt (CC_13_14) waits for a button."""
        return self.wait_mode() == "TEXT_PROMPT"

    def in_battle(self):
        """A battle runs (BATTLE_MODE_FLAG)."""
        return bool(self.w16(self.sym("BATTLE_MODE_FLAG")))

    def on_chain(self, mode):
        """Mode (C-port name or asm function name) is on the call chain."""
        if mode == "BATTLE":
            return self.in_battle()
        names = set(MODE_ALIASES.get(mode, (mode,)))
        return any(n in names for n in self.chain_names())

    # --- flags and party

    def flag(self, n):
        """Event flag ``n`` (1-based)."""
        return (self.w8(self.sym("EVENT_FLAGS") + (n - 1) // 8) >> ((n - 1) % 8)) & 1

    def set_flag(self, n, v):
        """Set or clear event flag ``n``."""
        a = self.sym("EVENT_FLAGS") + (n - 1) // 8
        bit = 1 << ((n - 1) % 8)
        self.put8(a, (self.w8(a) | bit) if v else (self.w8(a) & ~bit))

    def party(self):
        """Party member ids (1-6) in party order."""
        n = self.w8(self.gs + GS_PARTY_COUNT)
        ids = [self.w8(self.gs + GS_PARTY_MEMBERS + i) for i in range(min(n, 6))]
        return [i for i in ids if 1 <= i <= 6]

    def party_battler(self, char_id):
        """WRAM address of a party member's battler during a battle, or None."""
        if not self.in_battle():
            return None
        bt = self.sym("BATTLERS_TABLE")
        for i in range(FIRST_ENEMY_INDEX):
            b = bt + i * BATTLER_SIZE
            if (
                self.w8(b + BT_CONSCIOUSNESS)
                and self.w8(b + BT_ALLY_OR_ENEMY) == 0
                and self.w8(b + BT_NPC_ID) == 0
                and self.w8(b + BT_ROW) == char_id - 1
            ):
                return b
        return None

    def enemy_name(self, enemy_id):
        """An enemy's name from ENEMY_CONFIGURATION_TABLE in ROM."""
        a = (self.sym("ENEMY_CONFIGURATION_TABLE") & 0x3FFFFF) + enemy_id * ENEMY_CONFIG_SIZE + ENEMY_NAME
        return eb_text(self.core.rom[a : a + ENEMY_NAME_LEN])

    def leader_entity(self):
        """The leader's entity slot."""
        v = self.w16(self.gs + GS_CURRENT_PARTY_MEMBERS)
        return v if v < MAX_ENTITIES else PARTY_LEADER_ENTITY_INDEX

    def entity16(self, table, slot):
        """A word from an entity table (asm tables are word-indexed by slot)."""
        return self.w16(self.sym(table) + slot * 2)


def s16(v):
    """Interpret a 16-bit value as signed."""
    return v - 0x10000 if v & 0x8000 else v


# ---------------------------------------------------------------- the player


class Player:
    """Plays the sdl2_input_script.c command language on a :class:`Game`.

    Parameters
    ----------
    game : Game
        The game to drive.
    workdir : Path
        Directory for shots/ and snaps/ (the session dir).
    """

    def __init__(self, game, workdir):
        self.game = game
        self.core = game.core
        self.dir = Path(workdir)
        self.frame = 0
        self.shot_counter = 0
        self.record_video = None
        self.record_audio = None
        self.recording = False
        self.recorded = 0
        self.quit = False

    # --- frames

    def step(self, pad=0):
        """Run one frame with ``pad`` (a libretro joypad bitmask) held."""
        self.core.run(pad)
        self.frame += 1
        self.game.frame_changed()
        if self.recording:
            if self.record_video is not None:
                self.record_video.write(self.core.rgb().tobytes())
            if self.record_audio is not None:
                self.record_audio.write(self.core.take_audio())
            self.recorded += 1

    def parse_buttons(self, s):
        """Parse ``A+UP`` style buttons into a libretro bitmask."""
        if s in ("-", ""):
            return 0
        pad = 0
        for tok in s.split("+"):
            tok = tok.upper()
            if tok not in BUTTON_IDS:
                self.err(f"unknown button '{tok}'")
                continue
            pad |= 1 << BUTTON_IDS[tok]
        return pad

    def err(self, msg):
        """Print an ERR line."""
        log(f"ERR {msg}")

    # --- until

    def until(self, args):
        """Run an ``until COND [MAX] [PATTERN]`` command."""
        if not args:
            self.err("bad until")
            return
        cond, rest = args[0], args[1:]
        limit, pattern = 3600, "-"
        if rest and rest[0].isdigit():
            limit = int(rest[0])
            if len(rest) > 1:
                pattern = rest[1]
        elif rest:
            pattern = rest[0]
        g = self.game
        stable = 0

        def settled(ok):
            nonlocal stable
            stable = stable + 1 if ok else 0
            return stable >= 3

        if cond.startswith(("in:", "out:")):
            mode = cond.split(":", 1)[1].upper()
            want = cond.startswith("in:")

            def met():
                return settled(g.on_chain(mode) == want)

        elif cond == "idle":

            def met():
                return settled(g.not_idle_reason() is None)

        elif cond == "input":

            def met():
                return settled(g.waiting_for_input())

        elif cond == "battle":

            def met():
                return bool(
                    g.w16(g.sym("BATTLE_MODE"))
                    or g.w16(g.sym("BATTLE_SWIRL_COUNTDOWN"))
                    or g.w8(g.sym("ENEMY_HAS_BEEN_TOUCHED"))
                    or g.in_battle()
                )

        elif cond == "text":
            start = self.text_signature()

            def met():
                sig = self.text_signature()
                return sig != start and bool(sig)

        elif cond.startswith("said:"):
            self.err(f"{cond}: window text is not decoded in emu mode (use until text / until input)")
            return
        else:
            self.err(f"unknown until condition '{cond}'")
            return

        segs = pattern.split("|")
        elapsed = 0
        while True:
            ok = met()
            if ok or elapsed >= limit:
                log(f"UNTIL {'met' if ok else 'TIMEOUT'} after {elapsed} frames (frame={self.frame})")
                return
            if pattern == "advance":
                pad = (1 << BUTTON_IDS["A"]) if (g.text_prompt_waiting() and not elapsed & 1) else 0
            else:
                seg = segs[(elapsed // 48) % len(segs)]
                tap = seg.endswith("*")
                pad = self.parse_buttons(seg.rstrip("*"))
                if (tap and elapsed & 1) or stable > 0:
                    pad = 0
            self.step(pad)
            elapsed += 1

    def text_signature(self):
        """Open windows with their text cursors: changes when text is printed."""
        g = self.game
        ws = g.sym("WINDOW_STATS")
        t = g.sym("OPEN_WINDOW_TABLE")
        sig = []
        for i in g.windows_open():
            w = ws + g.w16(t + i * 2) * WINDOW_STATS_SIZE
            sig.append((i, g.w16(w + WS_TEXT_X), g.w16(w + WS_TEXT_Y)))
        return tuple(sig)

    # --- output

    def print_state(self, label):
        """Print STATE / PARTY / FOLLOW / NEAR / ENEMIES lines."""
        g = self.game
        gs = g.gs
        why = g.not_idle_reason()
        chain = ">".join(dict.fromkeys(g.chain_names())) or "?"
        mode = g.wait_mode()
        d = g.w16(gs + GS_LEADER_DIR)
        line = (
            f"STATE {label or '-'} frame={self.frame} modes=[{chain}] idle={why or 'yes'} "
            f"input={int(why is None or mode is not None)}"
        )
        if mode:
            line += f" wait={mode}"
        line += (
            f" pos=({g.w16(gs + GS_LEADER_X)},{g.w16(gs + GS_LEADER_Y)})"
            f" dir={DIR_NAMES[d] if d < 8 else '?'} music={g.w16(g.sym('CURRENT_MAP_MUSIC_TRACK'))}"
            f" bgm={g.w16(g.sym('CURRENT_MUSIC_TRACK'))} money={g.w32(gs + GS_MONEY)}"
            f" battle={g.w16(g.sym('BATTLE_MODE'))} ow_enemies={g.w16(g.sym('OVERWORLD_ENEMY_COUNT'))}"
        )
        blink = g.w16(g.sym("PLAYER_INTANGIBILITY_FRAMES"))
        if blink:
            line += f" blink={blink}"
        line += surface_text(g.w16(gs + GS_TRODDEN_TILE))
        ws = g.w16(gs + GS_WALKING_STYLE)
        if ws:
            line += f" walk={ws}"
        ps = g.w8(gs + GS_PARTY_STATUS)
        if ps:
            line += f" party_status={ps}"
        if g.w16(g.sym("DEBUG")):
            line += " debug=1"
        log(line)

        parts = []
        for cid in g.party():
            c = g.char(cid)
            name = eb_text(bytes(g.w8(c + i) for i in range(5)))
            s = (
                f"{cid}:{name} L{g.w8(c + CH_LEVEL)} HP {g.w16(c + CH_HP)}/{g.w16(c + CH_MAX_HP)} "
                f"PP {g.w16(c + CH_PP)}/{g.w16(c + CH_MAX_PP)}"
            )
            st = []
            for grp in range(7):
                v = g.w8(c + CH_AFFLICTIONS + grp)
                if v:
                    names = STATUS_NAMES[grp]
                    st.append(f"{grp + 1}:{names[v] if v < len(names) and names[v] else v}")
            if st:
                s += " st=" + ",".join(st)
            parts.append(s)
        log("PARTY " + " | ".join(parts))

        if g.in_battle():
            bt = g.sym("BATTLERS_TABLE")
            out = []
            for i in range(FIRST_ENEMY_INDEX, BATTLER_COUNT):
                b = bt + i * BATTLER_SIZE
                if not g.w8(b + BT_CONSCIOUSNESS):
                    continue
                out.append(
                    f"[{i - FIRST_ENEMY_INDEX}] {g.enemy_name(g.w16(b + BT_ID))} "
                    f"hp={g.w16(b + BT_HP)}/{g.w16(b + BT_HP_MAX)}"
                )
            log("ENEMIES " + " ".join(out))
            return

        lead = g.leader_entity()
        lx, ly = g.entity16("ENTITY_ABS_X_TABLE", lead), g.entity16("ENTITY_ABS_Y_TABLE", lead)
        if len(g.party()) > 1:
            follow = []
            order = [g.w8(gs + GS_PARTY_ORDER + i) for i in range(6)]
            for i in range(min(g.w8(gs + GS_PARTY_COUNT), 6)):
                e = g.w16(gs + GS_PARTY_ENTITY_SLOTS + i * 2)
                if e == lead or e >= MAX_ENTITIES or g.entity16("ENTITY_SCRIPT_TABLE", e) == 0xFFFF:
                    continue
                dx = s16(g.entity16("ENTITY_ABS_X_TABLE", e)) - s16(lx)
                dy = s16(g.entity16("ENTITY_ABS_Y_TABLE", e)) - s16(ly)
                hidden = "hidden" if g.entity16("ENTITY_ANIMATION_FRAME", e) == 0xFFFF else ""
                follow.append(f"{order[i]}:({dx:+d},{dy:+d}){hidden}")
            if follow:
                log("FOLLOW " + " ".join(follow))
        near = []
        for e in range(PARTY_LEADER_ENTITY_INDEX):
            if g.entity16("ENTITY_SCRIPT_TABLE", e) == 0xFFFF:
                continue
            npc = g.entity16("ENTITY_NPC_IDS", e)
            if npc == 0xFFFF:
                continue
            dx = s16(g.entity16("ENTITY_ABS_X_TABLE", e)) - g.w16(gs + GS_LEADER_X)
            dy = s16(g.entity16("ENTITY_ABS_Y_TABLE", e)) - g.w16(gs + GS_LEADER_Y)
            enemy = bool(npc & 0x8000)
            if not enemy and not (-160 <= dx <= 160 and -128 <= dy <= 128):
                continue
            if enemy:
                eid = s16(g.entity16("ENTITY_ENEMY_IDS", e))
                near.append(f"enemy {g.enemy_name(eid) if eid >= 0 else '?'} ({dx:+d},{dy:+d})")
            else:
                near.append(f"npc{npc} ({dx:+d},{dy:+d})")
        if near:
            log("NEAR " + " | ".join(near))

    def print_windows(self):
        """Print WIN lines (open windows: id, title, box) and the focused MENU."""
        g = self.game
        ws = g.sym("WINDOW_STATS")
        t = g.sym("OPEN_WINDOW_TABLE")
        focus = g.w16(g.sym("CURRENT_FOCUS_WINDOW"))
        for i in g.windows_open():
            w = ws + g.w16(t + i * 2) * WINDOW_STATS_SIZE
            title = eb_text(bytes(g.w8(w + WS_TITLE + k) for k in range(22)))
            box = f"{g.w16(w + WS_X)},{g.w16(w + WS_Y)} {g.w16(w + WS_W)}x{g.w16(w + WS_H)}"
            log(f"WIN w{i}{'*' if i == focus else ''}{f' [{title}]' if title else ''}| (box {box})")
        if focus == 0xFFFF or focus >= WINDOW_MAX or g.w16(t + focus * 2) == 0xFFFF:
            return
        w = ws + g.w16(t + focus * 2) * WINDOW_STATS_SIZE
        first = g.w16(w + WS_CURRENT_OPTION)
        # SELECTION_MENU keeps the highlighted option in a local, but leaves the
        # window's text cursor just after the cursor glyph, at (option x + 1, option y)
        cur = (g.w16(w + WS_TEXT_X), g.w16(w + WS_TEXT_Y))
        if not g.w16(w + WS_OPTION_COUNT) or first == 0xFFFF:
            return
        mo = g.sym("MENU_OPTIONS")
        items, idx = [], first
        for _ in range(64):  # follow the option list's next links
            if idx == 0xFFFF or idx >= 0x100:
                break
            a = mo + idx * MENU_OPTION_SIZE
            label = eb_text(bytes(g.w8(a + MO_LABEL + k) for k in range(25)))
            here = (g.w16(a + MO_TEXT_X) + 1, g.w16(a + MO_TEXT_Y)) == cur
            items.append(f">[{label}]" if here else f"[{label}]")
            idx = g.w16(a + MO_NEXT)
        log(f"MENU w{focus}| " + " ".join(items))

    def shot(self, args):
        """``shot [--crop W H] [--zoom N] [LABEL|PATH.png]``."""
        crop, zoom, label = None, 1, ""
        i = 0
        while i < len(args):
            if args[i] == "--crop" and i + 2 < len(args) + 1:
                try:
                    crop = (int(args[i + 1]), int(args[i + 2]))
                except (IndexError, ValueError):
                    self.err("usage: shot --crop W H [--zoom N] [LABEL]")
                    return
                i += 3
            elif args[i] == "--zoom":
                try:
                    zoom = int(args[i + 1])
                except (IndexError, ValueError):
                    zoom = 0
                if not 1 <= zoom <= 16:
                    self.err("usage: shot ... --zoom N (1-16)")
                    return
                i += 2
            else:
                label = args[i]
                i += 1
        if label.endswith(".png"):
            path = label
        else:
            (self.dir / "shots").mkdir(exist_ok=True)
            path = f"shots/{self.shot_counter:04d}{'-' if label else ''}{label}.png"
        self.shot_counter += 1
        img = self.core.rgb()
        x0, y0, w, h = 0, 0, WIDTH, HEIGHT
        if crop:
            g = self.game
            lead = g.leader_entity()
            cx = s16(g.entity16("ENTITY_SCREEN_X_TABLE", lead))
            cy = s16(g.entity16("ENTITY_SCREEN_Y_TABLE", lead)) - 8
            w, h = min(crop[0], WIDTH), min(crop[1], HEIGHT)
            x0 = max(0, min(cx - w // 2, WIDTH - w))
            y0 = max(0, min(cy - h // 2, HEIGHT - h))
            img = img[y0 : y0 + h, x0 : x0 + w]
        im = Image.fromarray(img)
        if zoom > 1:
            im = im.resize((w * zoom, h * zoom), Image.NEAREST)
        im.save(self.dir / path)
        extra = f" box={x0},{y0},{w}x{h} zoom={zoom}" if crop or zoom > 1 else ""
        log(f"SHOT {path} frame={self.frame}{extra}")

    # --- snapshots

    def snap(self, name):
        """Save a core savestate to snaps/NAME.state."""
        (self.dir / "snaps").mkdir(exist_ok=True)
        p = self.dir / "snaps" / f"{name}.state"
        p.write_bytes(self.core.serialize())
        p.with_suffix(".frame").write_text(str(self.frame))
        log(f"SNAP {name} ok")

    def restore(self, name):
        """Load snaps/NAME.state."""
        p = self.dir / "snaps" / f"{name}.state"
        if not p.exists():
            log(f"RESTORE {name} FAILED")
            return
        self.core.unserialize(p.read_bytes())
        with contextlib.suppress(OSError, ValueError):
            self.frame = int(p.with_suffix(".frame").read_text())
        self.game.frame_changed()
        log(f"RESTORE {name} ok")

    # --- teleport

    def teleport(self, x, y, d):
        """Move the party to world pixel (x, y) by running INITIALIZE_MAP in the game."""
        g = self.game
        why = g.not_idle_reason()
        if why:
            self.err(f"teleport needs the player idle on the overworld (not idle: {why})")
            return
        # Divert only while the CPU spins in WAIT_UNTIL_NEXT_FRAME (not in the NMI)
        wait = g.sym("WAIT_UNTIL_NEXT_FRAME")
        wait_end = g.function_end(wait)
        for _ in range(60):
            state = self.core.serialize()
            regs = read_regs(state)
            pc = (regs["PB"] << 16) | regs["PC"]
            if wait <= pc < wait_end:
                break
            self.step(0)
        else:
            self.err("teleport: the CPU never reached the frame wait")
            return
        done = STUB_ADDR + STUB_LIMIT - 1
        code = teleport_stub(g, x, y, d, pc, done)
        for i, b in enumerate(code):
            g.put8(STUB_ADDR + i, b)
        g.put8(done, 0)
        state = self.core.serialize()  # now includes the stub
        self.core.unserialize(write_regs(state, {"PB": STUB_ADDR >> 16, "PC": STUB_ADDR & 0xFFFF}))
        g.frame_changed()
        for _ in range(600):
            self.step(0)
            if g.w8(done):
                break
        else:
            self.err("teleport: the stub did not finish in 600 frames")
            return
        for i in range(STUB_LIMIT):
            g.put8(STUB_ADDR + i, 0)
        log(f"TELEPORT ({x},{y}) dir={DIR_NAMES[d & 7]}")

    def warp(self, arg):
        """``warp NAME|N`` / ``warps``: PSI Teleport destinations from ROM."""
        g = self.game
        base = g.sym("PSI_TELEPORT_DEST_TABLE") & 0x3FFFFF
        dests = []
        i = 1
        while True:
            e = base + i * PSI_TELEPORT_ENTRY_SIZE
            if not self.core.rom[e]:
                break
            name = eb_text(self.core.rom[e : e + PSI_TELEPORT_NAME_LEN])
            x = int.from_bytes(self.core.rom[e + 27 : e + 29], "little") * 8
            y = int.from_bytes(self.core.rom[e + 29 : e + 31], "little") * 8
            dests.append((i, name, x, y))
            i += 1
        if arg is None:
            for i, name, x, y in dests:
                log(f"WARP {i} {name} ({x},{y})")
            return
        for i, name, x, y in dests:
            if arg == str(i) or name.lower().startswith(arg.lower()):
                log(f"WARP {i} {name} ({x},{y})")
                self.teleport(x, y, 4)
                return
        self.err(f"unknown warp '{arg}' (try 'warps')")

    # --- instant edits

    def give(self, item, who):
        """Put ``item`` in the first empty slot of ``who`` (or the first member with room)."""
        g = self.game
        cands = [who] if who else g.party()
        for cid in cands:
            if not 1 <= cid <= 6:
                continue
            c = g.char(cid)
            for k in range(ITEM_SLOTS):
                if g.w8(c + CH_ITEMS + k) == 0:
                    g.put8(c + CH_ITEMS + k, item)
                    name = eb_text(bytes(g.w8(c + i) for i in range(5)))
                    log(f"GIVE item {item} -> char {cid} {name}")
                    return
        log(f"GIVE item {item} FAILED: {'that' if who else 'every party'} inventory is full")

    def set_hp_pp(self, kind, cid, v):
        """``hp CHAR N`` / ``pp CHAR N`` (char and its battler)."""
        g = self.game
        c = g.char(cid)
        cur, tgt = (CH_HP, CH_HP_TARGET) if kind == "hp" else (CH_PP, CH_PP_TARGET)
        g.put16(c + cur, v)
        g.put16(c + tgt, v)
        b = g.party_battler(cid)
        if b is not None:
            bc, bt = (BT_HP, BT_HP_TARGET) if kind == "hp" else (BT_PP, BT_PP_TARGET)
            g.put16(b + bc, v)
            g.put16(b + bt, v)

    # --- the command loop

    def run_line(self, line):
        """Run one script line. Frame-taking commands run to completion."""
        if line.startswith("note "):
            log("> " + line[5:].rstrip("\n"))
            return
        if line.startswith("mark "):
            log(f"MARK n={self.recorded} frame={self.frame} {line[5:].rstrip()}")
            return
        line = line.split("#", 1)[0].strip()
        if not line:
            return
        parts = line.split()
        cmd, args = parts[0], parts[1:]
        g = self.game
        ints = []
        for a in args:
            try:
                ints.append(int(a))
            except ValueError:
                break
        if cmd[0].isdigit():
            pad = self.parse_buttons(args[0] if args else "-")
            for _ in range(int(cmd)):
                self.step(pad)
        elif cmd == "wait":
            for _ in range(ints[0] if ints else 0):
                self.step(0)
        elif cmd == "press":
            pad = self.parse_buttons(args[0] if args else "-")
            count = int(args[1]) if len(args) > 1 and args[1].isdigit() else 1
            for _ in range(max(count, 1)):
                for phase in range(6):
                    self.step(pad if phase < 2 else 0)
        elif cmd == "until":
            self.until(args)
        elif cmd == "shot":
            self.shot(args)
        elif cmd == "state":
            self.print_state(args[0] if args else "")
        elif cmd == "text":
            self.print_windows()
        elif cmd == "sync":
            log(f"SYNC {args[0] if args else ''} frame={self.frame}")
        elif cmd == "flag":
            if not ints or not 1 <= ints[0] < EVENT_FLAG_COUNT:
                self.err("usage: flag N [0|1]")
                return
            if len(ints) > 1:
                g.set_flag(ints[0], ints[1])
            log(f"FLAG {ints[0]} = {g.flag(ints[0])}")
        elif cmd == "give":
            if not ints:
                self.err("usage: give ITEM [CHAR]")
                return
            self.give(ints[0], ints[1] if len(ints) > 1 else 0)
        elif cmd == "hpt":
            # HP/PP rolling targets (where the meters will settle), exp, bank balance
            log(
                "HPT"
                + "".join(
                    f" {cid}:{g.w16(g.char(cid) + CH_HP_TARGET)}/{g.w16(g.char(cid) + CH_PP_TARGET)}"
                    for cid in g.party()
                )
                + " exp"
                + "".join(f" {g.w32(g.char(cid) + CH_EXP)}" for cid in g.party())
                + f" bank {g.w32(g.gs + GS_BANK_BALANCE)}"
            )
        elif cmd == "rng":
            # RAND_A/RAND_B aren't in SRAM: seed both sides before comparing RNG-dependent steps
            if len(ints) >= 2:
                g.put16(g.sym("RAND_A"), ints[0])
                g.put16(g.sym("RAND_B"), ints[1])
            log(f"RNG {g.w16(g.sym('RAND_A'))} {g.w16(g.sym('RAND_B'))}")
        elif cmd == "money":
            if ints:
                g.put32(g.gs + GS_MONEY, ints[0])
            log(f"MONEY {g.w32(g.gs + GS_MONEY)}")
        elif cmd in ("hp", "pp"):
            if len(ints) < 2 or not 1 <= ints[0] <= 6:
                self.err(f"usage: {cmd} CHAR N")
                return
            self.set_hp_pp(cmd, ints[0], ints[1])
        elif cmd == "heal":
            for cid in g.party():
                c = g.char(cid)
                self.set_hp_pp("hp", cid, g.w16(c + CH_MAX_HP))
                self.set_hp_pp("pp", cid, g.w16(c + CH_MAX_PP))
        elif cmd == "status":
            if len(ints) < 3:
                self.err("usage: status CHAR GROUP VALUE")
                return
            cid, grp, v = ints[:3]
            if grp == 8:
                g.put8(g.gs + GS_PARTY_STATUS, v)
                log(f"STATUS party_status={v}")
            elif 1 <= cid <= 6 and 1 <= grp <= 7:
                g.put8(g.char(cid) + CH_AFFLICTIONS + grp - 1, v)
                log(f"STATUS char {cid} group {grp} = {v}")
            else:
                self.err("usage: status CHAR GROUP(1-8) VALUE")
        elif cmd == "debug":
            g.put16(g.sym("DEBUG"), 1 if args and args[0] == "on" else 0)
            log(f"DEBUG {'on' if g.w16(g.sym('DEBUG')) else 'off'}")
        elif cmd == "teleport":
            if len(ints) < 2:
                self.err("usage: teleport X Y [DIR]")
                return
            self.teleport(ints[0], ints[1], ints[2] & 7 if len(ints) > 2 else 4)
        elif cmd == "warp":
            if not args:
                self.err("usage: warp NAME|N")
                return
            self.warp(args[0])
        elif cmd == "warps":
            self.warp(None)
        elif cmd == "snap":
            self.snap(args[0] if args else "snap")
        elif cmd == "restore":
            self.restore(args[0] if args else "snap")
        elif cmd == "record":
            self.recording = bool(args) and args[0] == "on"
            self.core.take_audio()
        elif cmd in ("end", "quit"):
            self.quit = True
        elif cmd == "aux":
            self.err("aux keys are C-port debug keys (not available in emu mode)")
        else:
            self.err(f"unknown command '{cmd}'")


def teleport_stub(g, x, y, d, ret, done):
    """65816 code that teleports as the PSI Teleport arrival, then resumes at ``ret``.

    Mirrors ``input_script_root_boundary`` in sdl2_input_script.c: force a map
    reload, ``INITIALIZE_MAP``, then flush the entity creation queue, enable
    every entity and respawn Buzz Buzz / deliveries. Sets the byte at ``done``.
    """

    def long(a):
        return [a & 0xFF, (a >> 8) & 0xFF, a >> 16]

    def imm16(v):
        return [v & 0xFF, (v >> 8) & 0xFF]

    def jsl(name):
        return [0x22, *long(g.sym(name))]

    def sta(name):
        return [0x8F, *long(g.sym(name))]

    code = [0x08, 0x8B, 0x0B, 0xC2, 0x30]  # PHP; PHB; PHD; REP #$30
    code += [0xA9, *imm16(0xFFFF)]
    code += sta("CURRENT_MAP_MUSIC_TRACK") + sta("LOADED_MAP_PALETTE") + sta("LOADED_MAP_TILE_COMBO")
    code += [0xA0, *imm16(d), 0xA2, *imm16(y), 0xA9, *imm16(x)]  # LDY dir; LDX y; LDA x
    code += jsl("INITIALIZE_MAP")
    code += [0xC2, 0x30, 0xA9, *imm16(0)] + sta("PLAYER_HAS_MOVED_SINCE_MAP_LOAD")
    code += jsl("FLUSH_ENTITY_CREATION_QUEUE") + jsl("ENABLE_ALL_ENTITIES")
    code += [0xC2, 0x30, 0xA9, *imm16(0xFFFF)] + sta("STAIRS_DIRECTION")
    code += jsl("SPAWN_BUZZ_BUZZ")
    code += [0xE2, 0x20, 0xA9, 0x01, 0x8F, *long(done)]  # SEP #$20; LDA #1; STA done
    code += [0x2B, 0xAB, 0x28, 0x5C, *long(ret)]  # PLD; PLB; PLP; JML ret
    if len(code) >= STUB_LIMIT:
        raise RuntimeError("teleport stub too long")
    return code


def surface_text(sf):
    """`` surface=0xNN(names)`` as in sdl2_input_script.c print_surface."""
    out = f" surface=0x{sf & 0xFF:02X}"
    if not sf & 0xFF:
        return out
    names = []
    if sf & 0x0C == 0x0C:
        names.append("deep-water")
    elif sf & 0x08:
        names.append("shallow-water")
    elif sf & 0x04:
        names.append("sunstroke")
    for bit, name in ((0x10, "ladder-stairs"), (0x01, "hide-lower"), (0x02, "hide-upper"), (0x80, "solid")):
        if sf & bit:
            names.append(name)
    return f"{out}({','.join(names)})"


def session_lines(path):
    """Yield command lines from a FIFO forever (held open read-write: no EOF)."""
    fd = os.open(path, os.O_RDWR)
    buf = b""
    while True:
        while b"\n" not in buf:
            chunk = os.read(fd, 4096)
            if not chunk:
                return
            buf += chunk
        line, buf = buf.split(b"\n", 1)
        yield line.decode(errors="replace")


def main():
    """Command-line entry point (mirrors the port binary's harness flags)."""
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--headless", action="store_true", help="accepted for compatibility (always headless)")
    ap.add_argument("--save", help=".srm (8 KB) loaded into SRAM before boot")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--session", help="read commands from this FIFO forever")
    src.add_argument("--input", help="play this script, then quit")
    ap.add_argument("--record-video", help="write raw rgb24 frames (256x224) here")
    ap.add_argument("--record-audio", help="write raw s16le stereo (32040 Hz) here")
    ap.add_argument("--record-paused", action="store_true", help="start recording at 'record on'")
    a = ap.parse_args()

    t0 = time.time()
    core = Core(rom_path())
    if a.save:
        data = Path(a.save).read_bytes()
        n = min(len(data), len(core.sram))
        C.memmove(core.sram, data, n)
    game = Game(core, load_symbols())
    player = Player(game, Path.cwd())
    log(f"EMU core={core_path().name} rom={rom_path().name} ready in {time.time() - t0:.2f}s")

    if a.record_video or a.record_audio:
        log(f"RECORD video {WIDTH}x{HEIGHT} fps=60")
        if a.record_video:
            player.record_video = Path(a.record_video).open("wb")  # noqa: SIM115
        if a.record_audio:
            player.record_audio = Path(a.record_audio).open("wb")  # noqa: SIM115
            core.record_audio = True
        player.recording = not a.record_paused

    lines = session_lines(a.session) if a.session else Path(a.input).read_text().splitlines()
    for line in lines:
        player.run_line(line)
        if player.quit:
            break
        if a.session and a.save:
            # Keep the live SRAM next to the save (in-game saves land there)
            Path(a.save).write_bytes(bytes(core.sram))
    if player.record_video or player.record_audio:
        for f in (player.record_video, player.record_audio):
            if f:
                f.close()
        log(f"RECORD done frames={player.recorded}")


if __name__ == "__main__":
    main()
