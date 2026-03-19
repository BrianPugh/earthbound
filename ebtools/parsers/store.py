"""Store/shop table parser: 66 shops x 7 item IDs -> JSON export and packing."""

from pathlib import Path

from pydantic import BaseModel, Field

from ebtools.config import CommonData

ITEMS_PER_STORE = 7


class Store(BaseModel):
    """A single store inventory (7 item slots)."""

    id: int = Field(ge=0)
    items: list[int] = Field(min_length=ITEMS_PER_STORE, max_length=ITEMS_PER_STORE)
    item_names: list[str] = Field(min_length=ITEMS_PER_STORE, max_length=ITEMS_PER_STORE)


class StoreTable(BaseModel):
    """Top-level stores.json schema."""

    stores: list[Store]


def export_stores_json(
    data: bytes,
    common_data: CommonData,
    output_path: Path,
) -> None:
    """Export store table binary to JSON."""
    stores = []
    num = len(data) // ITEMS_PER_STORE

    for idx in range(num):
        offset = idx * ITEMS_PER_STORE
        items = list(data[offset : offset + ITEMS_PER_STORE])
        item_names = [
            common_data.items[item_id] if item_id < len(common_data.items) else f"UNKNOWN_{item_id}"
            for item_id in items
        ]
        stores.append(Store(id=idx, items=items, item_names=item_names))

    config = StoreTable(stores=stores)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        f.write(config.model_dump_json(indent=2))
        f.write("\n")


def pack_stores(json_path: Path, common_data: CommonData, output_path: Path) -> None:
    """Pack stores.json back to binary."""
    config = StoreTable.model_validate_json(json_path.read_bytes())
    buf = bytearray()

    for store in config.stores:
        buf.extend(store.items)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))
