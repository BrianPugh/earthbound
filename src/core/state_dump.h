#ifndef CORE_STATE_DUMP_H
#define CORE_STATE_DUMP_H

#include <stdbool.h>

/* Crash-safe ping-pong savestate persistence (build-order items #4 + #5).
 *
 * Storage goes through the port's platform_savestate_* slot backend (file-backed on
 * desktop, flash on embedded). Two ping-pong SLOTS each carry a monotonically
 * increasing sequence number and a CRC-32 over their payload:
 *   - save_slots() serializes the live state to the slot that is NOT the current
 *     newest-valid one, so a power-loss mid-write leaves the prior slot intact (the
 *     commit is atomic via the sequence number).
 *   - load_slots() validates (CRC) the newest slot and applies it, falling back to
 *     the older slot if the newest is torn. It validates BEFORE touching any live
 *     state, so a corrupt slot never partially overwrites the running game.
 *
 * Both MUST be called at a root-loop boundary (host_root_boundary) — never mid-pump —
 * since load replaces the mode stack wholesale. load_slots() returns false only when
 * NEITHER slot is valid. */
bool state_dump_save_slots(void);
bool state_dump_load_slots(void);

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
