"""Search dialogue YAML and JSON config files for text strings."""

import json
from pathlib import Path
from typing import Annotated

from cyclopts import App, Parameter

text_app = App(name="text", help="Text search and inspection tools.")


@text_app.command
def search(
    query: str,
    *,
    assets_dir: Annotated[Path, Parameter(help="Path to assets directory")] = Path("src/assets"),
    case_sensitive: Annotated[bool, Parameter(help="Enable case-sensitive matching")] = False,
) -> None:
    """Search dialogue and config files for a text string."""
    total_matches = 0
    files_with_matches = 0

    # Search dialogue YAML files
    dialogue_dir = assets_dir / "dialogue"
    if dialogue_dir.is_dir():
        yml_files = sorted(dialogue_dir.glob("*.yml"))
        for yml_file in yml_files:
            matches = _search_dialogue_yaml(yml_file, query, case_sensitive)
            if matches:
                files_with_matches += 1
                total_matches += len(matches)

    # Search JSON config files for inline text
    json_searches = [
        (assets_dir / "items" / "items.json", "items", "items", _search_items_json),
        (
            assets_dir / "battle" / "battle_actions.json",
            "actions",
            "actions",
            _search_battle_actions_json,
        ),
    ]
    for json_path, _root_key, _display_name, search_fn in json_searches:
        if json_path.is_file():
            matches = search_fn(json_path, query, case_sensitive)
            if matches:
                files_with_matches += 1
                total_matches += len(matches)

    print(
        f"\nFound {total_matches} match{'es' if total_matches != 1 else ''} in {files_with_matches} file{'s' if files_with_matches != 1 else ''}"
    )


def _matches(text: str, query: str, case_sensitive: bool) -> bool:
    if case_sensitive:
        return query in text
    return query.lower() in text.lower()


def _search_dialogue_yaml(yml_path: Path, query: str, case_sensitive: bool) -> list[str]:
    """Search a dialogue YAML file for text matches, printing results with context."""
    lines = yml_path.read_text().splitlines()
    matches = []

    # Find all text lines that match
    matching_line_numbers: list[int] = []
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("- text:"):
            # Extract text value after "- text:"
            text_value = stripped[len("- text:") :].strip()
            if _matches(text_value, query, case_sensitive):
                matching_line_numbers.append(i)

    if not matching_line_numbers:
        return []

    # For each match, find the enclosing label and show context
    for match_line_num in matching_line_numbers:
        # Find the enclosing label (walk backwards to find a non-indented line starting with a letter or _)
        label = "???"
        label_line = 0
        for i in range(match_line_num, -1, -1):
            line = lines[i]
            if line and not line[0].isspace() and not line.startswith("-") and line.rstrip().endswith(":"):
                label = line.rstrip().rstrip(":")
                label_line = i + 1  # 1-based
                break

        # Print header
        rel_path = yml_path
        print(f"{rel_path} :: {label} (line {label_line})")

        # Show context: 3 lines before and 3 lines after the match (within the label block)
        context_before = 3
        context_after = 3
        start = max(match_line_num - context_before, 0)
        end = min(match_line_num + context_after + 1, len(lines))

        # Don't show lines before the label unless they're part of it
        if start < label_line - 1:
            start = label_line - 1  # label_line is 1-based

        for i in range(start, end):
            line = lines[i]
            marker = "    <-- match" if i == match_line_num else ""
            print(f"  {line}{marker}")

        # Show ellipsis if there are more lines in the block after context
        if end < len(lines) and lines[end].startswith(" "):
            print("  ...")
        print()

        matches.append(label)

    return matches


def _search_items_json(json_path: Path, query: str, case_sensitive: bool) -> list[str]:
    """Search items.json help_text fields."""
    data = json.loads(json_path.read_text())
    matches = []

    for item in data.get("items", []):
        help_text = item.get("help_text", "")
        if help_text and _matches(help_text, query, case_sensitive):
            item_id = item.get("id", "?")
            name = item.get("name", "???")
            # Truncate long help text for display
            display_text = help_text
            if len(display_text) > 120:
                display_text = display_text[:120] + "..."
            print(f'{json_path} :: Item #{item_id} "{name}" help_text: "{display_text}"')
            matches.append(name)

    return matches


def _search_battle_actions_json(json_path: Path, query: str, case_sensitive: bool) -> list[str]:
    """Search battle_actions.json description fields."""
    data = json.loads(json_path.read_text())
    matches = []

    for action in data.get("actions", []):
        description = action.get("description", "")
        if description and _matches(description, query, case_sensitive):
            action_id = action.get("id", "?")
            display_text = description
            if len(display_text) > 120:
                display_text = display_text[:120] + "..."
            print(f'{json_path} :: Action #{action_id} description: "{display_text}"')
            matches.append(str(action_id))

    return matches
