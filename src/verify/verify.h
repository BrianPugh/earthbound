#ifndef VERIFY_H
#define VERIFY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef ENABLE_VERIFY

/* Initialize the verification emulator with a ROM file.
   Returns true on success. */
bool verify_init(const char *rom_path);

/* Advance the emulator one frame and compare PPU state.
   Call this once per frame from wait_for_vblank().
   pad_state is the current joypad bitmask to mirror to the emulator.
   Returns true if PPU states match. */
bool verify_frame(uint16_t pad_state);

/* Shut down the verification emulator and free resources. */
void verify_shutdown(void);

/* Return the current frame number. */
uint32_t verify_get_frame(void);

#else

/* No-op stubs when verification is disabled */
static inline bool verify_init(const char *rom_path) { (void)rom_path; return false; }
static inline bool verify_frame(uint16_t pad_state) { (void)pad_state; return true; }
static inline void verify_shutdown(void) {}
static inline uint32_t verify_get_frame(void) { return 0; }

#endif /* ENABLE_VERIFY */

#endif /* VERIFY_H */
