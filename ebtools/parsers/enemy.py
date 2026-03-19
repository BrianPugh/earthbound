"""Enemy config parser: 94-byte records -> assembly, JSON export, and packing.

Ported from app.d:parseEnemyConfig.
"""

import struct
from pathlib import Path

from pydantic import BaseModel, Field

from ebtools.byte_reader import ByteReader
from ebtools.config import CommonData, DumpDoc
from ebtools.parsers._common import format_pointer

RECORD_SIZE = 94
NAME_BYTES = 25  # bytes 1-24 are text, byte 25 is padding/null


class EnemyAction(BaseModel):
    """A single enemy action slot (action ID + argument)."""

    action: int = Field(ge=0, le=65535)
    argument: int = Field(ge=0, le=255)


class Enemy(BaseModel):
    """A single enemy configuration entry."""

    id: int = Field(ge=0, le=230)
    name: str
    the_flag: int = Field(ge=0, le=255)
    gender: str
    type: str
    battle_sprite: int = Field(ge=0, le=65535)
    overworld_sprite: int = Field(ge=0, le=65535)
    run_flag: int = Field(ge=0, le=255)
    hp: int = Field(ge=0, le=65535)
    pp: int = Field(ge=0, le=65535)
    experience: int = Field(ge=0, le=4294967295)
    money: int = Field(ge=0, le=65535)
    movement: int = Field(ge=0, le=65535)
    text_pointer_1: str  # hex string
    text_pointer_2: str  # hex string
    palette: int = Field(ge=0, le=255)
    level: int = Field(ge=0, le=255)
    music: str
    offense: int = Field(ge=0, le=65535)
    defense: int = Field(ge=0, le=65535)
    speed: int = Field(ge=0, le=255)
    guts: int = Field(ge=0, le=255)
    luck: int = Field(ge=0, le=255)
    weakness_fire: int = Field(ge=0, le=255)
    weakness_ice: int = Field(ge=0, le=255)
    weakness_flash: int = Field(ge=0, le=255)
    weakness_paralysis: int = Field(ge=0, le=255)
    weakness_hypnosis: int = Field(ge=0, le=255)
    miss_rate: int = Field(ge=0, le=255)
    action_order: int = Field(ge=0, le=255)
    actions: list[EnemyAction] = Field(min_length=5, max_length=5)
    iq: int = Field(ge=0, le=255)
    boss_flag: int = Field(ge=0, le=255)
    item_drop_rate: int = Field(ge=0, le=255)
    item_dropped: int = Field(ge=0, le=255)
    item_dropped_name: str = ""
    initial_status: int = Field(ge=0, le=255)
    death_style: int = Field(ge=0, le=255)
    row: int = Field(ge=0, le=255)
    max_allies_called: int = Field(ge=0, le=255)
    mirror_success_rate: int = Field(ge=0, le=255)


class EnemyConfig(BaseModel):
    """Top-level enemies.json schema."""

    enemies: list[Enemy]


def _decode_text(data: bytes, table: dict[int, str]) -> str:
    """Decode text bytes using the text table."""
    return "".join(table[b] for b in data if b in table)


def _encode_text(text: str, reverse_table: dict[str, int], total_len: int) -> bytearray:
    """Encode a string to EB text bytes, padded with 0x00 to total_len."""
    name_bytes = bytearray()
    for ch in text:
        if ch not in reverse_table:
            raise ValueError(f"Unmappable character '{ch}'")
        name_bytes.append(reverse_table[ch])
    if len(name_bytes) > total_len:
        raise ValueError(f"Name too long ({len(name_bytes)} > {total_len} bytes)")
    name_bytes.extend(b"\x00" * (total_len - len(name_bytes)))
    return name_bytes


def _ptr_to_int(hex_str: str) -> int:
    """Convert a hex string like '0xC539C4' to int."""
    return int(hex_str, 16)


def _int_to_ptr(val: int) -> str:
    """Convert an int to hex string like '0xC539C4'. 0 becomes '0x000000'."""
    return f"0x{val:06X}"


def _decode_enemy(
    idx: int,
    entry: bytes,
    text_table: dict[int, str],
    common_data: CommonData,
) -> Enemy:
    """Decode a single 94-byte enemy record into an Enemy model."""
    r = ByteReader(entry)

    the_flag = r.read_byte()
    name = _decode_text(entry[1:25], text_table)

    r.pos = 26
    gender_idx = r.read_byte()
    type_idx = r.read_byte()
    battle_sprite = r.read_le16()
    overworld_sprite = r.read_le16()
    run_flag = r.read_byte()
    hp = r.read_le16()
    pp = r.read_le16()
    experience = r.read_le32()
    money = r.read_le16()
    movement = r.read_le16()
    text_ptr_1 = r.read_le32()
    text_ptr_2 = r.read_le32()
    palette = r.read_byte()
    level = r.read_byte()
    music_idx = r.read_byte()
    offense = r.read_le16()
    defense = r.read_le16()
    speed = r.read_byte()
    guts = r.read_byte()
    luck = r.read_byte()
    weakness_fire = r.read_byte()
    weakness_ice = r.read_byte()
    weakness_flash = r.read_byte()
    weakness_paralysis = r.read_byte()
    weakness_hypnosis = r.read_byte()
    miss_rate = r.read_byte()
    action_order = r.read_byte()
    action_ids = [r.read_le16() for _ in range(5)]
    action_args = [r.read_byte() for _ in range(5)]
    iq = r.read_byte()
    boss_flag = r.read_byte()
    item_drop_rate = r.read_byte()
    item_dropped = r.read_byte()
    initial_status = r.read_byte()
    death_style = r.read_byte()
    row = r.read_byte()
    max_allies_called = r.read_byte()
    mirror_success_rate = r.read_byte()

    gender = common_data.genders[gender_idx] if gender_idx < len(common_data.genders) else f"UNKNOWN_{gender_idx}"
    enemy_type = common_data.enemyTypes[type_idx] if type_idx < len(common_data.enemyTypes) else f"UNKNOWN_{type_idx}"
    music = common_data.musicTracks[music_idx] if music_idx < len(common_data.musicTracks) else f"UNKNOWN_{music_idx}"
    item_name = common_data.items[item_dropped] if item_dropped < len(common_data.items) else f"UNKNOWN_{item_dropped}"

    actions = [EnemyAction(action=action_ids[i], argument=action_args[i]) for i in range(5)]

    return Enemy(
        id=idx,
        name=name,
        the_flag=the_flag,
        gender=gender,
        type=enemy_type,
        battle_sprite=battle_sprite,
        overworld_sprite=overworld_sprite,
        run_flag=run_flag,
        hp=hp,
        pp=pp,
        experience=experience,
        money=money,
        movement=movement,
        text_pointer_1=_int_to_ptr(text_ptr_1),
        text_pointer_2=_int_to_ptr(text_ptr_2),
        palette=palette,
        level=level,
        music=music,
        offense=offense,
        defense=defense,
        speed=speed,
        guts=guts,
        luck=luck,
        weakness_fire=weakness_fire,
        weakness_ice=weakness_ice,
        weakness_flash=weakness_flash,
        weakness_paralysis=weakness_paralysis,
        weakness_hypnosis=weakness_hypnosis,
        miss_rate=miss_rate,
        action_order=action_order,
        actions=actions,
        iq=iq,
        boss_flag=boss_flag,
        item_drop_rate=item_drop_rate,
        item_dropped=item_dropped,
        item_dropped_name=item_name,
        initial_status=initial_status,
        death_style=death_style,
        row=row,
        max_allies_called=max_allies_called,
        mirror_success_rate=mirror_success_rate,
    )


def export_enemies_json(
    enemy_data: bytes,
    text_table: dict[int, str],
    common_data: CommonData,
    output_path: Path,
) -> None:
    """Export enemy configuration binary to an editable JSON file."""
    enemies = []
    num_enemies = len(enemy_data) // RECORD_SIZE

    for idx in range(num_enemies):
        entry = enemy_data[idx * RECORD_SIZE : (idx + 1) * RECORD_SIZE]
        enemies.append(_decode_enemy(idx, entry, text_table, common_data))

    config = EnemyConfig(enemies=enemies)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        f.write(config.model_dump_json(indent=2))
        f.write("\n")


def _load_enemy_config(path: Path) -> EnemyConfig:
    """Load and validate enemies.json."""
    return EnemyConfig.model_validate_json(path.read_bytes())


def generate_enemies_header(enemies_json_path: Path, header_path: Path) -> None:
    """Generate enemies_generated.h from enemies.json.

    Produces an enum EnemyId and a static ENEMY_ID_NAMES array.
    """
    config = _load_enemy_config(enemies_json_path)
    enemies = config.enemies
    max_id = max(e.id for e in enemies) if enemies else 0
    count = max_id + 1

    lines = [
        "/* Auto-generated from enemies.json -- do not edit */",
        "#ifndef ENEMIES_GENERATED_H",
        "#define ENEMIES_GENERATED_H",
        "",
        "enum EnemyId {",
    ]

    for enemy in enemies:
        # Generate a C-safe symbol from the name
        symbol = enemy.name.strip().upper().replace(" ", "_").replace("'", "").replace(".", "")
        symbol = "".join(c if c.isalnum() or c == "_" else "_" for c in symbol)
        if not symbol:
            symbol = f"ENEMY_{enemy.id}"
        lines.append(f"    ENEMY_{symbol} = 0x{enemy.id:02X},")

    lines.append("};")
    lines.append("")
    lines.append(f"#define ENEMY_ID_COUNT {count}")
    lines.append("")
    lines.append("#ifdef ENEMIES_INCLUDE_NAMES")
    lines.append("static const char *ENEMY_ID_NAMES[ENEMY_ID_COUNT] = {")
    for enemy in enemies:
        display = enemy.name if enemy.name else f"Enemy {enemy.id}"
        display = display.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'    [0x{enemy.id:02X}] = "{display}",')
    lines.append("};")
    lines.append("#endif /* ENEMIES_INCLUDE_NAMES */")
    lines.append("")
    lines.append("#endif /* ENEMIES_GENERATED_H */")
    lines.append("")

    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text("\n".join(lines))


def pack_enemies(
    enemies_json_path: Path,
    text_table: dict[int, str],
    common_data: CommonData,
    output_path: Path,
) -> None:
    """Pack enemies.json back to a 231 x 94-byte binary."""
    reverse_table: dict[str, int] = {}
    for code, char in text_table.items():
        if char not in reverse_table:
            reverse_table[char] = code

    # Build reverse lookup maps for symbolic names
    gender_map = {name: idx for idx, name in enumerate(common_data.genders)}
    type_map = {name: idx for idx, name in enumerate(common_data.enemyTypes)}
    music_map = {name: idx for idx, name in enumerate(common_data.musicTracks)}

    config = _load_enemy_config(enemies_json_path)
    buf = bytearray()

    for enemy in config.enemies:
        # The Flag (u8) - byte 0
        buf.append(enemy.the_flag)
        # Name (25 bytes: up to 24 chars of text, padded with nulls) - bytes 1-25
        buf.extend(_encode_text(enemy.name, reverse_table, NAME_BYTES))
        # Gender (u8)
        buf.append(gender_map[enemy.gender])
        # Type (u8)
        buf.append(type_map[enemy.type])
        # Battle sprite (u16 LE)
        buf.extend(struct.pack("<H", enemy.battle_sprite))
        # Overworld sprite (u16 LE)
        buf.extend(struct.pack("<H", enemy.overworld_sprite))
        # Run flag (u8)
        buf.append(enemy.run_flag)
        # HP (u16 LE)
        buf.extend(struct.pack("<H", enemy.hp))
        # PP (u16 LE)
        buf.extend(struct.pack("<H", enemy.pp))
        # Experience (u32 LE)
        buf.extend(struct.pack("<I", enemy.experience))
        # Money (u16 LE)
        buf.extend(struct.pack("<H", enemy.money))
        # Movement (u16 LE)
        buf.extend(struct.pack("<H", enemy.movement))
        # Text pointers (u32 LE each)
        buf.extend(struct.pack("<I", _ptr_to_int(enemy.text_pointer_1)))
        buf.extend(struct.pack("<I", _ptr_to_int(enemy.text_pointer_2)))
        # Palette (u8)
        buf.append(enemy.palette)
        # Level (u8)
        buf.append(enemy.level)
        # Music (u8)
        buf.append(music_map[enemy.music])
        # Offense (u16 LE)
        buf.extend(struct.pack("<H", enemy.offense))
        # Defense (u16 LE)
        buf.extend(struct.pack("<H", enemy.defense))
        # Speed, Guts, Luck (u8 each)
        buf.append(enemy.speed)
        buf.append(enemy.guts)
        buf.append(enemy.luck)
        # Weaknesses (5 x u8)
        buf.append(enemy.weakness_fire)
        buf.append(enemy.weakness_ice)
        buf.append(enemy.weakness_flash)
        buf.append(enemy.weakness_paralysis)
        buf.append(enemy.weakness_hypnosis)
        # Miss rate, Action order (u8 each)
        buf.append(enemy.miss_rate)
        buf.append(enemy.action_order)
        # 5 action IDs (u16 LE each)
        for action in enemy.actions:
            buf.extend(struct.pack("<H", action.action))
        # 5 action arguments (u8 each)
        for action in enemy.actions:
            buf.append(action.argument)
        # IQ (u8)
        buf.append(enemy.iq)
        # Boss flag (u8)
        buf.append(enemy.boss_flag)
        # Item drop rate (u8)
        buf.append(enemy.item_drop_rate)
        # Item dropped (u8)
        buf.append(enemy.item_dropped)
        # Initial status (u8)
        buf.append(enemy.initial_status)
        # Death style (u8)
        buf.append(enemy.death_style)
        # Row (u8)
        buf.append(enemy.row)
        # Max allies called (u8)
        buf.append(enemy.max_allies_called)
        # Mirror success rate (u8)
        buf.append(enemy.mirror_success_rate)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


def parse_enemy_config(
    dir: Path,
    base_name: str,
    extension: str,
    source: bytes,
    offset: int,
    doc: DumpDoc,
    common_data: CommonData,
) -> list[str]:
    """Parse enemy configuration (94-byte records) to assembly."""
    filename = f"{base_name}.{extension}"

    with (dir / filename).open("w") as out:
        for i in range(0, len(source), RECORD_SIZE):
            entry = source[i : i + RECORD_SIZE]
            if len(entry) < RECORD_SIZE:
                break

            r = ByteReader(entry)

            out.write(f"  .BYTE ${r.read_byte():02X} ;The Flag\n")
            out.write(f'  PADDEDEBTEXT "{_decode_text(entry[1:25], doc.textTable)}", 25\n')
            r.pos = 26
            out.write(f"  .BYTE GENDER::{common_data.genders[r.read_byte()]}\n")
            out.write(f"  .BYTE ENEMYTYPE::{common_data.enemyTypes[r.read_byte()]}\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Battle sprite\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Out-of-battle sprite\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Run flag\n")
            out.write(f"  .WORD {r.read_le16()} ;HP\n")
            out.write(f"  .WORD {r.read_le16()} ;PP\n")
            out.write(f"  .DWORD {r.read_le32()} ;Experience\n")
            out.write(f"  .WORD {r.read_le16()} ;Money\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Movement\n")
            out.write(format_pointer(r.read_le32()))
            out.write(format_pointer(r.read_le32()))
            out.write(f"  .BYTE ${r.read_byte():02X} ;Palette\n")
            out.write(f"  .BYTE {r.read_byte()} ;Level\n")
            out.write(f"  .BYTE MUSIC::{common_data.musicTracks[r.read_byte()]}\n")
            out.write(f"  .WORD {r.read_le16()} ;Offense\n")
            out.write(f"  .WORD {r.read_le16()} ;Defense\n")
            out.write(f"  .BYTE {r.read_byte()} ;Speed\n")
            out.write(f"  .BYTE {r.read_byte()} ;Guts\n")
            out.write(f"  .BYTE {r.read_byte()} ;Luck\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Weakness to fire\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Weakness to ice\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Weakness to flash\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Weakness to paralysis\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Weakness to hypnosis/brainshock\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Miss rate\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Action order\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Action 1\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Action 2\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Action 3\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Action 4\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Final action\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Action 1 argument\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Action 2 argument\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Action 3 argument\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Action 4 argument\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Final action argument\n")
            out.write(f"  .BYTE {r.read_byte()} ;IQ\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Boss flag\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Item drop rate\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Item dropped\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Initial status\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Death style\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Row\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Max number of allies called\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Mirror success rate\n")
            out.write("\n")

    return [filename]
