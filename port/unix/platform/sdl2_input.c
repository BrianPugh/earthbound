#include "platform/platform.h"
#include "include/pad.h"
#include <SDL.h>
#include <signal.h>

static uint16_t pad_state;
static uint16_t pad_prev;
static uint16_t aux_state;
static volatile sig_atomic_t quit_requested;

/* Global mode flags */
bool platform_headless = false;
bool platform_skip_intro = false;
int platform_max_frames = 0;

static void signal_handler(int sig) {
    (void)sig;
    quit_requested = 1;
}

bool platform_input_init(void) {
    pad_state = 0;
    pad_prev = 0;
    aux_state = 0;
    quit_requested = 0;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    return true;
}

void platform_input_shutdown(void) {
    /* nothing to clean up */
}

void platform_input_poll(void) {
    SDL_Event event;

    pad_prev = pad_state;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            quit_requested = 1;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            uint16_t bit = 0;
            uint16_t aux_bit = 0;
            switch (event.key.keysym.scancode) {
            case SDL_SCANCODE_X:      bit = PAD_A;      break;
            case SDL_SCANCODE_Z:      bit = PAD_B;      break;
            case SDL_SCANCODE_S:      bit = PAD_X;      break;
            case SDL_SCANCODE_A:      bit = PAD_Y;      break;
            case SDL_SCANCODE_Q:      bit = PAD_L;      break;
            case SDL_SCANCODE_W:      bit = PAD_R;      break;
            case SDL_SCANCODE_RETURN: bit = PAD_START;  break;
            case SDL_SCANCODE_RSHIFT: bit = PAD_SELECT; break;
            case SDL_SCANCODE_UP:     bit = PAD_UP;            break;
            case SDL_SCANCODE_DOWN:   bit = PAD_DOWN;          break;
            case SDL_SCANCODE_LEFT:   bit = PAD_LEFT;          break;
            case SDL_SCANCODE_RIGHT:  bit = PAD_RIGHT;         break;
            case SDL_SCANCODE_F1:     aux_bit = AUX_DEBUG_DUMP;   break;
            case SDL_SCANCODE_F2:     aux_bit = AUX_VRAM_DUMP;    break;
            case SDL_SCANCODE_F3:     aux_bit = AUX_FPS_TOGGLE;   break;
            case SDL_SCANCODE_F4:     aux_bit = AUX_STATE_DUMP;   break;
            case SDL_SCANCODE_TAB:    aux_bit = AUX_FAST_FORWARD; break;
            case SDL_SCANCODE_F5:     aux_bit = AUX_DEBUG_TOGGLE; break;
            default: break;
            }
            if (event.type == SDL_KEYDOWN) {
                pad_state |= bit;
                aux_state |= aux_bit;
            } else {
                pad_state &= ~bit;
                aux_state &= ~aux_bit;
            }
            break;
        }
        }
    }
}

uint16_t platform_input_get_pad(void) {
    return pad_state;
}

uint16_t platform_input_get_pad_new(void) {
    return pad_state & ~pad_prev;
}

uint16_t platform_input_get_aux(void) {
    return aux_state;
}

bool platform_input_quit_requested(void) {
    return quit_requested != 0;
}

void platform_request_quit(void) {
    quit_requested = 1;
}
