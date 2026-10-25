"""Savestate layout fingerprint (``src/core/state_layout.h``).

A savestate slot is only loadable by a build whose serialized structs have the
same field layout as the build that wrote it. Section sizes catch most layout
changes; this fingerprint catches the rest (a field reorder, a type swap of equal
width, a ``ModeState`` union member change, an enum renumbering).

The hash covers the source text (comments and whitespace stripped) of every type,
enum and object/function-like macro reachable from the globals listed in
``state_dump.c``'s ``k_sections`` table, plus every type defined in
``core/mode_stack.h`` (the per-mode phase enums are stored as plain ``uint8_t``).
An identifier named in a comment inside a reachable definition (the
``uint8_t phase; /* OverworldPhase */`` convention) also pulls in that type.
``state_dump.c`` mixes the result with its section id/size table at run time to
form the header's ``layout_id``.

Usage::

    uv run --no-sync python src/tools/savestate_layout.py          # rewrite the header
    uv run --no-sync python src/tools/savestate_layout.py --check  # exit 1 if stale
    uv run --no-sync python src/tools/savestate_layout.py --list   # show what is hashed
"""

import argparse
import re
import sys
import zlib
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent
HEADER = SRC / "core" / "state_layout.h"
STATE_DUMP = SRC / "core" / "state_dump.c"
MODE_STACK = SRC / "core" / "mode_stack.h"
SKIP_DIRS = {"vendor", "data", "tools"}

IDENT = re.compile(r"[A-Za-z_]\w*")
TAG_KEYWORDS = ("struct", "union", "enum")
PACK_MACROS = ("PACKED_STRUCT", "END_PACKED_STRUCT")


def _strip_comments(text: str) -> tuple[str, list[tuple[int, str]]]:
    """Blank out comments and string literals; return the code and (offset, comment) pairs."""
    out = []
    olen = 0
    comments = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            piece = " " + "\n" * text.count("\n", i, j)
            comments.append((olen, text[i:j]))
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            piece = " "
            comments.append((olen, text[i:j]))
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j += 1
            piece = c + c
        else:
            j = i + 1
            piece = c
        out.append(piece)
        olen += len(piece)
        i = j
    return "".join(out), comments


def _normalize(code: str) -> str:
    return " ".join(code.split())


class Index:
    """Type, enumerator and macro definitions across src/ (comment-free text)."""

    def __init__(self) -> None:
        self.types: dict[str, set[str]] = {}  # name -> definition texts
        self.refs: dict[str, set[str]] = {}  # definition text -> identifiers it uses
        self.file_types: dict[Path, set[str]] = {}
        self.decls: dict[str, list[str]] = {}  # file-scope extern/static declaration texts per name

    def _add(self, name: str, text: str, refs: set[str], path: Path) -> None:
        self.types.setdefault(name, set()).add(text)
        self.refs.setdefault(text, set()).update(refs)
        self.file_types.setdefault(path, set()).add(name)

    def scan(self, path: Path) -> None:
        raw = path.read_text(encoding="utf-8", errors="replace")
        code, comments = _strip_comments(raw)
        # Preprocessor directives (with continuations) come out first; #defines are indexed.
        lines = code.split("\n")
        body = []
        k = 0
        while k < len(lines):
            line = lines[k]
            if line.lstrip().startswith("#"):
                directive = line
                body.append(" " * len(line))
                while directive.endswith("\\") and k + 1 < len(lines):
                    k += 1
                    directive = directive[:-1] + " " + lines[k]
                    body.append(" " * len(lines[k]))
                m = re.match(r"\s*#\s*define\s+(\w+)(.*)", directive)
                if m:
                    text = _normalize("#define " + m.group(1) + m.group(2))
                    self._add(m.group(1), text, set(IDENT.findall(m.group(2))), path)
            else:
                body.append(line)
            k += 1
        code = "\n".join(body)
        # Comment text by position, for the "/* EnumName */" references.
        comment_at = sorted(comments)

        depth = 0
        start = 0
        for pos, ch in enumerate(code):
            if ch in "{(":
                depth += 1
            elif ch in "})":
                depth -= 1
                if depth == 0 and ch == "}":
                    stmt = code[start : pos + 1]
                    head = stmt.split("{", 1)[0]
                    first = next((w for w in IDENT.findall(stmt) if w not in PACK_MACROS), "")
                    if first not in ("typedef", *TAG_KEYWORDS) and "=" not in head:
                        start = pos + 1  # a function definition: no trailing ';'
            elif ch == ";" and depth == 0:
                stmt = code[start : pos + 1]
                notes = " ".join(t for off, t in comment_at if start <= off < pos)
                self._statement(stmt, notes, path)
                start = pos + 1

    def _statement(self, stmt: str, notes: str, path: Path) -> None:
        # END_PACKED_STRUCT closes the previous definition; PACKED_STRUCT stays in
        # the hashed text (packing is part of the layout).
        text = re.sub(r"^(?:END_PACKED_STRUCT\s*)+", "", _normalize(stmt))
        words = [w for w in IDENT.findall(text) if w not in PACK_MACROS]
        if not words:
            return
        refs = set(words) | set(IDENT.findall(notes))
        if words[0] == "typedef" or (words[0] in TAG_KEYWORDS and "{" in text):
            names = set()
            if "{" in text:
                m = re.match(r"(?:PACKED_STRUCT\s+)?(?:typedef\s+)?(?:struct|union|enum)\s+(\w+)\s*[:{]", text)
                if m:
                    names.add(m.group(1))
                tail = text[text.rfind("}") + 1 :]
                names.update(n for n in IDENT.findall(re.sub(r"\[[^\]]*\]", "", tail)) if n != "__attribute__")
                inner = text[text.find("{") + 1 : text.rfind("}")]
                if re.match(r"(?:typedef\s+)?enum\b", text):  # (enums are never packed)
                    for item in inner.split(","):
                        m2 = IDENT.match(item.strip())
                        if m2:
                            names.add(m2.group(0))
            else:
                fp = re.search(r"\(\s*\*\s*(\w+)\s*\)", text)
                names.add(fp.group(1) if fp else IDENT.findall(text)[-1])
            for name in names:
                self._add(name, text, refs, path)
        elif words[0] in ("extern", "static"):
            decl = re.sub(r"__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)", "", text.split("=")[0])
            if "(" not in decl:
                for name in IDENT.findall(re.sub(r"\[[^\]]*\]", "", decl))[-1:]:
                    self.decls.setdefault(name, []).append(decl)


def build_index() -> Index:
    index = Index()
    for path in sorted(SRC.rglob("*")):
        if path.suffix not in (".c", ".h") or SKIP_DIRS & set(path.relative_to(SRC).parts[:-1]):
            continue
        if "generated" in path.name:
            continue
        index.scan(path)
    return index


def section_globals() -> list[str]:
    code, _ = _strip_comments(STATE_DUMP.read_text(encoding="utf-8"))
    table = code[code.index("k_sections[] =") :]
    table = table[: table.index("};")]
    return re.findall(r"ADD(?:FN)?\(\s*SECTION_\w+\s*,\s*&?(\w+)", table)


def closure(index: Index) -> list[str]:
    """Sorted, de-duplicated definition texts reachable from the serialized globals."""
    seeds: set[str] = set()
    for name in section_globals():
        decls = index.decls.get(name)
        if not decls:
            raise SystemExit(f"savestate_layout: no declaration found for section global {name!r}")
        for decl in decls:
            seeds.update(IDENT.findall(decl))
    seeds.update(index.file_types.get(MODE_STACK, set()))
    seen_names: set[str] = set()
    texts: set[str] = set()
    todo = sorted(seeds)
    while todo:
        name = todo.pop()
        if name in seen_names:
            continue
        seen_names.add(name)
        for text in index.types.get(name, ()):
            if text in texts:
                continue
            texts.add(text)
            todo.extend(r for r in index.refs[text] if r not in seen_names)
    return sorted(texts)


def fingerprint() -> tuple[int, list[str]]:
    texts = closure(build_index())
    return zlib.crc32("\n".join(texts).encode()), texts


def render(value: int, count: int) -> str:
    return (
        "/* Generated by src/tools/savestate_layout.py: do not edit. Regenerate after\n"
        " * changing any type a savestate section serializes (the pytest suite fails\n"
        " * while this is stale). state_dump.c mixes it with the section id/size table\n"
        " * into the slot header's layout_id. */\n"
        "#ifndef EB_CORE_STATE_LAYOUT_H\n"
        "#define EB_CORE_STATE_LAYOUT_H\n"
        "\n"
        f"#define STATE_LAYOUT_SOURCE_HASH 0x{value:08X}u /* over {count} definitions */\n"
        "\n"
        "#endif /* EB_CORE_STATE_LAYOUT_H */\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true", help="exit 1 if the header is stale")
    parser.add_argument("--list", action="store_true", help="print the hashed definitions")
    args = parser.parse_args()
    value, texts = fingerprint()
    if args.list:
        for text in texts:
            print(text[:160])
        return 0
    want = render(value, len(texts))
    have = HEADER.read_text(encoding="utf-8") if HEADER.exists() else ""
    if args.check:
        if have != want:
            print(f"{HEADER} is stale: run python src/tools/savestate_layout.py", file=sys.stderr)
            return 1
        return 0
    if have != want:
        HEADER.write_text(want, encoding="utf-8")
        print(f"wrote {HEADER} (0x{value:08X}, {len(texts)} definitions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
