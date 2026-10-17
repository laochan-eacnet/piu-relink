#pragma once

#include <stddef.h>
#include <stdint.h>

enum pt_input_action {
    PT_INPUT_UPPER_LEFT,
    PT_INPUT_UPPER_RIGHT,
    PT_INPUT_CENTER,
    PT_INPUT_LOWER_LEFT,
    PT_INPUT_LOWER_RIGHT,
    PT_INPUT_P2_UPPER_LEFT,
    PT_INPUT_P2_UPPER_RIGHT,
    PT_INPUT_P2_CENTER,
    PT_INPUT_P2_LOWER_LEFT,
    PT_INPUT_P2_LOWER_RIGHT,
    PT_INPUT_SERVICE,
    PT_INPUT_TEST,
    PT_INPUT_COIN1,
    PT_INPUT_CARD1,
    PT_INPUT_CARD2,
    PT_INPUT_ACTION_COUNT
};

struct pt_key_binding {
    const char *configured_name;
    unsigned long keysym;
    uint32_t keycode;
    uint32_t phoenix_action;
};

int pt_bindings_resolve(void *display);
const struct pt_key_binding *pt_binding_get(enum pt_input_action action);
int pt_binding_find_keycode(uint32_t keycode);
const char *pt_input_action_name(enum pt_input_action action);

