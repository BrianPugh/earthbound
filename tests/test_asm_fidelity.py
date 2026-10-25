"""Mechanical asm<->C fidelity checks for the C port.

These read only the disassembly (``asm/``, ``include/``) and the C sources, so
they need neither the linker map nor extracted assets. Each test targets a bug
class where a ported value was retyped by hand and drifted from the original:

- ``test_constants_match_asm``: a C ``#define``/enum member that shares its name
  with an asm ``.DEFINE`` or enum member (``SCOPE::NAME`` -> ``SCOPE_NAME``) must
  have the same value.
- ``test_annotated_literals_match_asm``: a C literal annotated with an asm
  constant (``play_sfx(94); /* SFX::NAMING_CONFIRM */``) must equal it.
- ``test_call_arguments_match_asm``: literal arguments of ``fade_in``/``fade_out``,
  ``change_music`` and ``play_sfx`` must match an ``LDA``/``LDX``/``TXA`` setup
  of the same routine in an asm file the enclosing C function refers to.

Run ``python tests/test_asm_fidelity.py`` for the full report, including C calls
the third test could not tie to any asm routine.
"""

import contextlib
import re
from functools import cache
from pathlib import Path

ROOT = Path(__file__).parent.parent
ASM_CONSTANT_FILES = ["include/enums.asm", "include/hardware.asm", "include/config.asm", "include/constants/*.asm"]
DEFINES = {"USA"}  # the US retail build the port follows

# JP/prototype-only sources describe other builds.
VERSION_SPECIFIC = re.compile(r"-(jp|proto)\.asm$")

# Name shared by an asm and a C constant on purpose with a different value.
CONSTANT_EXCLUSIONS = {
    # The port's battlers_table holds 8 party + 12 enemy slots (asm: 32). The
    # largest enemy group in enemy_battle_groups_table has 11 enemies, and
    # battle_random_targeting wraps at BATTLER_COUNT where the asm wraps at a
    # literal 32, which picks the same bit while bits 20-31 are never set.
    "BATTLER_COUNT",
}

# asm routine -> (C function, argument registers in C parameter order).
CALL_PAIRS = {
    "FADE_IN": ("fade_in", ("A", "X")),
    "FADE_OUT": ("fade_out", ("A", "X")),
    "FADE_IN_WITH_MOSAIC": ("fade_in_with_mosaic", ("A", "X")),
    "FADE_OUT_WITH_MOSAIC": ("fade_out_with_mosaic", ("A", "X", "Y")),
    "CHANGE_MUSIC": ("change_music", ("A",)),
    "PLAY_SOUND": ("play_sfx", ("A",)),
}
# A C call is checked against every routine of its family: a C fade_out() at a
# site whose asm uses FADE_OUT_WITH_MOSAIC is a timing divergence (the NMI fade
# takes delay+1 frames per step, the mosaic loop takes delay).
FAMILIES = {
    "fade_in": "fade",
    "fade_out": "fade",
    "fade_in_with_mosaic": "fade",
    "fade_out_with_mosaic": "fade",
    "change_music": "music",
    "play_sfx": "sfx",
}

# (C file, C function, call) checked by hand: why the literal call is right.
CALL_EXCLUSIONS = {
    # STZ FADE_PARAMETERS::step (cancel any fade) in FORCE_BLANK_AND_WAIT_VBLANK.
    ("src/game/battle_ui.c", "force_blank_and_wait_vblank", "fade_out(0, 0)"),
    ("src/game/battle_ui.c", "force_blank_and_wait_vblank_work_step", "fade_out(0, 0)"),
    # FILE_SELECT_MENU's first-time @WAIT_FADE: LDA #MUSIC::SETUP_SCREEN; JSL
    # CHANGE_MUSIC (asm/intro/file_select_menu.asm), hoisted into the caller.
    ("src/intro/init_intro.c", "mode_step_init_intro", "change_music(3)"),
}

# Confirmed divergences whose fix is pending; drop the entry with the fix.
KNOWN_DIVERGENCES = {
    # INITIALIZE_GAME_OVER_SCREEN and SPAWN's Continue path use FADE_OUT_WITH_MOSAIC
    # (Y=0): a blocking loop with WAIT_FRAMES(delay) per step and no OAM clear. The
    # port runs the NMI fade (delay+1 frames per step) under FADE_TICK_SCREEN_ONLY,
    # which also clears OAM each frame. Fix: push GAME_MODE_MOSAIC_FADE instead.
    ("src/game/overworld_palette.c", "game_over_screen_begin", "fade_out(1, 1)"),
    ("src/game/overworld_palette.c", "mode_step_game_over", "fade_out(2, 1)"),
}


# ---------------------------------------------------------------- asm parsing


def _asm_lines(path: Path) -> list[str]:
    return [line.split(";")[0].rstrip() for line in path.read_text(errors="replace").splitlines()]


def _asm_eval(expr: str, env: dict[str, int]) -> int:
    expr = re.sub(r"\$([0-9A-Fa-f]+)", lambda m: str(int(m.group(1), 16)), expr.strip())
    expr = re.sub(r"%([01]+)", lambda m: str(int(m.group(1), 2)), expr)
    expr = re.sub(r"[A-Za-z_][\w:]*", lambda m: str(env[m.group(0).replace("::", "_")]), expr)
    if not re.fullmatch(r"[\d\s+\-*/()<>&|^~]+", expr):
        raise ValueError(expr)
    return int(eval(expr.replace("/", "//")))  # noqa: S307 - digits and operators only


def _asm_condition(cond: str) -> bool:
    cond = re.sub(r"\.DEFINED\((\w+)\)", lambda m: str(m.group(1) in DEFINES), cond)
    cond = cond.replace("||", " or ").replace("&&", " and ").replace("!", " not ")
    if not re.fullmatch(r"[\sA-Za-z()]*", cond):
        raise ValueError(cond)
    return bool(eval(cond))  # noqa: S307 - True/False/and/or/not only


@cache
def asm_constants() -> dict[str, int]:
    """``.DEFINE`` names and enum members (``SCOPE_MEMBER``) of the US build."""
    env: dict[str, int] = {}
    paths = [p for pattern in ASM_CONSTANT_FILES for p in sorted(ROOT.glob(pattern))]
    for path in paths:
        active: list[bool] = []
        scope = None
        counter = 0
        for line in _asm_lines(path):
            line = line.strip()
            if not line:
                continue
            if m := re.match(r"\.IF(N?)DEF\s+(\w+)", line, re.I):
                active.append((m.group(2) in DEFINES) != bool(m.group(1)))
            elif m := re.match(r"\.IF\s+(.*)", line, re.I):
                active.append(_asm_condition(m.group(1)))
            elif re.match(r"\.ELSE\b", line, re.I):
                active[-1] = not active[-1]
            elif re.match(r"\.ENDIF\b", line, re.I):
                active.pop()
            elif not all(active):
                continue
            elif m := re.match(r"\.ENUM\s*(\w*)", line, re.I):
                scope, counter = m.group(1), 0
            elif re.match(r"\.ENDENUM", line, re.I):
                scope = None
            elif m := re.match(r"\.DEFINE\s+(\w+)\s+(.+)", line, re.I):
                with contextlib.suppress(KeyError, ValueError, SyntaxError):
                    env[m.group(1)] = _asm_eval(m.group(2), env)
            elif scope is not None and (m := re.match(r"(\w+)\s*(?:=\s*(.+))?$", line)):
                prefix = f"{scope}_" if scope else ""
                if m.group(2) is not None:
                    # Members may refer to earlier members of the same enum unqualified.
                    local = env | {k[len(prefix) :]: v for k, v in env.items() if prefix and k.startswith(prefix)}
                    try:
                        counter = _asm_eval(m.group(2), local)
                    except (KeyError, ValueError, SyntaxError):
                        counter = None
                if counter is not None:
                    env[prefix + m.group(1)] = counter
                    counter += 1
    return env


@cache
def asm_label_files() -> dict[str, Path]:
    labels = {}
    for path in (ROOT / "asm").rglob("*.asm"):
        if VERSION_SPECIFIC.search(path.name):
            continue
        for m in re.finditer(r"^(\w+):", path.read_text(errors="replace"), re.M):
            labels.setdefault(m.group(1), path)
    return labels


@cache
def _asm_files_by_name() -> dict[str, list[Path]]:
    files: dict[str, list[Path]] = {}
    for path in (ROOT / "asm").rglob("*.asm"):
        if not VERSION_SPECIFIC.search(path.name):
            files.setdefault(path.name, []).append(path)
    return files


def asm_files_named(name: str) -> list[Path]:
    return _asm_files_by_name().get(name, [])


def _asm_immediate(operand: str) -> int | None:
    try:
        return _asm_eval(operand.removeprefix("#").replace(".LOWORD", ""), asm_constants()) & 0xFFFF
    except (KeyError, ValueError, SyntaxError):
        return None


def _asm_call_args(lines: list[str], call_index: int) -> dict[str, int | None]:
    """Register values set up before the call at ``call_index`` (None = not an immediate).

    Walks back over ``LDA #``/``LDX #``/``LDY #``/``TXA`` and mode switches;
    any other instruction or a label ends the setup.
    """
    regs: dict[str, int | None] = {}
    a_from_x = False
    for line in reversed(lines[max(0, call_index - 8) : call_index]):
        text = line.strip()
        if not text or text.startswith(".") or re.match(r"(REP|SEP)\b", text):
            continue
        if m := re.match(r"LD([AXY])\s+(#.+)$", text):
            reg = m.group(1)
            if reg not in regs and not (reg == "A" and a_from_x):
                regs[reg] = _asm_immediate(m.group(2))
            continue
        if text == "TXA" and "A" not in regs:
            a_from_x = True
            continue
        break
    if a_from_x:
        regs["A"] = regs.get("X")
    return regs


@cache
def asm_calls() -> dict[Path, list[tuple[str, tuple]]]:
    """Map each asm file to [(C function name, argument tuple)] for every CALL_PAIRS call."""
    calls: dict[Path, list[tuple[str, tuple]]] = {}
    pattern = re.compile(rf"\s*JS[LR]\s+({'|'.join(CALL_PAIRS)})\s*$")
    for path in (ROOT / "asm").rglob("*.asm"):
        if VERSION_SPECIFIC.search(path.name):
            continue
        lines = _asm_lines(path)
        for i, line in enumerate(lines):
            if m := pattern.match(line):
                c_name, regs = CALL_PAIRS[m.group(1)]
                found = _asm_call_args(lines, i)
                calls.setdefault(path, []).append((c_name, tuple(found.get(r) for r in regs)))
    return calls


# ------------------------------------------------------------------ C parsing


def _strip_c_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _c_files() -> list[Path]:
    return [p for p in (ROOT / "src").rglob("*.[ch]") if not {"vendor", "tools"} & set(p.parts)]


@cache
def c_constants() -> dict[str, set[int]]:
    """Name -> values of integer-literal ``#define``s and enum members under src/."""
    consts: dict[str, set[int]] = {}
    for path in _c_files():
        text = _strip_c_comments(path.read_text())
        for m in re.finditer(r"^\s*#\s*define\s+(\w+)\s+\(?\s*(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\s*\)?\s*$", text, re.M):
            consts.setdefault(m.group(1), set()).add(int(m.group(2), 0))
        for body in re.findall(r"\benum\b[^{;]*\{([^}]*)\}", text):
            value: int | None = 0
            for item in body.split(","):
                if not (item := item.strip()):
                    continue
                m = re.fullmatch(r"(\w+)\s*(?:=\s*(.+))?", item, re.S)
                if not m:
                    value = None
                    continue
                if m.group(2) is not None:
                    try:
                        value = int(m.group(2).strip().rstrip("uUlL"), 0)
                    except ValueError:
                        value = None
                if value is not None:
                    consts.setdefault(m.group(1), set()).add(value)
                    value += 1
    return consts


def _c_value(arg: str) -> int | None:
    arg = arg.strip().removeprefix("(uint8_t)").removeprefix("(uint16_t)").strip()
    if re.fullmatch(r"0[xX][0-9A-Fa-f]+|\d+", arg):
        return int(arg, 0)
    values = c_constants().get(arg, set())
    return next(iter(values)) if len(values) == 1 else None


def _c_function_spans(lines: list[str]) -> list[tuple[int, int, int, str]]:
    """(comment start, body start, end, name) for each top-level function definition."""
    starts = []
    for i, line in enumerate(lines):
        m = re.match(r"[A-Za-z_][\w \*]*?\b(\w+)\s*\(", line)
        if m and not line.rstrip().endswith(";") and m.group(1) not in {"if", "while", "for", "switch", "return"}:
            starts.append((i, m.group(1)))
    spans = []
    for n, (start, name) in enumerate(starts):
        top = start
        while top > 0 and not lines[top - 1].startswith("}"):
            top -= 1
        end = starts[n + 1][0] if n + 1 < len(starts) else len(lines)
        spans.append((top, start, end, name))
    return spans


def _asm_references(region: str, func_name: str) -> set[Path]:
    """Return the asm files a C function names: labels (any case), its own name, asm/... paths."""
    labels = asm_label_files()
    refs = {labels[t] for t in re.findall(r"\b[A-Z][A-Z0-9_]{3,}\b", region) if t in labels}
    refs |= {labels[t.upper()] for t in re.findall(r"\b[a-z][a-z0-9_]{3,}\b", region) if t.upper() in labels}
    refs |= {ROOT / p for p in re.findall(r"asm/[\w/\-]+\.asm", region) if (ROOT / p).exists()}
    refs |= {f for name in re.findall(r"\b([\w\-]+\.asm)\b", region) for f in asm_files_named(name)}
    if func_name.upper() in labels:
        refs.add(labels[func_name.upper()])
    return refs


def check_c_calls() -> tuple[list[str], list[str]]:
    """(mismatches, unmatched) for every literal-argument CALL_PAIRS call in C."""
    c_names = {c for c, _ in CALL_PAIRS.values()}
    pattern = re.compile(rf"\b({'|'.join(c_names)})\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)")
    calls = asm_calls()
    mismatches, unmatched = [], []
    for path in sorted(p for p in _c_files() if p.suffix == ".c"):
        rel = str(path.relative_to(ROOT))
        raw = path.read_text().splitlines()
        code = _strip_c_comments("\n".join(raw)).split("\n")
        for top, start, end, func in _c_function_spans(raw):
            refs = None
            for lineno in range(start, end):
                for m in pattern.finditer(code[lineno]):
                    c_name, args = m.group(1), tuple(_c_value(a) for a in m.group(2).split(","))
                    if None in args or (rel, func, m.group(0)) in CALL_EXCLUSIONS | KNOWN_DIVERGENCES:
                        continue
                    if refs is None:
                        refs = _asm_references("\n".join(raw[top:end]), func)
                    family = FAMILIES[c_name]
                    asm = sorted({(n, a) for p in refs for n, a in calls.get(p, []) if FAMILIES[n] == family}, key=str)
                    where = f"{rel}:{lineno + 1} {func}: {m.group(0)}"
                    if not asm:
                        unmatched.append(where)
                    elif not any(
                        n == c_name and all(x is None or x == y for x, y in zip(a, args, strict=False)) for n, a in asm
                    ):
                        mismatches.append(f"{where}; asm has {asm}")
    return mismatches, unmatched


# ---------------------------------------------------------------------- tests


def test_constants_match_asm():
    asm, c = asm_constants(), c_constants()
    shared = (asm.keys() & c.keys()) - CONSTANT_EXCLUSIONS
    assert len(shared) > 300, f"only {len(shared)} shared constant names; parser broken?"
    drifted = {name: (asm[name], sorted(c[name])) for name in sorted(shared) if c[name] != {asm[name]}}
    assert not drifted, f"C constants that disagree with their asm twin (asm, C): {drifted}"


def test_annotated_literals_match_asm():
    """``<literal> /* NS::NAME */`` in C must equal the asm constant NS::NAME."""
    asm = asm_constants()
    annotated = re.compile(
        r"\b(0[xX][0-9A-Fa-f]+|\d+)[uU]?\s*[),;]*\s*/[*/]\s*(?:asm\s*)?([A-Z][A-Z0-9_]*)::([A-Z][A-Z0-9_]*)\s*(?:\*/|$)"
    )
    checked, wrong = 0, []
    for path in _c_files():
        for lineno, line in enumerate(path.read_text().splitlines(), 1):
            for m in annotated.finditer(line):
                name = f"{m.group(2)}_{m.group(3)}"
                # `1 << 11 /* FLAGS::X */` cites the mask, not the shift count.
                if name not in asm or line[: m.start()].rstrip().endswith("<<"):
                    continue
                checked += 1
                if int(m.group(1), 0) != asm[name]:
                    wrong.append(f"{path.relative_to(ROOT)}:{lineno}: {m.group(0).strip()} (asm {asm[name]})")
    assert checked > 20, f"only {checked} annotated literals found; pattern broken?"
    assert not wrong, "C literals that disagree with the asm constant they cite:\n" + "\n".join(wrong)


def test_call_arguments_match_asm():
    mismatches, _ = check_c_calls()
    assert not mismatches, "C call arguments that match no same-routine asm call:\n" + "\n".join(mismatches)


if __name__ == "__main__":
    bad, loose = check_c_calls()
    print("mismatched:", *bad, sep="\n  ")
    print("no asm routine found:", *loose, sep="\n  ")
