"""Parser registry: extension string -> handler function."""

from ebtools.parsers.compressed_text import parse_compressed_text
from ebtools.parsers.distortion import parse_distortion
from ebtools.parsers.enemy import parse_enemy_config
from ebtools.parsers.flyover import parse_flyover
from ebtools.parsers.item import parse_item_config
from ebtools.parsers.movement import parse_movement
from ebtools.parsers.npc import parse_npc_config
from ebtools.parsers.nspc import parse_nspc
from ebtools.parsers.raw import write_raw
from ebtools.parsers.staff_text import parse_staff_text
from ebtools.parsers.text import parse_text_data

# Extension -> parser function.
# Parsers have signature:
#   (dir, base_name, extension, data, offset, doc, common_data) -> list[str]
# Except nspc which also takes full_rom.
PARSERS = {
    "ebtxt": parse_text_data,
    "npcconfig": parse_npc_config,
    "flyover": parse_flyover,
    "enemyconfig": parse_enemy_config,
    "itemconfig": parse_item_config,
    "distortion": parse_distortion,
    "movement": parse_movement,
    "ebctxt": parse_compressed_text,
    "stafftext": parse_staff_text,
}

# These parsers need the full ROM data as an extra argument.
FULL_ROM_PARSERS = {
    "nspc": parse_nspc,
}
