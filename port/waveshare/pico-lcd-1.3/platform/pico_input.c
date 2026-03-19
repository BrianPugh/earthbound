#include "platform/platform.h"
#include "include/pad.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "board.h"

/* Global mode flags */
bool platform_headless = false;
bool platform_skip_intro = false;
int platform_max_frames = 0;

static uint16_t pad_state;
static uint16_t pad_prev;
static uint16_t aux_state;

typedef struct {
    uint gpio;
    uint16_t pad_bit;
} button_map_t;

static const button_map_t button_map[] = {
    { PIN_JOY_UP,    PAD_UP    },
    { PIN_JOY_DOWN,  PAD_DOWN  },
    { PIN_JOY_LEFT,  PAD_LEFT  },
    { PIN_JOY_RIGHT, PAD_RIGHT },
    { PIN_BTN_B,     PAD_X     },  /* waveshare B -> toggle map */
    { PIN_BTN_X,     PAD_B     },  /* waveshare X -> cancel/back */
    { PIN_BTN_Y,     PAD_L     },  /* waveshare Y -> confirm/interact */
};

typedef struct {
    uint gpio;
    uint16_t aux_bit;
} aux_map_t;

static const aux_map_t aux_map[] = {
    { PIN_BTN_A, AUX_FPS_TOGGLE },  /* waveshare A -> toggle FPS counter */
};

#define BUTTON_COUNT (sizeof(button_map) / sizeof(button_map[0]))
#define AUX_COUNT    (sizeof(aux_map) / sizeof(aux_map[0]))

bool platform_input_init(void) {
    pad_state = 0;
    pad_prev = 0;
    aux_state = 0;

    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        gpio_init(button_map[i].gpio);
        gpio_set_dir(button_map[i].gpio, GPIO_IN);
        gpio_pull_up(button_map[i].gpio);
    }

    for (size_t i = 0; i < AUX_COUNT; i++) {
        gpio_init(aux_map[i].gpio);
        gpio_set_dir(aux_map[i].gpio, GPIO_IN);
        gpio_pull_up(aux_map[i].gpio);
    }

    return true;
}

void platform_input_shutdown(void) {}

void platform_input_poll(void) {
    pad_prev = pad_state;
    pad_state = 0;
    aux_state = 0;

    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        if (!gpio_get(button_map[i].gpio))  /* active low */
            pad_state |= button_map[i].pad_bit;
    }

    for (size_t i = 0; i < AUX_COUNT; i++) {
        if (!gpio_get(aux_map[i].gpio))  /* active low */
            aux_state |= aux_map[i].aux_bit;
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
    return false;  /* no quit on embedded */
}

void platform_request_quit(void) {}
