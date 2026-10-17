#define _GNU_SOURCE

#include "input/input.h"

#include "config/config.h"
#include "input/bindings.h"
#include "input/lxio.h"
#include "safety/network_card.h"

#include <dlfcn.h>
#include <stdint.h>

#define X_KEY_PRESS 2
#define X_KEY_RELEASE 3
#define X_KEYCODE_OFFSET 52u
#define PHOENIX_BINDING_COUNT (*(volatile uint32_t *)0x097BE388u)
#define PHOENIX_BINDINGS ((volatile struct phoenix_keyboard_binding *)0x097BE38Cu)
#define PHOENIX_MAX_BINDINGS 128u

typedef unsigned long pt_keysym;
typedef int (*xnext_event_fn)(void *, void *);
typedef int (*xquery_keymap_fn)(void *, char[32]);
typedef int (*xlookup_string_fn)(void *, char *, int, pt_keysym *, void *);

struct phoenix_keyboard_binding {
    uint32_t mode;
    uint32_t reserved;
    uint32_t key_index;
    uint32_t action_id;
    uint32_t state;
    uint32_t timestamp;
};

static xnext_event_fn real_xnext_event;
static xquery_keymap_fn real_xquery_keymap;
static xlookup_string_fn real_xlookup_string;

static void install_phoenix_binding(enum pt_input_action action)
{
    const struct pt_key_binding *binding = pt_binding_get(action);
    volatile struct phoenix_keyboard_binding *table = PHOENIX_BINDINGS;
    uint32_t count = PHOENIX_BINDING_COUNT;
    uint32_t index;

    if (!binding || binding->keycode < 8u || count < 5u ||
        count >= PHOENIX_MAX_BINDINGS || pt_config_get()->virtual_lxio)
        return;
    for (index = 5u; index < count; ++index) {
        if (table[index].mode == 0u &&
            table[index].action_id == binding->phoenix_action) {
            table[index].key_index = binding->keycode - 8u;
            return;
        }
    }
    table[count].mode = 0;
    table[count].reserved = table[0].reserved;
    table[count].key_index = binding->keycode - 8u;
    table[count].action_id = binding->phoenix_action;
    table[count].state = 0;
    table[count].timestamp = 0;
    PHOENIX_BINDING_COUNT = count + 1u;
}

void pt_input_install_phoenix_bindings(void)
{
    enum pt_input_action action;

    for (action = PT_INPUT_UPPER_LEFT; action < PT_INPUT_CARD1; ++action)
        install_phoenix_binding(action);
}

void pt_input_initialize(void)
{
    pt_lxio_reset();
}

static int action_consumed(int action)
{
    if (action < 0)
        return 0;
    return action >= PT_INPUT_CARD1 ? pt_config_get()->virtual_card_reader :
                                     pt_config_get()->virtual_lxio;
}

int XNextEvent(void *display, void *event)
{
    uint32_t keycode;
    int event_type;
    int action;
    int result;

    if (!real_xnext_event)
        real_xnext_event = (xnext_event_fn)dlsym(RTLD_NEXT, "XNextEvent");
    if (!real_xnext_event)
        return -1;
    result = real_xnext_event(display, event);
    if (!event || pt_bindings_resolve(display) != 0)
        return result;

    event_type = *(const int *)event;
    if (event_type != X_KEY_PRESS && event_type != X_KEY_RELEASE)
        return result;
    keycode = *(const uint32_t *)((const unsigned char *)event + X_KEYCODE_OFFSET);
    action = pt_binding_find_keycode(keycode);
    if (action < 0)
        return result;

    if (action >= PT_INPUT_CARD1 && pt_config_get()->virtual_card_reader) {
        pt_network_card_key((unsigned int)(action - PT_INPUT_CARD1),
                            event_type == X_KEY_PRESS);
        *(int *)event = 0;
        return result;
    }

    if (pt_config_get()->virtual_lxio) {
        pt_lxio_set_action((enum pt_input_action)action,
                           event_type == X_KEY_PRESS);
        *(int *)event = 0;
    } else if (event_type == X_KEY_PRESS) {
        install_phoenix_binding((enum pt_input_action)action);
    }
    return result;
}

int XQueryKeymap(void *display, char keymap[32])
{
    unsigned int action;
    int result;

    if (!real_xquery_keymap)
        real_xquery_keymap =
            (xquery_keymap_fn)dlsym(RTLD_NEXT, "XQueryKeymap");
    if (!real_xquery_keymap)
        return 0;
    result = real_xquery_keymap(display, keymap);
    if (!result || !keymap ||
        (!pt_config_get()->virtual_lxio && !pt_config_get()->virtual_card_reader) ||
        pt_bindings_resolve(display) != 0)
        return result;

    for (action = 0; action < PT_INPUT_ACTION_COUNT; ++action) {
        uint32_t keycode;

        if (!action_consumed((int)action))
            continue;
        keycode = pt_binding_get((enum pt_input_action)action)->keycode;
        if (keycode < 8u)
            continue;
        keymap[keycode >> 3] =
            (char)((unsigned char)keymap[keycode >> 3] &
                   (unsigned char)~(1u << (keycode & 7u)));
    }
    return result;
}

int XLookupString(void *event, char *buffer, int capacity,
                  pt_keysym *keysym_return, void *compose_status)
{
    uint32_t keycode;

    if (event) {
        keycode = *(const uint32_t *)((const unsigned char *)event +
                                     X_KEYCODE_OFFSET);
        if (action_consumed(pt_binding_find_keycode(keycode))) {
            if (buffer && capacity > 0)
                buffer[0] = '\0';
            if (keysym_return)
                *keysym_return = 0;
            return 0;
        }
    }
    if (!real_xlookup_string)
        real_xlookup_string =
            (xlookup_string_fn)dlsym(RTLD_NEXT, "XLookupString");
    return real_xlookup_string ?
        real_xlookup_string(event, buffer, capacity, keysym_return,
                            compose_status) : 0;
}
