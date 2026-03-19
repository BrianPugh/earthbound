/*
 * SNES audio platform implementation.
 *
 * Replaces the lakesnes SPC700 emulator with real hardware communication.
 * The game logic calls change_music() / play_sfx() which write to APU
 * ports.  On SNES, these writes go to hardware ports $2140-$2143.
 */

#include "platform/platform.h"

bool platform_audio_init(void) {
    /*
     * TODO: Upload SPC700 driver binary to APU RAM.
     *
     * The SPC700 IPL boot ROM at $FFC0-$FFFF handles initial upload:
     *   1. Wait for $2140 to read $AA, $2141 to read $BB (ready signal)
     *   2. Write destination address to $2142-$2143
     *   3. Write data bytes to $2141, incrementing counter in $2140
     *   4. Start execution by writing entry address + $00 to trigger
     *
     * The audio pack data comes from the extracted SPC binary assets.
     * On the C port, this is done via memcpy to apu->ram[]; on SNES,
     * it uses the IPL protocol above.
     */
    return true;
}

void platform_audio_shutdown(void) {
    /*
     * TODO: Send stop command to SPC700.
     * Write 0 to port 0 ($2140) to stop music playback.
     */
}

void platform_audio_lock(void) {
    /* No-op: SNES is single-threaded, no mutex needed. */
}

void platform_audio_unlock(void) {
    /* No-op: SNES is single-threaded, no mutex needed. */
}
