"""String table builder for inline text fields.

Collects encoded strings, deduplicates them, and produces a single
concatenated binary blob.  Each string's byte offset within the blob
is returned by ``add()`` so that packers can store it as a pointer.
"""

from ebtools.text_dsl.inline import encode_inline_text


class StringTableBuilder:
    """Accumulate encoded inline text strings into a deduplicated blob."""

    def __init__(self, reverse_text_table: dict[str, int]) -> None:
        self._reverse_text_table = reverse_text_table
        self._strings: dict[str, int] = {}  # text -> byte offset
        self._buf = bytearray()

    def add(self, text: str) -> int:
        """Add a string, return its byte offset.  Deduplicates."""
        if text in self._strings:
            return self._strings[text]

        offset = len(self._buf)
        encoded = encode_inline_text(text, self._reverse_text_table)
        self._buf.extend(encoded)
        self._strings[text] = offset
        return offset

    def build(self) -> bytes:
        """Return the complete string table as bytes."""
        return bytes(self._buf)
