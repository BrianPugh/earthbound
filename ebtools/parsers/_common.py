"""Shared utilities for assembly output parsers."""


def format_pointer(ptr: int) -> str:
    """Format a 24-bit pointer as a .DWORD assembly directive."""
    if ptr == 0:
        return "  .DWORD NULL\n"
    return f"  .DWORD TEXT_BLOCK_{ptr:06X}\n"
