"""YAML configuration models for DumpDoc and CommonData."""

from pathlib import Path

import yaml
from pydantic import BaseModel, ConfigDict, Field


class Music(BaseModel):
    model_config = ConfigDict(extra="ignore")

    packPointerTable: int = 0
    songPointerTable: int = 0
    numPacks: int = 0


class DumpInfo(BaseModel):
    model_config = ConfigDict(extra="ignore")

    subdir: str = ""
    name: str = ""
    offset: int = 0
    size: int = 0
    extension: str = "bin"
    compressed: bool = False


class DumpDoc(BaseModel):
    model_config = ConfigDict(extra="ignore")

    dumpEntries: list[DumpInfo] = Field(default_factory=list)
    textTable: dict[int, str] = Field(default_factory=dict)
    staffTextTable: dict[int, str] = Field(default_factory=dict)
    flyoverTextTable: dict[int, str] = Field(default_factory=dict)
    flyoverLabels: dict[int, str] = Field(default_factory=dict)
    renameLabels: dict[str, dict[int, str]] = Field(default_factory=dict)
    compressedTextStrings: list[str] = Field(default_factory=list)
    defaultDumpPath: str = "bin"
    romIdentifier: str = ""
    dontUseTextTable: bool = False
    multibyteFlyovers: bool = False
    music: Music = Field(default_factory=Music)

    @property
    def supports_compressed_text(self) -> bool:
        return bool(self.compressedTextStrings)


class CommonData(BaseModel):
    model_config = ConfigDict(extra="ignore")

    eventFlags: list[str] = Field(default_factory=list)
    items: list[str] = Field(default_factory=list)
    movements: list[str] = Field(default_factory=list)
    musicTracks: list[str] = Field(default_factory=list)
    partyMembers: list[str] = Field(default_factory=list)
    sfx: list[str] = Field(default_factory=list)
    sprites: list[str] = Field(default_factory=list)
    statusGroups: list[str] = Field(default_factory=list)
    windows: list[str] = Field(default_factory=list)
    directions: list[str] = Field(default_factory=list)
    genders: list[str] = Field(default_factory=list)
    enemyTypes: list[str] = Field(default_factory=list)
    itemFlags: list[str] = Field(default_factory=list)
    enemyGroups: list[str] = Field(default_factory=list)


def load_dump_doc(path: Path) -> DumpDoc:
    """Load a DumpDoc from a YAML file."""
    with path.open() as f:
        raw = yaml.safe_load(f)

    return DumpDoc.model_validate(raw)


def load_common_data(path: Path) -> CommonData:
    """Load CommonData from a YAML file."""
    with path.open() as f:
        raw = yaml.safe_load(f)

    return CommonData.model_validate(raw)
