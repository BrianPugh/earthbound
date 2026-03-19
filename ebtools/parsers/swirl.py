"""Swirl effect parser: HDMA window table data -> JSON export and packing.

Each swirl file contains variable-length HDMA frame data that defines
the battle transition window animation effect. The raw binary data is
preserved as hex strings for lossless round-trip.
"""

from pathlib import Path

from pydantic import BaseModel, computed_field


class SwirlEntry(BaseModel):
    """A single swirl effect."""

    id: int
    data_hex: str

    @computed_field
    @property
    def size(self) -> int:
        return len(self.data)

    @property
    def data(self) -> bytes:
        return bytes.fromhex(self.data_hex)


class SwirlConfig(BaseModel):
    """Container for all swirl effects."""

    swirls: list[SwirlEntry]


def export_swirls_json(bin_dir: Path, output_path: Path) -> int:
    """Export all swirl files to a single JSON file.

    Returns the number of swirls exported.
    """
    swirl_dir = bin_dir / "swirls"
    if not swirl_dir.exists():
        return 0

    swirls = []
    for swirl_file in sorted(swirl_dir.glob("*.swirl"), key=lambda p: int(p.stem)):
        swirl_id = int(swirl_file.stem)
        data = swirl_file.read_bytes()
        swirls.append(SwirlEntry(id=swirl_id, data_hex=data.hex()))

    if not swirls:
        return 0

    config = SwirlConfig(swirls=swirls)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(config.model_dump_json(indent=2) + "\n")
    return len(swirls)


def pack_swirls(json_path: Path, output_dir: Path) -> None:
    """Pack swirls.json back to individual .swirl binary files."""
    config = SwirlConfig.model_validate_json(json_path.read_bytes())

    swirl_dir = output_dir / "swirls"
    swirl_dir.mkdir(parents=True, exist_ok=True)

    for swirl in config.swirls:
        (swirl_dir / f"{swirl.id}.swirl").write_bytes(swirl.data)
