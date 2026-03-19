"""Event script text decoder with all control codes.

Ported from textdump.d (1431 lines).
"""

from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path

from ebtools.config import CommonData, DumpDoc


@dataclass
class TextFile:
    start: int  # ROM address (offset + 0xC00000)
    length: int


def parse_text_data(
    dir: Path,
    base_name: str,
    extension: str,
    source: bytes,
    offset: int,
    doc: DumpDoc,
    common_data: CommonData,
) -> list[str]:
    """Parse event script text data.

    Outputs:
    - .ebtxt (main text with compressed macros for US, raw hex for JP)
    - .ebtxt.uncompressed (expanded compressed text, US only)
    - .symbols.asm (label declarations)
    """
    jp_text = doc.dontUseTextTable

    # Build map of all ebtxt entries for cross-file label resolution
    text_files: dict[str, TextFile] = {}
    for entry in doc.dumpEntries:
        if entry.extension == "ebtxt":
            text_files[entry.name] = TextFile(
                start=entry.offset + 0xC00000,
                length=entry.size,
            )

    # Output files
    filename = f"{base_name}.ebtxt"
    uncompressed_filename = f"{base_name}.ebtxt.uncompressed"
    symbol_filename = f"{base_name}.symbols.asm"

    stack = ExitStack()
    out_file = stack.enter_context((dir / filename).open("w"))
    out_file_c = None
    if not jp_text:
        out_file_c = stack.enter_context((dir / uncompressed_filename).open("w"))
    symbol_file = stack.enter_context((dir / symbol_filename).open("w"))

    # State
    pos = 0
    current_offset = offset
    label_printed = False
    raw: list[int] = []
    tmpbuff = ""
    tmpcompbuff = ""

    def write_formatted(text: str) -> None:
        out_file.write(text + "\n")
        if out_file_c is not None:
            out_file_c.write(text + "\n")

    def write_line(text: str = "") -> None:
        out_file.write(text + "\n")
        if out_file_c is not None:
            out_file_c.write(text + "\n")

    def label(addr: int, throw_on_undefined: bool) -> str:
        if addr == 0:
            return "NULL"
        for name, tf in text_files.items():
            if tf.start <= addr < tf.start + tf.length:
                rename_map = doc.renameLabels.get(name)
                if rename_map is not None:
                    found = rename_map.get(addr - tf.start)
                    if found is not None:
                        return found
                if throw_on_undefined:
                    raise Exception(f"No label found for {name}/{addr - tf.start:04X}")
                else:
                    return ""
        raise Exception("No matching files")

    def next_byte() -> int:
        nonlocal pos, current_offset, label_printed
        label_printed = False
        val = source[pos]
        pos += 1
        current_offset += 1
        return val

    def next_le16() -> int:
        return next_byte() | (next_byte() << 8)

    def next_le24() -> int:
        return next_byte() | (next_byte() << 8) | (next_byte() << 16)

    def next_le32() -> int:
        return next_byte() | (next_byte() << 8) | (next_byte() << 16) | (next_byte() << 24)

    def flush_buff() -> None:
        nonlocal tmpbuff, raw
        if not tmpbuff:
            return
        if jp_text:
            hex_str = "".join(f"\\x{b:02X}" for b in raw)
            out_file.write(f'\t.BYTE "{hex_str}" ;"{tmpbuff}"\n')
        else:
            out_file.write(f'\tEBTEXT "{tmpbuff}"\n')
        raw = []
        tmpbuff = ""

    def flush_compressed_buff() -> None:
        nonlocal tmpcompbuff
        if not tmpcompbuff or jp_text:
            return
        out_file_c.write(f'\tEBTEXT "{tmpcompbuff}"\n')
        tmpcompbuff = ""

    def flush_buffs() -> None:
        flush_buff()
        if not jp_text:
            flush_compressed_buff()

    def print_label(throw_on_undefined: bool) -> None:
        nonlocal label_printed
        if label_printed or pos >= len(source):
            return
        labelstr = label(current_offset, throw_on_undefined)
        if labelstr == "":
            return
        flush_buffs()
        symbol_file.write(f".GLOBAL {labelstr}: far\n")
        write_line()
        write_formatted(f"{labelstr}: ;${current_offset:06X}")
        label_printed = True

    def event_flag_name(flag: int) -> str:
        if flag >= 0x400:
            return f"OVERFLOW{flag:03X}"
        return common_data.eventFlags[flag]

    # --- Sub-control-code parsers (closures over shared state) ---

    def parse_cc18() -> None:
        sub_cc = next_byte()
        if sub_cc == 0x00:
            write_line("\tEBTEXT_CLOSE_WINDOW")
        elif sub_cc == 0x01:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_OPEN_WINDOW WINDOW::{common_data.windows[arg]}")
        elif sub_cc == 0x02:
            write_line("\tEBTEXT_SAVE_WINDOW_TEXT_ATTRIBUTES")
        elif sub_cc == 0x03:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_SWITCH_TO_WINDOW ${arg:02X}")
        elif sub_cc == 0x04:
            write_line("\tEBTEXT_CLOSE_ALL_WINDOWS")
        elif sub_cc == 0x05:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_FORCE_TEXT_ALIGNMENT ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x06:
            write_line("\tEBTEXT_CLEAR_WINDOW")
        elif sub_cc == 0x07:
            arg = next_le32()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_CHECK_FOR_INEQUALITY ${arg:06X}, ${arg2:02X}")
        elif sub_cc == 0x08:
            arg = next_le24()
            write_formatted(f"\tEBTEXT_SELECTION_MENU_NO_CANCEL ${arg:06X}")
        elif sub_cc == 0x09:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_SELECTION_MENU_ALLOW_CANCEL ${arg:02X}")
        elif sub_cc == 0x0A:
            write_line("\tEBTEXT_SHOW_WALLET_WINDOW")
        else:
            write_formatted(f"UNHANDLED: 18 {sub_cc:02X}")

    def parse_cc19() -> None:
        sub_cc = next_byte()
        if sub_cc == 0x02:
            payload = ""
            jp_text_buffer = ""
            while True:
                x = next_byte()
                if x == 0:
                    break
                if x == 1:
                    dest = label(next_le32(), True)
                    if jp_text:
                        write_formatted(
                            f'\tEBTEXT_LOAD_STRING_TO_MEMORY_WITH_SELECT_SCRIPT "{payload}", {dest} ; "{jp_text_buffer}"'
                        )
                    else:
                        write_formatted(f'\tEBTEXT_LOAD_STRING_TO_MEMORY_WITH_SELECT_SCRIPT "{payload}", {dest}')
                    break
                elif x == 2:
                    if jp_text:
                        write_formatted(f'\tEBTEXT_LOAD_STRING_TO_MEMORY "{payload}" ; "{jp_text_buffer}"')
                    else:
                        write_formatted(f'\tEBTEXT_LOAD_STRING_TO_MEMORY "{payload}"')
                    break
                else:
                    if jp_text:
                        payload += f"\\x{x:02X}"
                        jp_text_buffer += doc.textTable[x]
                    else:
                        payload += doc.textTable[x]
        elif sub_cc == 0x04:
            write_line("\tEBTEXT_CLEAR_LOADED_STRINGS")
        elif sub_cc == 0x05:
            arg = next_byte()
            status_group = next_byte()
            status = next_byte()
            write_formatted(
                f"\tEBTEXT_INFLICT_STATUS PARTY_MEMBER_TEXT::{common_data.partyMembers[arg + 1]}, ${status_group:02X}, ${status:02X}"
            )
        elif sub_cc == 0x10:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_CHARACTER_NUMBER ${arg:02X}")
        elif sub_cc == 0x11:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_CHARACTER_NAME_LETTER ${arg:02X}")
        elif sub_cc == 0x14:
            write_line("\tEBTEXT_GET_ESCARGO_EXPRESS_ITEM")
        elif sub_cc == 0x16:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_GET_CHARACTER_STATUS ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x18:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_EXP_FOR_NEXT_LEVEL ${arg:02X}")
        elif sub_cc == 0x19:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_ADD_ITEM_ID_TO_WORK_MEMORY ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x1A:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_ESCARGO_EXPRESS_ITEM_BY_SLOT ${arg:02X}")
        elif sub_cc == 0x1B:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_WINDOW_MENU_OPTION_COUNT ${arg:02X}")
        elif sub_cc == 0x1C:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_TRANSFER_ITEM_TO_QUEUE ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x1D:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_GET_QUEUED_ITEM_DATA ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x1E:
            write_line("\tEBTEXT_GET_CURRENT_NUMBER")
        elif sub_cc == 0x1F:
            write_line("\tEBTEXT_GET_CURRENT_INVENTORY_ITEM")
        elif sub_cc == 0x20:
            write_line("\tEBTEXT_GET_PLAYER_CONTROLLED_PARTY_COUNT")
        elif sub_cc == 0x21:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_IS_ITEM_DRINK ${arg:02X}")
        elif sub_cc == 0x22:
            arg = next_byte()
            arg2 = next_byte()
            arg3 = next_le16()
            write_formatted(f"\tEBTEXT_GET_DIRECTION_OF_OBJECT_FROM_CHARACTER ${arg:02X}, ${arg2:02X}, ${arg3:04X}")
        elif sub_cc == 0x23:
            arg = next_le16()
            arg2 = next_le16()
            arg3 = next_byte()
            write_formatted(f"\tEBTEXT_GET_DIRECTION_OF_OBJECT_FROM_NPC ${arg:04X}, ${arg2:04X}, ${arg3:02X}")
        elif sub_cc == 0x24:
            arg = next_le16()
            arg2 = next_le16()
            write_formatted(f"\tEBTEXT_GET_DIRECTION_OF_OBJECT_FROM_SPRITE ${arg:04X}, ${arg2:04X}")
        elif sub_cc == 0x25:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_IS_ITEM_CONDIMENT ${arg:02X}")
        elif sub_cc == 0x26:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_SET_RESPAWN_POINT ${arg:02X}")
        elif sub_cc == 0x27:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_RESOLVE_CC_TABLE_DATA ${arg:02X}")
        elif sub_cc == 0x28:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_LETTER_FROM_STAT ${arg:02X}")
        else:
            write_formatted(f"UNHANDLED: 19 {sub_cc:02X}")

    def parse_cc1a() -> None:
        sub_cc = next_byte()
        if sub_cc == 0x01:
            dest = next_le32()
            dest2 = next_le32()
            dest3 = next_le32()
            dest4 = next_le32()
            arg5 = next_byte()
            write_formatted(
                f"\tEBTEXT_PARTY_MEMBER_SELECTION_MENU_UNCANCELLABLE {label(dest, True)}, {label(dest2, True)}, {label(dest3, True)}, {label(dest4, True)}, ${arg5:02X}"
            )
        elif sub_cc == 0x05:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_SHOW_CHARACTER_INVENTORY ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x06:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_DISPLAY_SHOP_MENU ${arg:02X}")
        elif sub_cc == 0x07:
            write_line("\tEBTEXT_SELECT_ESCARGO_EXPRESS_ITEM")
        elif sub_cc == 0x0A:
            write_line("\tEBTEXT_OPEN_PHONE_MENU")
        else:
            write_formatted(f"UNHANDLED: 1A {sub_cc:02X}")

    def parse_cc1b() -> None:
        sub_cc = next_byte()
        if sub_cc == 0x00:
            write_line("\tEBTEXT_COPY_ACTIVE_MEMORY_TO_STORAGE")
        elif sub_cc == 0x01:
            write_line("\tEBTEXT_COPY_STORAGE_MEMORY_TO_ACTIVE")
        elif sub_cc == 0x02:
            dest = next_le32()
            write_formatted(f"\tEBTEXT_JUMP_IF_FALSE {label(dest, True)}")
        elif sub_cc == 0x03:
            dest = next_le32()
            write_formatted(f"\tEBTEXT_JUMP_IF_TRUE {label(dest, True)}")
        elif sub_cc == 0x04:
            write_line("\tEBTEXT_SWAP_WORKING_AND_ARG_MEMORY")
        elif sub_cc == 0x05:
            write_line("\tEBTEXT_COPY_ACTIVE_MEMORY_TO_WORKING_MEMORY")
        elif sub_cc == 0x06:
            write_line("\tEBTEXT_COPY_WORKING_MEMORY_TO_ACTIVE_MEMORY")
        else:
            write_formatted(f"UNHANDLED: 1B {sub_cc:02X}")

    def parse_cc1c() -> None:
        sub_cc = next_byte()
        if sub_cc == 0x00:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_TEXT_COLOUR_EFFECTS ${arg:02X}")
        elif sub_cc == 0x01:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_PRINT_STAT ${arg:02X}")
        elif sub_cc == 0x02:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_PRINT_CHAR_NAME ${arg:02X}")
        elif sub_cc == 0x03:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_PRINT_CHAR ${arg:02X}")
        elif sub_cc == 0x04:
            write_line("\tEBTEXT_OPEN_HP_PP_WINDOWS")
        elif sub_cc == 0x05:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_PRINT_ITEM_NAME ITEM::{common_data.items[arg]}")
        elif sub_cc == 0x06:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_PRINT_TELEPORT_DESTINATION_NAME ${arg:02X}")
        elif sub_cc == 0x07:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_PRINT_HORIZONTAL_TEXT_STRING ${arg:02X}")
        elif sub_cc == 0x08:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_PRINT_SPECIAL_GFX ${arg:02X}")
        elif sub_cc == 0x09:
            write_line("\tEBTEXT_SET_NUMBER_PADDING")
        elif sub_cc == 0x0A:
            arg = next_le32()
            write_formatted(f"\tEBTEXT_PRINT_NUMBER ${arg:08X}")
        elif sub_cc == 0x0B:
            arg = next_le32()
            write_formatted(f"\tEBTEXT_PRINT_MONEY_AMOUNT ${arg:08X}")
        elif sub_cc == 0x0C:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_PRINT_VERTICAL_TEXT_STRING ${arg:02X}")
        elif sub_cc == 0x0D:
            write_line("\tEBTEXT_PRINT_ACTION_USER_NAME")
        elif sub_cc == 0x0E:
            write_line("\tEBTEXT_PRINT_ACTION_TARGET_NAME")
        elif sub_cc == 0x0F:
            write_line("\tEBTEXT_PRINT_ACTION_AMOUNT")
        elif sub_cc == 0x11:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_HINT_NEW_LINE ${arg:02X}")
        elif sub_cc == 0x12:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_PRINT_PSI_NAME ${arg:02X}")
        elif sub_cc == 0x13:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_DISPLAY_PSI_ANIMATION ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x14:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_LOAD_SPECIAL ${arg:02X}")
        elif sub_cc == 0x15:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_LOAD_SPECIAL_FOR_JUMP_MULTI ${arg:02X}")
        else:
            write_formatted(f"UNHANDLED: 1C {sub_cc:02X}")

    def parse_cc1d() -> None:
        sub_cc = next_byte()
        if sub_cc == 0x00:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_GIVE_ITEM_TO_CHARACTER ${arg:02X}, ITEM::{common_data.items[arg2]}")
        elif sub_cc == 0x01:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_TAKE_ITEM_FROM_CHARACTER ${arg:02X}, ITEM::{common_data.items[arg2]}")
        elif sub_cc == 0x02:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_PLAYER_HAS_INVENTORY_FULL ${arg:02X}")
        elif sub_cc == 0x03:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_PLAYER_HAS_INVENTORY_ROOM ${arg:02X}")
        elif sub_cc == 0x04:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_CHECK_IF_CHARACTER_DOESNT_HAVE_ITEM ${arg:02X}, ITEM::{common_data.items[arg2]}")
        elif sub_cc == 0x05:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_CHECK_IF_CHARACTER_HAS_ITEM ${arg:02X}, ITEM::{common_data.items[arg2]}")
        elif sub_cc == 0x06:
            arg = next_le32()
            write_formatted(f"\tEBTEXT_ADD_TO_ATM ${arg:08X}")
        elif sub_cc == 0x07:
            arg = next_le32()
            write_formatted(f"\tEBTEXT_TAKE_FROM_ATM ${arg:08X}")
        elif sub_cc == 0x08:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_ADD_TO_WALLET ${arg:04X}")
        elif sub_cc == 0x09:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_TAKE_FROM_WALLET ${arg:04X}")
        elif sub_cc == 0x0A:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_BUY_PRICE_OF_ITEM ITEM::{common_data.items[arg]}")
        elif sub_cc == 0x0B:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_SELL_PRICE_OF_ITEM ITEM::{common_data.items[arg]}")
        elif sub_cc == 0x0C:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_ESCARGO_EXPRESS_ITEM_STATUS ${arg:04X}")
        elif sub_cc == 0x0D:
            who = next_byte()
            what = next_byte()
            what2 = next_byte()
            write_formatted(
                f"\tEBTEXT_CHARACTER_HAS_AILMENT ${who:02X}, STATUS_GROUP::{common_data.statusGroups[what - 1]}, ${what2:02X}"
            )
        elif sub_cc == 0x0E:
            who = next_byte()
            what = next_byte()
            write_formatted(f"\tEBTEXT_GIVE_ITEM_TO_CHARACTER_B ${who:02X}, ITEM::{common_data.items[what]}")
        elif sub_cc == 0x0F:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_TAKE_ITEM_FROM_CHARACTER_2 ${arg:04X}")
        elif sub_cc == 0x10:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_CHECK_ITEM_EQUIPPED ${arg:04X}")
        elif sub_cc == 0x11:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_CHECK_ITEM_USABLE_BY_SLOT ${arg:04X}")
        elif sub_cc == 0x12:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_ESCARGO_EXPRESS_MOVE ${arg:04X}")
        elif sub_cc == 0x13:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_DELIVER_ESCARGO_EXPRESS_ITEM ${arg:04X}")
        elif sub_cc == 0x14:
            dest = next_le32()
            write_formatted(f"\tEBTEXT_HAVE_ENOUGH_MONEY ${dest:08X}")
        elif sub_cc == 0x15:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_PUT_VAL_IN_ARGMEM ${arg:02X}")
        elif sub_cc == 0x17:
            dest = next_le32()
            write_formatted(f"\tEBTEXT_HAVE_ENOUGH_MONEY_IN_ATM ${dest:08X}")
        elif sub_cc == 0x18:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_ESCARGO_EXPRESS_STORE ${arg:02X}")
        elif sub_cc == 0x19:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_HAVE_X_PARTY_MEMBERS ${arg:02X}")
        elif sub_cc == 0x20:
            write_line("\tEBTEXT_TEST_IS_USER_TARGETTING_SELF")
        elif sub_cc == 0x21:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GENERATE_RANDOM_NUMBER ${arg:02X}")
        elif sub_cc == 0x22:
            write_line("\tEBTEXT_TEST_IF_EXIT_MOUSE_USABLE")
        elif sub_cc == 0x23:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_ITEM_CATEGORY ${arg:02X}")
        elif sub_cc == 0x24:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_GET_GAME_STATE_C4 ${arg:02X}")
        else:
            write_formatted(f"UNHANDLED: 1D {sub_cc:02X}")

    def parse_cc1e() -> None:
        sub_cc = next_byte()
        if sub_cc == 0x00:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_RECOVER_HP_PERCENT ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x01:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_DEPLETE_HP_PERCENT ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x02:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_RECOVER_HP_PERCENT ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x03:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_DEPLETE_HP_AMOUNT ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x04:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_RECOVER_PP_PERCENT ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x05:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_DEPLETE_PP_PERCENT ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x06:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_RECOVER_PP_PERCENT ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x07:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_DEPLETE_PP_AMOUNT ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x08:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_SET_CHARACTER_LEVEL ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x09:
            arg = next_byte()
            arg2 = next_le24()
            write_formatted(f"\tEBTEXT_GIVE_EXPERIENCE ${arg:02X}, ${arg2:06X}")
        elif sub_cc == 0x0A:
            arg = next_byte()
            arg2 = next_le16()
            write_formatted(f"\tEBTEXT_BOOST_IQ ${arg:02X}, ${arg2:04X}")
        elif sub_cc == 0x0B:
            arg = next_byte()
            arg2 = next_le16()
            write_formatted(f"\tEBTEXT_BOOST_GUTS ${arg:02X}, ${arg2:04X}")
        elif sub_cc == 0x0C:
            arg = next_byte()
            arg2 = next_le16()
            write_formatted(f"\tEBTEXT_BOOST_SPEED ${arg:02X}, ${arg2:04X}")
        elif sub_cc == 0x0D:
            arg = next_byte()
            arg2 = next_le16()
            write_formatted(f"\tEBTEXT_BOOST_VITALITY ${arg:02X}, ${arg2:04X}")
        elif sub_cc == 0x0E:
            arg = next_byte()
            arg2 = next_le16()
            write_formatted(f"\tEBTEXT_BOOST_LUCK ${arg:02X}, ${arg2:04X}")
        else:
            write_formatted(f"UNHANDLED: 1E {sub_cc:02X}")

    def parse_cc1f() -> None:
        sub_cc = next_byte()
        if sub_cc == 0x00:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_PLAY_MUSIC ${arg:02X}, MUSIC::{common_data.musicTracks[arg2]}")
        elif sub_cc == 0x01:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_STOP_MUSIC ${arg:02X}")
        elif sub_cc == 0x02:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_PLAY_SOUND SFX::{common_data.sfx[arg]}")
        elif sub_cc == 0x03:
            write_line("\tEBTEXT_RESTORE_DEFAULT_MUSIC")
        elif sub_cc == 0x04:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_SET_TEXT_PRINTING_SOUND ${arg:02X}")
        elif sub_cc == 0x05:
            write_line("\tEBTEXT_DISABLE_SECTOR_MUSIC_CHANGE")
        elif sub_cc == 0x06:
            write_line("\tEBTEXT_ENABLE_SECTOR_MUSIC_CHANGE")
        elif sub_cc == 0x07:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_APPLY_MUSIC_EFFECT ${arg:02X}")
        elif sub_cc == 0x11:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_ADD_PARTY_MEMBER PARTY_MEMBER::{common_data.partyMembers[arg]}")
        elif sub_cc == 0x12:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_REMOVE_PARTY_MEMBER PARTY_MEMBER::{common_data.partyMembers[arg]}")
        elif sub_cc == 0x13:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_CHANGE_CHARACTER_DIRECTION ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x14:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_CHANGE_PARTY_DIRECTION ${arg:02X}")
        elif sub_cc == 0x15:
            arg = next_le16()
            arg2 = next_le16()
            arg3 = next_byte()
            write_formatted(
                f"\tEBTEXT_GENERATE_ACTIVE_SPRITE OVERWORLD_SPRITE::{common_data.sprites[arg]}, EVENT_SCRIPT::{common_data.movements[arg2]}, ${arg3:02X}"
            )
        elif sub_cc == 0x16:
            arg = next_le16()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_CHANGE_TPT_ENTRY_DIRECTION ${arg:04X}, ${arg2:02X}")
        elif sub_cc == 0x17:
            arg = next_le16()
            arg2 = next_le16()
            arg3 = next_byte()
            write_formatted(
                f"\tEBTEXT_CREATE_ENTITY ${arg:04X}, EVENT_SCRIPT::{common_data.movements[arg2]}, ${arg3:02X}"
            )
        elif sub_cc == 0x1A:
            arg = next_le16()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_CREATE_FLOATING_SPRITE_NEAR_TPT_ENTRY ${arg:04X}, ${arg2:02X}")
        elif sub_cc == 0x1B:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_DELETE_FLOATING_SPRITE_NEAR_TPT_ENTRY ${arg:04X}")
        elif sub_cc == 0x1C:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_CREATE_FLOATING_SPRITE_NEAR_CHARACTER ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x1D:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_DELETE_FLOATING_SPRITE_NEAR_CHARACTER ${arg:02X}")
        elif sub_cc == 0x1E:
            arg = next_le16()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_DELETE_TPT_INSTANCE ${arg:04X}, ${arg2:02X}")
        elif sub_cc == 0x1F:
            arg = next_le16()
            arg2 = next_byte()
            write_formatted(
                f"\tEBTEXT_DELETE_GENERATED_SPRITE OVERWORLD_SPRITE::{common_data.sprites[arg]}, ${arg2:02X}"
            )
        elif sub_cc == 0x20:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_TRIGGER_PSI_TELEPORT ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x21:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_TELEPORT_TO ${arg:02X}")
        elif sub_cc == 0x23:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_TRIGGER_BATTLE ENEMY_GROUP::{common_data.enemyGroups[arg]}")
        elif sub_cc == 0x30:
            write_line("\tEBTEXT_USE_NORMAL_FONT")
        elif sub_cc == 0x31:
            write_line("\tEBTEXT_USE_MR_SATURN_FONT")
        elif sub_cc == 0x41:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_TRIGGER_EVENT ${arg:02X}")
        elif sub_cc == 0x50:
            write_line("\tEBTEXT_DISABLE_CONTROLLER_INPUT")
        elif sub_cc == 0x51:
            write_line("\tEBTEXT_ENABLE_CONTROLLER_INPUT")
        elif sub_cc == 0x52:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_CREATE_NUMBER_SELECTOR ${arg:02X}")
        elif sub_cc == 0x60:
            write_line("\tEBTEXT_TEXT_SPEED_DELAY")
        elif sub_cc == 0x61:
            write_line("\tEBTEXT_TRIGGER_MOVEMENT_CODE")
        elif sub_cc == 0x62:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_ENABLE_BLINKING_TRIANGLE ${arg:02X}")
        elif sub_cc == 0x63:
            arg = next_le32()
            write_formatted(f"\tEBTEXT_SCREEN_RELOAD_PTR {label(arg, True)}")
        elif sub_cc == 0x64:
            write_line("\tEBTEXT_DELETE_ALL_NPCS")
        elif sub_cc == 0x65:
            write_line("\tEBTEXT_DELETE_FIRST_NPC")
        elif sub_cc == 0x66:
            arg = next_byte()
            arg2 = next_byte()
            arg3 = next_le32()
            write_formatted(f"\tEBTEXT_ACTIVATE_HOTSPOT ${arg:02X}, ${arg2:02X}, {label(arg3, True)}")
        elif sub_cc == 0x67:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_DEACTIVATE_HOTSPOT ${arg:02X}")
        elif sub_cc == 0x68:
            write_line("\tEBTEXT_STORE_COORDINATES_TO_MEMORY")
        elif sub_cc == 0x69:
            write_line("\tEBTEXT_TELEPORT_TO_STORED_COORDINATES")
        elif sub_cc == 0x71:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_REALIZE_PSI ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x83:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_EQUIP_ITEM_TO_CHARACTER ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0x90:
            # Missing from D source but present in SubCC1F enum
            write_formatted("UNHANDLED: 1F 90")
        elif sub_cc == 0xA0:
            write_line("\tEBTEXT_SET_TPT_DIRECTION_UP")
        elif sub_cc == 0xA1:
            write_line("\tEBTEXT_SET_TPT_DIRECTION_DOWN")
        elif sub_cc == 0xA2:
            write_line("\tEBTEXT_GET_INTERACTING_EVENT_FLAG")
        elif sub_cc == 0xB0:
            write_line("\tEBTEXT_SAVE_GAME")
        elif sub_cc == 0xC0:
            flush_buffs()
            arg_count = next_byte()
            dests = []
            for _ in range(arg_count):
                dests.append(label(next_le32(), True))
            write_formatted(f"\tEBTEXT_JUMP_MULTI2 {', '.join(dests)}")
        elif sub_cc == 0xD0:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_TRY_FIX_ITEM {arg}")
        elif sub_cc == 0xD1:
            write_line("\tEBTEXT_GET_DIRECTION_OF_NEARBY_TRUFFLE")
        elif sub_cc == 0xD2:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_SUMMON_WANDERING_PHOTOGRAPHER ${arg:02X}")
        elif sub_cc == 0xD3:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_TRIGGER_TIMED_EVENT ${arg:02X}")
        elif sub_cc == 0xE1:
            arg = next_le16()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_CHANGE_MAP_PALETTE ${arg:04X}, ${arg2:02X}")
        elif sub_cc == 0xE4:
            arg = next_le16()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_CHANGE_GENERATED_SPRITE_DIRECTION ${arg:04X}, ${arg2:02X}")
        elif sub_cc == 0xE5:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_SET_PLAYER_LOCK ${arg:02X}")
        elif sub_cc == 0xE6:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_DELAY_TPT_APPEARANCE ${arg:04X}")
        elif sub_cc == 0xE7:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_DISABLE_SPRITE_MOVEMENT ${arg:04X}")
        elif sub_cc == 0xE8:
            arg = next_byte()
            write_formatted(f"\tEBTEXT_RESTRICT_PLAYER_MOVEMENT_WHEN_CAMERA_REPOSITIONED ${arg:02X}")
        elif sub_cc == 0xE9:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_ENABLE_NPC_MOVEMENT ${arg:04X}")
        elif sub_cc == 0xEA:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_ENABLE_SPRITE_MOVEMENT ${arg:04X}")
        elif sub_cc == 0xEB:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_MAKE_INVISIBLE ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0xEC:
            arg = next_byte()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_MAKE_VISIBLE ${arg:02X}, ${arg2:02X}")
        elif sub_cc == 0xED:
            write_line("\tEBTEXT_RESTORE_MOVEMENT")
        elif sub_cc == 0xEE:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_WARP_PARTY_TO_TPT_ENTRY ${arg:04X}")
        elif sub_cc == 0xEF:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_SET_CAMERA_FOCUS_BY_SPRITE_ID ${arg:04X}")
        elif sub_cc == 0xF0:
            write_line("\tEBTEXT_RIDE_BICYCLE")
        elif sub_cc == 0xF1:
            arg = next_le16()
            arg2 = next_le16()
            write_formatted(f"\tEBTEXT_SET_TPT_MOVEMENT_CODE ${arg:04X}, EVENT_SCRIPT::{common_data.movements[arg2]}")
        elif sub_cc == 0xF2:
            arg = next_le16()
            arg2 = next_le16()
            write_formatted(
                f"\tEBTEXT_SET_SPRITE_MOVEMENT_CODE OVERWORLD_SPRITE::{common_data.sprites[arg]}, EVENT_SCRIPT::{common_data.movements[arg2]}"
            )
        elif sub_cc == 0xF3:
            arg = next_le16()
            arg2 = next_byte()
            write_formatted(f"\tEBTEXT_CREATE_FLOATING_SPRITE_NEAR_ENTITY ${arg:04X}, ${arg2:02X}")
        elif sub_cc == 0xF4:
            arg = next_le16()
            write_formatted(f"\tEBTEXT_DELETE_FLOATING_SPRITE_NEAR_ENTITY ${arg:04X}")
        else:
            write_formatted(f"UNHANDLED: 1F {sub_cc:02X}")

    # Write include header
    base = Path(base_name).name
    write_formatted(f'.INCLUDE "{base}.symbols.asm"\n')

    # Print initial label
    print_label(True)

    # Main parsing loop
    while pos < len(source):
        # Check if current offset has a rename label
        for name, tf in text_files.items():
            if tf.start <= current_offset < tf.start + tf.length:
                rename_map = doc.renameLabels.get(name)
                if rename_map is not None and (current_offset - tf.start) in rename_map:
                    print_label(True)
                    break

        first = next_byte()

        # Check text table
        if first in doc.textTable:
            raw.append(first)
            tmpbuff += doc.textTable[first]
            tmpcompbuff += doc.textTable[first]
            continue

        # Control codes
        if first == 0x00:
            flush_buffs()
            write_line("\tEBTEXT_LINE_BREAK")
        elif first == 0x01:
            flush_buffs()
            write_line("\tEBTEXT_START_NEW_LINE")
        elif first == 0x02:
            flush_buffs()
            write_line("\tEBTEXT_END_BLOCK")
            print_label(False)
        elif first == 0x03:
            flush_buffs()
            write_line("\tEBTEXT_HALT_WITH_PROMPT")
        elif first == 0x04:
            flush_buffs()
            flag = next_le16()
            write_formatted(f"\tEBTEXT_SET_EVENT_FLAG EVENT_FLAG::{event_flag_name(flag)}")
        elif first == 0x05:
            flush_buffs()
            flag = next_le16()
            write_formatted(f"\tEBTEXT_CLEAR_EVENT_FLAG EVENT_FLAG::{event_flag_name(flag)}")
        elif first == 0x06:
            flush_buffs()
            flag = next_le16()
            dest = next_le32()
            write_formatted(f"\tEBTEXT_JUMP_IF_FLAG_SET {label(dest, True)}, EVENT_FLAG::{event_flag_name(flag)}")
        elif first == 0x07:
            flush_buffs()
            flag = next_le16()
            write_formatted(f"\tEBTEXT_CHECK_EVENT_FLAG EVENT_FLAG::{common_data.eventFlags[flag]}")
        elif first == 0x08:
            flush_buffs()
            dest = next_le32()
            write_formatted(f"\tEBTEXT_CALL_TEXT {label(dest, True)}")
        elif first == 0x09:
            flush_buffs()
            arg_count = next_byte()
            dests = []
            for _ in range(arg_count):
                dests.append(label(next_le32(), True))
            write_formatted(f"\tEBTEXT_JUMP_MULTI {', '.join(dests)}")
        elif first == 0x0A:
            flush_buffs()
            dest = next_le32()
            write_formatted(f"\tEBTEXT_JUMP {label(dest, True)}\n")
        elif first == 0x0B:
            flush_buffs()
            arg = next_byte()
            write_formatted(f"\tEBTEXT_TEST_IF_WORKMEM_TRUE ${arg:02X}")
        elif first == 0x0C:
            flush_buffs()
            arg = next_byte()
            write_formatted(f"\tEBTEXT_TEST_IF_WORKMEM_FALSE ${arg:02X}")
        elif first == 0x0D:
            flush_buffs()
            dest = next_byte()
            write_formatted(f"\tEBTEXT_COPY_TO_ARGMEM ${dest:02X}")
        elif first == 0x0E:
            flush_buffs()
            dest = next_byte()
            write_formatted(f"\tEBTEXT_STORE_TO_ARGMEM ${dest:02X}")
        elif first == 0x0F:
            flush_buffs()
            write_line("\tEBTEXT_INCREMENT_WORKMEM")
        elif first == 0x10:
            flush_buffs()
            time = next_byte()
            write_formatted(f"\tEBTEXT_PAUSE {time}")
        elif first == 0x11:
            flush_buffs()
            write_line("\tEBTEXT_CREATE_SELECTION_MENU")
        elif first == 0x12:
            flush_buffs()
            write_line("\tEBTEXT_CLEAR_TEXT_LINE")
        elif first == 0x13:
            flush_buffs()
            write_line("\tEBTEXT_HALT_WITHOUT_PROMPT")
        elif first == 0x14:
            flush_buffs()
            write_line("\tEBTEXT_HALT_WITH_PROMPT_ALWAYS")
        elif 0x15 <= first <= 0x17:
            flush_buff()
            if doc.supports_compressed_text:
                arg = next_byte()
                id_ = ((first - 0x15) << 8) + arg
                text_str = doc.compressedTextStrings[id_]
                out_file.write(f'\tEBTEXT_COMPRESSED_BANK_{first - 0x14} ${arg:02X} ;"{text_str}"\n')
                tmpcompbuff += text_str
            else:
                write_formatted(f"UNHANDLED: {first:02X}")
        elif first == 0x18:
            flush_buffs()
            parse_cc18()
        elif first == 0x19:
            flush_buffs()
            parse_cc19()
        elif first == 0x1A:
            flush_buffs()
            parse_cc1a()
        elif first == 0x1B:
            flush_buffs()
            parse_cc1b()
        elif first == 0x1C:
            flush_buffs()
            parse_cc1c()
        elif first == 0x1D:
            flush_buffs()
            parse_cc1d()
        elif first == 0x1E:
            flush_buffs()
            parse_cc1e()
        elif first == 0x1F:
            flush_buffs()
            parse_cc1f()
        else:
            flush_buffs()
            write_formatted(f"\t.BYTE ${first:02X}")

    stack.close()

    if jp_text:
        return [filename, symbol_filename]
    else:
        return [filename, uncompressed_filename, symbol_filename]
