"""The ``embed-registry`` command: generate C source for embedded binary assets."""

import fnmatch
import re
import sys
import textwrap
from collections import defaultdict
from pathlib import Path
from typing import Annotated

from cyclopts import Parameter

# Locale prefixes in priority order.
_LOCALE_PREFIXES = ("US/", "JP/")


def _path_to_identifier(path: str) -> str:
    """Convert a relative file path to a valid C identifier."""
    ident = re.sub(r"[^a-zA-Z0-9]", "_", path)
    if ident and ident[0].isdigit():
        ident = "a" + ident
    return "asset_" + ident


def _path_to_enum_name(path: str) -> str:
    """Convert a relative file path to an UPPER_SNAKE_CASE enum name."""
    name = re.sub(r"[^a-zA-Z0-9]", "_", path).upper()
    if name and name[0].isdigit():
        name = "A" + name
    return "ASSET_" + name


def _collect_assets(manifest_path: Path, bin_dir: Path, custom_dir: Path) -> dict[str, Path]:
    """Collect binary asset files listed in the manifest, with custom_dir overrides."""
    assets: dict[str, Path] = {}

    for line in manifest_path.read_text().splitlines():
        rel_path = line.strip()
        if not rel_path:
            continue
        full_path = bin_dir / rel_path
        if full_path.is_file():
            assets[rel_path] = full_path.resolve()

    if custom_dir.is_dir():
        for child in custom_dir.rglob("*"):
            if not child.is_file() or child.name == ".gitkeep":
                continue
            rel_path = child.relative_to(custom_dir).as_posix()
            if "/png/" in rel_path or rel_path.startswith("png/"):
                continue
            assets[rel_path] = child.resolve()

    return assets


def _detect_families(
    paths: list[str],
    locale: str,
) -> tuple[list[str], dict[str, list[tuple[int, str]]]]:
    """Detect parameterized families (assets with purely numeric basenames).

    Locale-prefixed families (e.g. US/maps/palettes/) are merged with their
    common counterparts (maps/palettes/) into a single family keyed by the
    unprefixed directory.  For each index, the locale-specific version is
    preferred if it exists.

    Returns (singletons, families) where:
      singletons: paths that are NOT part of a numeric family
      families: dict mapping family_key -> [(numeric_index, full_path), ...]
    """
    locale_prefix = f"{locale}/"

    groups: dict[str, list[tuple[str, str]]] = defaultdict(list)
    for path in paths:
        parts = path.rsplit("/", 1)
        if len(parts) == 2:
            directory, filename = parts
        else:
            directory, filename = "", parts[0]

        dot_idx = filename.find(".")
        if dot_idx >= 0:
            basename = filename[:dot_idx]
            ext = filename[dot_idx:]
        else:
            basename = filename
            ext = ""

        key = f"{directory}/{ext}" if directory else ext
        groups[key].append((basename, path))

    # First pass: identify numeric families within each group, then merge
    # locale-prefixed families with their common counterparts.
    raw_families: dict[str, list[tuple[int, str]]] = {}
    singletons: list[str] = []

    for key, members in groups.items():
        numeric_members: list[tuple[int, str]] = []
        non_numeric: list[str] = []
        for basename, path in members:
            try:
                idx = int(basename)
                numeric_members.append((idx, path))
            except ValueError:
                non_numeric.append(path)

        # Non-numeric members are always singletons
        singletons.extend(non_numeric)

        if len(numeric_members) >= 2:
            raw_families[key] = numeric_members
        elif numeric_members:
            # Single numeric member — treat as singleton
            singletons.append(numeric_members[0][1])

    # Merge locale-prefixed families with common counterparts.
    # E.g. "US/maps/palettes/.pal" merges into "maps/palettes/.pal".
    families: dict[str, list[tuple[int, str]]] = {}
    for key, members in raw_families.items():
        base_key = key
        for prefix in _LOCALE_PREFIXES:
            if key.startswith(prefix):
                base_key = key[len(prefix) :]
                break
        if base_key not in families:
            families[base_key] = []
        families[base_key].extend(members)

    # Deduplicate: for each index, prefer locale-specific version
    for key in families:
        idx_to_path: dict[int, str] = {}
        for idx, path in families[key]:
            if idx not in idx_to_path or path.startswith(locale_prefix):
                idx_to_path[idx] = path
        families[key] = sorted(idx_to_path.items())

    singletons.sort()
    return singletons, families


def _build_locale_map(all_paths: set[str], locale: str) -> dict[str, str]:
    """Build mapping from unprefixed base path to actual locale-specific path."""
    prefix = f"{locale}/"
    locale_map: dict[str, str] = {}
    for path in all_paths:
        if path.startswith(prefix):
            base = path[len(prefix) :]
            locale_map[base] = path
    return locale_map


def _family_macro_name(dir_part: str) -> str:
    """Generate a family macro name from the directory part."""
    macro_base = re.sub(r"[^a-zA-Z0-9]", "_", dir_part).upper()
    if macro_base and macro_base[0].isdigit():
        macro_base = "A" + macro_base
    return f"ASSET_{macro_base}" if macro_base else "ASSET_FAMILY"


def embed_registry(
    manifest: Path,
    bin_dir: Path,
    output_dir: Path,
    incbin_dir: Path,
    *,
    custom_dir: Path = Path("src/custom_assets"),
    exclude: list[str] | None = None,
    locale: str = "US",
) -> None:
    """Generate C source files for embedding binary assets using incbin.h.

    Parameters
    ----------
    manifest
        Path to assets.manifest (generated during extraction).
    bin_dir
        Path to extracted ROM assets (e.g. asm/bin/).
    output_dir
        Directory for generated C files.
    incbin_dir
        Path to directory containing incbin.h.
    custom_dir
        Path to custom asset overrides.
    exclude
        Glob patterns for asset paths to exclude (e.g. ``audiopacks/*``).
    locale
        Build locale for resolving locale-specific assets (US or JP).
    """
    output_dir.mkdir(parents=True, exist_ok=True)

    assets = _collect_assets(manifest, bin_dir, custom_dir)

    if exclude:
        assets = {k: v for k, v in assets.items() if not any(fnmatch.fnmatch(k, pat) for pat in exclude)}

    sorted_paths = sorted(assets.keys())

    if not sorted_paths:
        print("Warning: no assets found", file=sys.stderr)

    header = "/* Auto-generated by ebtools embed-registry - do not edit */\n"
    incbin_path = incbin_dir.resolve().as_posix()

    # --- embedded_assets.inc.c (INCBIN definitions) ---
    inc_path = output_dir / "embedded_assets.inc.c"
    incbin_lines = [f'INCBIN({_path_to_identifier(p)}, "{assets[p].as_posix()}");' for p in sorted_paths]
    inc_path.write_text(header + "\n" + "\n".join(incbin_lines) + "\n")

    # --- embedded_assets_decl.h (extern declarations for INCBIN symbols) ---
    decl_path = output_dir / "embedded_assets_decl.h"
    decl_lines = [header, "#ifndef EMBEDDED_ASSETS_DECL_H", "#define EMBEDDED_ASSETS_DECL_H", ""]
    for p in sorted_paths:
        ident = _path_to_identifier(p)
        decl_lines.append(f"extern const unsigned char {ident}Data[];")
        decl_lines.append(f"extern const unsigned int {ident}Size;")
    decl_lines.extend(["", "#endif /* EMBEDDED_ASSETS_DECL_H */", ""])
    decl_path.write_text("\n".join(decl_lines))

    # ===================================================================
    # Direct-access asset system: no global array, zero indirection
    # ===================================================================

    locale_map = _build_locale_map(set(sorted_paths), locale)
    singletons, families = _detect_families(sorted_paths, locale)

    # Build path → incbin identifier mapping
    path_to_incbin: dict[str, str] = {p: _path_to_identifier(p) for p in sorted_paths}

    # Build enum entries (still needed for family indexing compatibility)
    enum_entries: list[tuple[str, str]] = []  # (enum_name, path_comment)
    enum_to_incbin: dict[str, str | None] = {}
    path_to_enum: dict[str, str] = {}

    for path in singletons:
        enum_name = _path_to_enum_name(path)
        enum_entries.append((enum_name, path))
        enum_to_incbin[enum_name] = path_to_incbin[path]
        path_to_enum[path] = enum_name

    # Track family info for array generation
    # family_key → (macro_name, min_idx, max_idx, [(idx, path, incbin_id), ...])
    family_info: dict[str, tuple[str, int, int, list[tuple[int, str | None, str | None]]]] = {}

    for key in sorted(families.keys()):
        members = families[key]
        min_idx = members[0][0]
        max_idx = members[-1][0]
        idx_to_path = dict(members)

        parts = key.rsplit("/", 1)
        dir_part = parts[0] if len(parts) == 2 else ""
        ext_part = parts[1] if len(parts) == 2 else parts[0]
        macro_name = _family_macro_name(dir_part)

        entries: list[tuple[int, str | None, str | None]] = []
        first_enum = None
        locale_prefix = f"{locale}/"
        for idx in range(min_idx, max_idx + 1):
            path = idx_to_path.get(idx)
            if path:
                # For merged families, always use the unprefixed enum name
                # so family members are contiguous regardless of locale.
                base_path = path[len(locale_prefix) :] if path.startswith(locale_prefix) else path
                enum_name = _path_to_enum_name(base_path)
                incbin_id = path_to_incbin[path]
                enum_entries.append((enum_name, path))
                enum_to_incbin[enum_name] = incbin_id
                path_to_enum[path] = enum_name
                entries.append((idx, path, incbin_id))
            else:
                sentinel_path = f"{dir_part}/{idx}{ext_part}" if dir_part else f"{idx}{ext_part}"
                enum_name = _path_to_enum_name(sentinel_path)
                enum_entries.append((enum_name, f"[gap] {sentinel_path}"))
                enum_to_incbin[enum_name] = None
                entries.append((idx, None, None))

            if first_enum is None:
                first_enum = enum_name

        comment = f"{dir_part}/<n>{ext_part} n={min_idx}..{max_idx}"
        family_info[key] = (macro_name, min_idx, max_idx, entries)

    # Build locale aliases
    locale_aliases: dict[str, str] = {}
    # Also build locale aliases for _DATA/_SIZE macros
    locale_data_aliases: dict[str, str] = {}  # ASSET_FOO_DATA → ASSET_US_FOO_DATA
    locale_size_aliases: dict[str, str] = {}  # ASSET_FOO_SIZE → ASSET_US_FOO_SIZE
    for base_path, actual_path in locale_map.items():
        if actual_path in path_to_enum:
            target_enum = path_to_enum[actual_path]
            alias_enum = _path_to_enum_name(base_path)
            if alias_enum != target_enum and alias_enum not in enum_to_incbin:
                locale_aliases[alias_enum] = target_enum
                locale_data_aliases[f"{alias_enum}_DATA"] = f"{target_enum}_DATA"
                locale_size_aliases[f"{alias_enum}_SIZE"] = f"{target_enum}_SIZE"

    # ===================================================================
    # Generate asset_ids.h with direct-access macros
    # ===================================================================
    ids_lines = [
        header,
        "#ifndef ASSET_IDS_H",
        "#define ASSET_IDS_H",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        '#include "embedded_assets_decl.h"',
        "",
    ]

    # Enum (kept for family indexing and backward compat)
    ids_lines.append("typedef enum {")
    for enum_name, comment in enum_entries:
        ids_lines.append(f"    {enum_name}, /* {comment} */")
    ids_lines.append(f"    ASSET_COUNT  /* {len(enum_entries)} */")
    ids_lines.append("} AssetId;")
    ids_lines.append("")

    # Direct-access macros for each asset: ASSET_FOO_DATA / ASSET_FOO_SIZE
    ids_lines.append("/* Direct-access macros: zero indirection, resolved at link time */")
    for enum_name, _comment in enum_entries:
        incbin_id = enum_to_incbin.get(enum_name)
        if incbin_id is not None:
            ids_lines.append(f"#define {enum_name}_DATA ((const uint8_t *){incbin_id}Data)")
            ids_lines.append(f"#define {enum_name}_SIZE ((size_t){incbin_id}Size)")
        else:
            # Gap sentinel
            ids_lines.append(f"#define {enum_name}_DATA ((const uint8_t *)0)")
            ids_lines.append(f"#define {enum_name}_SIZE ((size_t)0)")
    ids_lines.append("")

    # Locale-resolved aliases (enum + _DATA + _SIZE)
    if locale_aliases:
        ids_lines.append("/* Locale-resolved aliases */")
        for alias, target in sorted(locale_aliases.items()):
            ids_lines.append(f"#define {alias} {target}")
        for alias, target in sorted(locale_data_aliases.items()):
            ids_lines.append(f"#define {alias} {target}")
        for alias, target in sorted(locale_size_aliases.items()):
            ids_lines.append(f"#define {alias} {target}")
        ids_lines.append("")

    # Per-family array declarations and accessor macros
    ids_lines.append("/* Per-family lookup (for runtime-indexed access) */")
    ids_lines.append("typedef struct { const unsigned char *data; const unsigned int *size_ptr; } AssetFamilyEntry;")
    ids_lines.append("")

    for key in sorted(family_info.keys()):
        macro_name, min_idx, max_idx, entries = family_info[key]
        count = max_idx - min_idx + 1
        array_name = f"asset_family_{macro_name.lower()[6:]}"  # strip ASSET_ prefix

        # Find the first enum name for this family (use unprefixed name)
        first_enum_name = None
        lp = f"{locale}/"
        for _idx, path, _incbin_id in entries:
            if path is not None:
                base = path[len(lp) :] if path.startswith(lp) else path
                first_enum_name = _path_to_enum_name(base)
                break

        # Enum-style macro: ASSET_FAMILY(n) → enum value (for use with ASSET_DATA/ASSET_SIZE)
        if first_enum_name is not None:
            if min_idx == 0:
                ids_lines.append(f"#define {macro_name}(n) ({first_enum_name} + (n))")
            else:
                ids_lines.append(f"#define {macro_name}(n) ({first_enum_name} + (n) - {min_idx})")

        # Per-family array + direct-access macros
        ids_lines.append(f"extern const AssetFamilyEntry {array_name}[{count}];")
        if min_idx == 0:
            ids_lines.append(f"#define {macro_name}_DATA(n) ((const uint8_t *){array_name}[n].data)")
            ids_lines.append(f"#define {macro_name}_SIZE(n) ((size_t)*{array_name}[n].size_ptr)")
        else:
            ids_lines.append(f"#define {macro_name}_DATA(n) ((const uint8_t *){array_name}[(n) - {min_idx}].data)")
            ids_lines.append(f"#define {macro_name}_SIZE(n) ((size_t)*{array_name}[(n) - {min_idx}].size_ptr)")

    ids_lines.extend(["", "#endif /* ASSET_IDS_H */", ""])

    ids_path = output_dir / "asset_ids.h"
    ids_path.write_text("\n".join(ids_lines))

    # ===================================================================
    # Generate embedded_assets_array.c (INCBIN definitions + family arrays)
    # ===================================================================
    array_lines = [
        header,
        "#define INCBIN_PREFIX",
        f'#include "{incbin_path}/incbin.h"',
        '#include "asset_ids.h"',
        '#include "embedded_assets.h"',
        "",
        f'#include "{inc_path.resolve().as_posix()}"',
        "",
    ]

    # Global array: const-initialized with {data_ptr, &size_ptr} pairs.
    # Both are link-time constants, so no runtime init needed.
    array_lines.append("const AssetEntry embedded_assets[ASSET_COUNT] = {")
    for enum_name, _comment in enum_entries:
        incbin_id = enum_to_incbin.get(enum_name)
        if incbin_id is not None:
            array_lines.append(f"    [{enum_name}] = {{ {incbin_id}Data, &{incbin_id}Size }},")
        else:
            array_lines.append(f"    [{enum_name}] = {{ 0, 0 }},")
    array_lines.append("};")
    array_lines.append("")

    # Per-family arrays
    for key in sorted(family_info.keys()):
        macro_name, min_idx, max_idx, entries = family_info[key]
        count = max_idx - min_idx + 1
        array_name = f"asset_family_{macro_name.lower()[6:]}"

        array_lines.append(f"const AssetFamilyEntry {array_name}[{count}] = {{")
        for idx, _path, incbin_id in entries:
            if incbin_id is not None:
                array_lines.append(f"    {{ {incbin_id}Data, &{incbin_id}Size }}, /* {idx} */")
            else:
                array_lines.append(f"    {{ 0, 0 }}, /* {idx} [gap] */")
        array_lines.append("};")
        array_lines.append("")

    array_path = output_dir / "embedded_assets_array.c"
    array_path.write_text("\n".join(array_lines))

    # ===================================================================
    # Generate embedded_assets.h (declares AssetEntry + global array)
    # ===================================================================
    hdr_path = output_dir / "embedded_assets.h"
    hdr_path.write_text(
        header
        + textwrap.dedent("""\
        #ifndef EMBEDDED_ASSETS_H
        #define EMBEDDED_ASSETS_H

        #include "asset_ids.h"

        /* Asset entry: data pointer + pointer to INCBIN-generated size variable.
         * Both fields are link-time constants, so the array is fully const
         * (lives in .rodata / flash on embedded targets, no init function needed). */
        typedef struct {
            const unsigned char *data;
            const unsigned int *size_ptr;
        } AssetEntry;

        extern const AssetEntry embedded_assets[ASSET_COUNT];

        #endif /* EMBEDDED_ASSETS_H */
    """)
    )

    print(
        f"Generated {len(sorted_paths)} embedded assets ({len(enum_entries)} enum entries, "
        f"{len(family_info)} families, {len(locale_aliases)} locale aliases) in {output_dir}"
    )
