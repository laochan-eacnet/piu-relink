#define _GNU_SOURCE

#include "input/bindings.h"

#include "config/config.h"
#include "core/log.h"

#include <dlfcn.h>
#include <string.h>

typedef unsigned long (*xstring_to_keysym_fn)(const char *);
typedef unsigned int (*xkeysym_to_keycode_fn)(void *, unsigned long);

static struct pt_key_binding bindings[PT_INPUT_ACTION_COUNT];
static void *resolved_display;

static const uint32_t PHOENIX_ACTIONS[PT_INPUT_CARD1] = {
    1u, 0u, 4u, 2u, 3u, 6u, 5u, 9u, 7u, 8u, 16u, 15u, 18u
};

static const char *configured_name(enum pt_input_action action)
{
    const struct pt_input_bindings *input = &pt_config_get()->input;

    switch (action) {
    case PT_INPUT_UPPER_LEFT: return input->upper_left;
    case PT_INPUT_UPPER_RIGHT: return input->upper_right;
    case PT_INPUT_CENTER: return input->center;
    case PT_INPUT_LOWER_LEFT: return input->lower_left;
    case PT_INPUT_LOWER_RIGHT: return input->lower_right;
    case PT_INPUT_P2_UPPER_LEFT: return input->p2_upper_left;
    case PT_INPUT_P2_UPPER_RIGHT: return input->p2_upper_right;
    case PT_INPUT_P2_CENTER: return input->p2_center;
    case PT_INPUT_P2_LOWER_LEFT: return input->p2_lower_left;
    case PT_INPUT_P2_LOWER_RIGHT: return input->p2_lower_right;
    case PT_INPUT_SERVICE: return input->service;
    case PT_INPUT_TEST: return input->test;
    case PT_INPUT_COIN1: return input->coin1;
    case PT_INPUT_CARD1: return input->card1;
    case PT_INPUT_CARD2: return input->card2;
    default: return "";
    }
}

const char *pt_input_action_name(enum pt_input_action action)
{
    static const char *const names[PT_INPUT_ACTION_COUNT] = {
        "p1-upper-left", "p1-upper-right", "p1-center", "p1-lower-left",
        "p1-lower-right", "p2-upper-left", "p2-upper-right", "p2-center",
        "p2-lower-left", "p2-lower-right", "service", "test", "coin1",
        "card1", "card2"
    };
    return action < PT_INPUT_ACTION_COUNT ? names[action] : "unknown";
}

int pt_bindings_resolve(void *display)
{
    xstring_to_keysym_fn string_to_keysym;
    xkeysym_to_keycode_fn keysym_to_keycode;
    unsigned int action;

    if (!display)
        return -1;
    if (resolved_display == display)
        return 0;
    string_to_keysym = (xstring_to_keysym_fn)dlsym(RTLD_NEXT, "XStringToKeysym");
    keysym_to_keycode =
        (xkeysym_to_keycode_fn)dlsym(RTLD_NEXT, "XKeysymToKeycode");
    if (!string_to_keysym || !keysym_to_keycode) {
        pt_log("input", "cannot resolve X11 key conversion functions");
        return -1;
    }

    memset(bindings, 0, sizeof(bindings));
    for (action = 0; action < PT_INPUT_ACTION_COUNT; ++action) {
        unsigned int previous;
        const char *name;
        unsigned long keysym;
        uint32_t keycode;

        if (action >= PT_INPUT_CARD1 && !pt_config_get()->virtual_card_reader)
            continue;
        name = configured_name((enum pt_input_action)action);
        keysym = string_to_keysym(name);

        if (!keysym && name[0] && !name[1])
            keysym = (unsigned char)name[0];
        keycode = keysym ? keysym_to_keycode(display, keysym) : 0u;
        if (!keysym || keycode < 8u || keycode >= 256u) {
            pt_log("input", "invalid binding %s=%s",
                   pt_input_action_name((enum pt_input_action)action), name);
            return -1;
        }
        for (previous = 0; previous < action; ++previous) {
            if (bindings[previous].keycode == keycode) {
                pt_log("input", "duplicate key %s for %s and %s", name,
                       pt_input_action_name((enum pt_input_action)previous),
                       pt_input_action_name((enum pt_input_action)action));
                return -1;
            }
        }
        bindings[action].configured_name = name;
        bindings[action].keysym = keysym;
        bindings[action].keycode = keycode;
        if (action < PT_INPUT_CARD1)
            bindings[action].phoenix_action = PHOENIX_ACTIONS[action];
    }
    resolved_display = display;
    if (pt_config_verbose())
        pt_log("input", "resolved all configured X11 key bindings");
    return 0;
}

const struct pt_key_binding *pt_binding_get(enum pt_input_action action)
{
    return action < PT_INPUT_ACTION_COUNT ? &bindings[action] : NULL;
}

int pt_binding_find_keycode(uint32_t keycode)
{
    unsigned int action;

    for (action = 0; action < PT_INPUT_ACTION_COUNT; ++action)
        if (keycode && bindings[action].keycode == keycode)
            return (int)action;
    return -1;
}
