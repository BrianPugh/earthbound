#ifndef CORE_STATE_DUMP_H
#define CORE_STATE_DUMP_H

#include <stdbool.h>

/* Dump all module state to a binary file.
 * Format: "EBSD" header + tagged sections + 0xFFFF terminator.
 * Returns true on success. */
bool state_dump_save(const char *path);

/* Restore all module state from a file written by state_dump_save(). Each tagged
 * section is read straight back into its live global; unknown/mis-sized sections
 * are skipped. MUST be called at a root-loop boundary (host_root_boundary) — never
 * mid-pump — since it replaces the mode stack wholesale. Returns true on success
 * (a missing/short/wrong-magic file returns false and leaves earlier sections that
 * were already read in place; full validate-on-load is a later build-order item). */
bool state_dump_load(const char *path);

/* Crash-safe ping-pong persistence (build-order item #4). Writes alternate between
 * two slots ("<basepath>.0" / "<basepath>.1"), each tagged with a monotonically
 * increasing sequence number and a CRC-32 over its payload. A load picks the valid
 * slot with the highest sequence and falls back to the other if the newest one is
 * torn (CRC mismatch). This makes a power-loss mid-write non-destructive: the prior
 * slot stays intact until the new write fully commits. Returns true on success;
 * load_slots returns false only when NEITHER slot is valid. */
bool state_dump_save_slots(const char *basepath);
bool state_dump_load_slots(const char *basepath);

/* save -> load -> save idempotency self-test on the current live state: returns
 * true iff the loader reads back exactly what the writer wrote (byte-identical).
 * Desktop-only diagnostic (the `--selftest-savestate` flag); always false on
 * embedded. */
bool state_dump_roundtrip_test(void);

/* Crash-safety self-test: writes two ping-pong slots, corrupts the newer one and
 * confirms a load falls back to the older slot, then corrupts both and confirms the
 * load fails cleanly. Returns true on success. Desktop-only; always false on
 * embedded. */
bool state_dump_crashsafe_test(void);

#endif /* CORE_STATE_DUMP_H */
