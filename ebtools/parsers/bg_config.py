"""Battle background config table parser: 17-byte records -> JSON export and packing."""

from pathlib import Path

from pydantic import BaseModel, Field

from ebtools.byte_reader import ByteReader

RECORD_SIZE = 17


class BGConfigEntry(BaseModel):
    """A single battle background config entry (17 bytes)."""

    id: int = Field(ge=0)
    graphics_index: int = Field(ge=0, le=255)
    palette_index: int = Field(ge=0, le=255)
    bits_per_pixel: int = Field(ge=0, le=255)
    palette_animation_type: int = Field(ge=0, le=255)
    palette_animation_param_1: int = Field(ge=0, le=255)
    palette_animation_param_2: int = Field(ge=0, le=255)
    scroll_type: int = Field(ge=0, le=255)
    scroll_speed_h: int = Field(ge=0, le=255)
    scroll_speed_v: int = Field(ge=0, le=255)
    distortion_type: int = Field(ge=0, le=255)
    distortion_param: int = Field(ge=0, le=255)
    arrangement_index: int = Field(ge=0, le=255)
    unknown_12: int = Field(ge=0, le=255)
    unknown_13: int = Field(ge=0, le=255)
    unknown_14: int = Field(ge=0, le=255)
    unknown_15: int = Field(ge=0, le=255)
    unknown_16: int = Field(ge=0, le=255)


class BGConfigTable(BaseModel):
    """Top-level bg_config.json schema."""

    entries: list[BGConfigEntry]


def export_bg_config_json(data: bytes, output_path: Path) -> None:
    """Export BG config table binary to JSON."""
    entries = []
    num = len(data) // RECORD_SIZE

    for idx in range(num):
        r = ByteReader(data[idx * RECORD_SIZE : (idx + 1) * RECORD_SIZE])
        entries.append(
            BGConfigEntry(
                id=idx,
                graphics_index=r.read_byte(),
                palette_index=r.read_byte(),
                bits_per_pixel=r.read_byte(),
                palette_animation_type=r.read_byte(),
                palette_animation_param_1=r.read_byte(),
                palette_animation_param_2=r.read_byte(),
                scroll_type=r.read_byte(),
                scroll_speed_h=r.read_byte(),
                scroll_speed_v=r.read_byte(),
                distortion_type=r.read_byte(),
                distortion_param=r.read_byte(),
                arrangement_index=r.read_byte(),
                unknown_12=r.read_byte(),
                unknown_13=r.read_byte(),
                unknown_14=r.read_byte(),
                unknown_15=r.read_byte(),
                unknown_16=r.read_byte(),
            )
        )

    config = BGConfigTable(entries=entries)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        f.write(config.model_dump_json(indent=2))
        f.write("\n")


def pack_bg_config(json_path: Path, output_path: Path) -> None:
    """Pack bg_config.json back to binary."""
    config = BGConfigTable.model_validate_json(json_path.read_bytes())
    buf = bytearray()

    for entry in config.entries:
        buf.append(entry.graphics_index)
        buf.append(entry.palette_index)
        buf.append(entry.bits_per_pixel)
        buf.append(entry.palette_animation_type)
        buf.append(entry.palette_animation_param_1)
        buf.append(entry.palette_animation_param_2)
        buf.append(entry.scroll_type)
        buf.append(entry.scroll_speed_h)
        buf.append(entry.scroll_speed_v)
        buf.append(entry.distortion_type)
        buf.append(entry.distortion_param)
        buf.append(entry.arrangement_index)
        buf.append(entry.unknown_12)
        buf.append(entry.unknown_13)
        buf.append(entry.unknown_14)
        buf.append(entry.unknown_15)
        buf.append(entry.unknown_16)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))
