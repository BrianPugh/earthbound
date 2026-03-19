"""HALLZ2 compressor using all command types."""

from ebtools.hallz.decompress import _BIT_REVERSE


def _encode_header(cmd_id: int, length: int) -> bytes:
    """Encode a HALLZ2 command header (short or extended form)."""
    if length <= 32:
        return bytes([(cmd_id << 5) | (length - 1)])
    encoded = length - 1
    return bytes([0xE0 | (cmd_id << 2) | (encoded >> 8), encoded & 0xFF])


def _hash2(data: bytes, pos: int) -> int:
    """Simple 2-byte hash for match finder."""
    return ((data[pos] << 8) | data[pos + 1]) & 0xFFFF


class _MatchFinder:
    """Hash chain match finder for forward and backward copy search."""

    _HASH_SIZE = 1 << 16
    _MAX_CHAIN = 512

    def __init__(self, data: bytes):
        self.data = data
        self.n = len(data)
        # Forward hash chain: keyed on (data[pos], data[pos+1])
        self.head = [-1] * self._HASH_SIZE
        self.chain = [-1] * self.n
        # Backward hash chain: keyed on (data[pos], data[pos-1])
        self.bwd_head = [-1] * self._HASH_SIZE
        self.bwd_chain = [-1] * self.n

    def advance(self, pos: int) -> None:
        """Add position to hash chains (call for each position as we advance)."""
        if pos + 1 < self.n:
            h = _hash2(self.data, pos)
            self.chain[pos] = self.head[h]
            self.head[h] = pos
        if pos > 0:
            # Backward hash: key is (data[pos], data[pos-1]) — first 2 bytes
            # read when doing a backward copy from this offset
            h = ((self.data[pos] << 8) | self.data[pos - 1]) & 0xFFFF
            self.bwd_chain[pos] = self.bwd_head[h]
            self.bwd_head[h] = pos

    def find_longest(self, pos: int) -> tuple[int, int]:
        """Find longest forward match at pos using hash chains."""
        if pos + 1 >= self.n:
            return 0, 0

        data = self.data
        max_len = min(self.n - pos, 1024)
        best_len = 0
        best_off = 0

        h = _hash2(data, pos)
        off = self.head[h]
        chain_left = self._MAX_CHAIN

        while off >= 0 and chain_left > 0:
            if data[off] == data[pos]:
                length = 0
                while length < max_len and data[off + length] == data[pos + length]:
                    length += 1
                if length > best_len:
                    best_len = length
                    best_off = off
                    if best_len == max_len:
                        break
            off = self.chain[off]
            chain_left -= 1

        return best_len, best_off

    def find_longest_backward(self, pos: int) -> tuple[int, int]:
        """Find longest backward copy match using hash chains."""
        if pos + 1 >= self.n:
            return 0, 0

        data = self.data
        max_len = min(self.n - pos, 1024)
        best_len = 0
        best_off = 0

        # Hash on (data[pos], data[pos+1]) maps to backward reads starting
        # at offset where data[off]==data[pos] and data[off-1]==data[pos+1]
        h = _hash2(data, pos)
        off = self.bwd_head[h]
        chain_left = self._MAX_CHAIN

        while off >= 0 and chain_left > 0:
            if off < pos and data[off] == data[pos]:
                length = 0
                ref = off
                while length < max_len:
                    if data[ref] != data[pos + length]:
                        break
                    length += 1
                    if ref > 0:
                        ref -= 1
                if length > best_len:
                    best_len = length
                    best_off = off
                    if best_len == max_len:
                        break
            off = self.bwd_chain[off]
            chain_left -= 1

        return best_len, best_off


def compress(data: bytes) -> bytes:
    """HALLZ2 compressor using all command types.

    Uses a greedy algorithm that at each position evaluates all possible
    commands (byte fill, word fill, incrementing fill, forward/reversed/backward
    copy) and picks the one with the best compression savings. Falls back to
    literal encoding when no command saves bytes.
    """
    out = bytearray()
    n = len(data)
    if n == 0:
        out.append(0xFF)
        return bytes(out)

    mf = _MatchFinder(data)
    pos = 0
    literal_start = 0

    def flush_literals(start: int, end: int) -> None:
        """Emit literal (cmd 0) commands for data[start:end]."""
        p = start
        while p < end:
            chunk = min(end - p, 1024)
            out.extend(_encode_header(0, chunk))
            out.extend(data[p : p + chunk])
            p += chunk

    def count_byte_fill(p: int) -> int:
        val = data[p]
        limit = min(n, p + 1024)
        i = p + 1
        while i < limit and data[i] == val:
            i += 1
        return i - p

    def count_word_fill(p: int) -> int:
        if p + 3 >= n:
            return 0
        lo, hi = data[p], data[p + 1]
        limit = min(n - 1, p + 2048)
        count = 1
        i = p + 2
        while i < limit and data[i] == lo and data[i + 1] == hi:
            count += 1
            i += 2
        return count

    def count_inc_fill(p: int) -> int:
        val = data[p]
        limit = min(n, p + 1024)
        i = p + 1
        while i < limit and data[i] == ((val + (i - p)) & 0xFF):
            i += 1
        run = i - p
        return run if run > 1 else 0

    def find_reversed_match(p: int) -> tuple[int, int]:
        """Find longest bit-reversed match using hash chain lookup."""
        if p + 1 >= n:
            return 0, 0
        best_len = 0
        best_off = 0
        max_len = min(n - p, 1024)
        # Hash on bit-reversed bytes to find candidates
        rev0 = _BIT_REVERSE[data[p]]
        rev1 = _BIT_REVERSE[data[p + 1]]
        h = ((rev0 << 8) | rev1) & 0xFFFF
        off = mf.head[h]
        chain_left = 256
        while off >= 0 and chain_left > 0:
            if off + 1 < p:  # non-overlapping
                length = 0
                avail = min(max_len, p - off)
                while length < avail and _BIT_REVERSE[data[off + length]] == data[p + length]:
                    length += 1
                if length > best_len:
                    best_len = length
                    best_off = off
                    if best_len == max_len:
                        break
            off = mf.chain[off]
            chain_left -= 1
        return best_len, best_off

    def find_backward_match(p: int) -> tuple[int, int]:
        """Find longest backward copy match using hash chain."""
        return mf.find_longest_backward(p)

    while pos < n:
        best_savings = 0
        best_cmd = None  # (cmd_id, header_length, extra_bytes, consumed_input)

        # Byte fill (cmd 1)
        run = count_byte_fill(pos)
        if run >= 3:
            cost = 2 if run <= 32 else 3
            savings = run - cost
            if savings > best_savings:
                best_savings = savings
                best_cmd = (1, run, bytes([data[pos]]), run)

        # Incrementing fill (cmd 3)
        run = count_inc_fill(pos)
        if run >= 3:
            cost = 2 if run <= 32 else 3
            savings = run - cost
            if savings > best_savings:
                best_savings = savings
                best_cmd = (3, run, bytes([data[pos]]), run)

        # Word fill (cmd 2)
        wrun = count_word_fill(pos)
        if wrun >= 2:
            consumed = wrun * 2
            cost = 3 if wrun <= 32 else 4
            savings = consumed - cost
            if savings > best_savings:
                best_savings = savings
                best_cmd = (2, wrun, bytes([data[pos], data[pos + 1]]), consumed)

        # Forward copy (cmd 4) - using hash chain
        if pos > 0:
            mlen, moff = mf.find_longest(pos)
            if mlen >= 4:
                cost = 3 if mlen <= 32 else 4
                savings = mlen - cost
                if savings > best_savings:
                    best_savings = savings
                    best_cmd = (4, mlen, moff.to_bytes(2, "big"), mlen)

        # Only try reversed/backward if forward didn't find great savings
        if pos > 0 and best_savings < 8:
            # Bit-reversed copy (cmd 5)
            mlen, moff = find_reversed_match(pos)
            if mlen >= 4:
                cost = 3 if mlen <= 32 else 4
                savings = mlen - cost
                if savings > best_savings:
                    best_savings = savings
                    best_cmd = (5, mlen, moff.to_bytes(2, "big"), mlen)

            # Backward copy (cmd 6)
            mlen, moff = find_backward_match(pos)
            if mlen >= 4:
                cost = 3 if mlen <= 32 else 4
                savings = mlen - cost
                if savings > best_savings:
                    best_savings = savings
                    best_cmd = (6, mlen, moff.to_bytes(2, "big"), mlen)

        if best_cmd and best_savings > 0:
            # Flush any pending literals
            if pos > literal_start:
                flush_literals(literal_start, pos)
            # Emit the command
            cmd_id, cmd_len, extra, consumed = best_cmd
            out.extend(_encode_header(cmd_id, cmd_len))
            out.extend(extra)
            # Advance hash chain for all positions consumed
            for i in range(pos, pos + consumed):
                mf.advance(i)
            pos += consumed
            literal_start = pos
        else:
            mf.advance(pos)
            pos += 1
            if pos - literal_start >= 1024:
                flush_literals(literal_start, pos)
                literal_start = pos

    # Flush remaining literals
    if pos > literal_start:
        flush_literals(literal_start, pos)

    # Terminator
    out.append(0xFF)
    return bytes(out)
