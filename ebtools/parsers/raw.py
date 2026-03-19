"""Raw binary parser: writes data bytes directly to output file."""

from pathlib import Path

from ebtools.config import CommonData, DumpDoc


def write_raw(
    dir: Path,
    base_name: str,
    extension: str,
    data: bytes,
    offset: int,
    doc: DumpDoc,
    common_data: CommonData,
) -> list[str]:
    """Write raw binary data to a file."""
    filename = f"{base_name}.{extension}"
    (dir / filename).write_bytes(data)
    return [filename]
