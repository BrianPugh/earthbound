#include "platform/platform.h"
#include "game/audio.h"
#include "game_main.h"
#include "board.h"

#ifdef ENABLE_AUDIO

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include <string.h>

/* PWM audio: 8-bit resolution at 125 MHz → ~488 kHz carrier.
 * A repeating timer fires at 32 kHz to feed one mono sample to PWM.
 * The main loop generates 534-sample frames into a ring buffer. */

#define AUDIO_SAMPLE_RATE   32000
#define SAMPLES_PER_FRAME   534
#define PWM_WRAP            255

/* Ring buffer: power-of-two size for cheap index masking.
 * 1024 samples ≈ 1.9 frames of headroom at 32 kHz. */
#define RING_BUF_SAMPLES    1024
#define RING_BUF_MASK       (RING_BUF_SAMPLES - 1)

/* Mono samples, unsigned 8-bit (0–255), ready for PWM */
static uint8_t ring_buf[RING_BUF_SAMPLES];
static volatile uint32_t ring_read;   /* ISR reads */
static volatile uint32_t ring_write;  /* main thread writes */

static uint pwm_slice;
static uint pwm_channel;
static struct repeating_timer audio_timer;
static spin_lock_t *audio_spinlock;

/* Called at ~32 kHz from timer ISR.  Reads one sample from the ring buffer
 * and writes it to the PWM duty register. */
static bool audio_timer_callback(struct repeating_timer *t) {
    (void)t;
    uint32_t r = ring_read;
    uint32_t w = ring_write;
    if (r != w) {
        pwm_set_chan_level(pwm_slice, pwm_channel, ring_buf[r & RING_BUF_MASK]);
        ring_read = r + 1;
    } else {
        /* Underrun — output midpoint (silence) */
        pwm_set_chan_level(pwm_slice, pwm_channel, PWM_WRAP / 2);
    }
    return true;  /* keep repeating */
}

/* Convert a frame of signed 16-bit stereo samples to unsigned 8-bit mono
 * and append to the ring buffer. */
static void push_frame(const int16_t *stereo, int count) {
    for (int i = 0; i < count; i++) {
        /* Mix stereo to mono: average L and R */
        int32_t l = stereo[i * 2];
        int32_t r = stereo[i * 2 + 1];
        int32_t mono = (l + r) / 2;   /* -32768..32767 */

        /* Shift from signed 16-bit to unsigned 8-bit */
        uint8_t sample = (uint8_t)((mono + 32768) >> 8);  /* 0..255 */

        uint32_t w = ring_write;
        /* Drop sample if buffer full (shouldn't happen in normal operation) */
        if (w - ring_read < RING_BUF_SAMPLES) {
            ring_buf[w & RING_BUF_MASK] = sample;
            ring_write = w + 1;
        }
    }
}

bool platform_audio_init(void) {
    /* Set up spinlock (lightweight mutex for ISR context) */
    int spin_id = spin_lock_claim_unused(true);
    audio_spinlock = spin_lock_init(spin_id);

    /* Initialize game audio engine */
    audio_init();

    /* Pre-fill ring buffer with silence */
    ring_read = 0;
    ring_write = 0;

    /* Configure PWM on the audio pin */
    gpio_set_function(PIN_AUDIO_PWM, GPIO_FUNC_PWM);
    pwm_slice = pwm_gpio_to_slice_num(PIN_AUDIO_PWM);
    pwm_channel = pwm_gpio_to_channel(PIN_AUDIO_PWM);
    pwm_set_wrap(pwm_slice, PWM_WRAP);
    pwm_set_chan_level(pwm_slice, pwm_channel, PWM_WRAP / 2);  /* silence */
    pwm_set_enabled(pwm_slice, true);

    /* Start repeating timer at 32 kHz (negative period = µs interval) */
    int period_us = -(1000000 / AUDIO_SAMPLE_RATE);  /* -31 µs */
    add_repeating_timer_us(period_us, audio_timer_callback, NULL, &audio_timer);

    return true;
}

void platform_audio_shutdown(void) {
    cancel_repeating_timer(&audio_timer);
    pwm_set_enabled(pwm_slice, false);
    audio_shutdown();
}

void platform_audio_lock(void) {
    if (audio_spinlock) {
        spin_lock_unsafe_blocking(audio_spinlock);
    }
}

void platform_audio_unlock(void) {
    if (audio_spinlock) {
        spin_unlock_unsafe(audio_spinlock);
    }
}

void platform_audio_pump(void) {
    /* Generate frames until the ring buffer is reasonably full.
     * Target: keep at least 2 frames ahead of the read pointer. */
    while ((ring_write - ring_read) < (SAMPLES_PER_FRAME * 2)) {
        int16_t frame_buf[SAMPLES_PER_FRAME * 2];  /* stereo */
        if (game_is_fast_forward()) {
            memset(frame_buf, 0, sizeof(frame_buf));
        } else {
            audio_generate_samples(frame_buf, SAMPLES_PER_FRAME);
        }
        push_frame(frame_buf, SAMPLES_PER_FRAME);
    }
}

#else /* !ENABLE_AUDIO */

bool platform_audio_init(void) { return true; }
void platform_audio_shutdown(void) {}
void platform_audio_lock(void) {}
void platform_audio_unlock(void) {}
void platform_audio_pump(void) {}

#endif /* ENABLE_AUDIO */
