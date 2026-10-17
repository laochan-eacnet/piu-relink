#define _GNU_SOURCE
#include "card_input_fixture.h"

/* XKeyEvent's public, 32-bit Xlib ABI; no X11 development library is needed. */
struct fixture_key_event {
    int type;
    unsigned long serial;
    int send_event;
    void *display;
    unsigned long window, root, subwindow, time;
    int x, y, x_root, y_root;
    unsigned int state, keycode;
    int same_screen;
};

static const struct { const char *name; unsigned int code; } key_names[] = {
    {"Q", 24}, {"E", 26}, {"S", 39}, {"Z", 52}, {"C", 54},
    {"U", 30}, {"O", 32}, {"K", 45}, {"M", 58}, {"period", 60},
    {"F1", 67}, {"F2", 68}, {"5", 14}, {"F3", 69}, {"F4", 70},
    {"F6", 72}, {"F7", 73}
};
static struct fixture_key_event queued_event;
static unsigned char physical_keymap[32];

int XNextEvent(void *, void *);
int XQueryKeymap(void *, char[32]);
int XLookupString(void *, char *, int, unsigned long *, void *);

void pt_lxio_reset(void)
{
    ++lxio_resets;
}

void pt_lxio_set_action(enum pt_input_action action, int pressed)
{
    /* A card must never enter the cabinet/LXIO action namespace. */
    assert((unsigned int)action < PT_INPUT_CARD1);
    assert(pressed == 0 || pressed == 1);
    ++lxio_calls;
}

unsigned int keycode(const char *name)
{
    size_t index;

    for (index = 0; index < sizeof(key_names) / sizeof(key_names[0]); ++index)
        if (!strcmp(name, key_names[index].name))
            return key_names[index].code;
    return 0;
}

static unsigned long backend_string_to_keysym(const char *name)
{
    unsigned int code = keycode(name);

    ++conversion_calls;
    if (!strcmp(name, "F3") || !strcmp(name, "F4") ||
        !strcmp(name, "invalid-card-name"))
        ++card_name_calls;
    return code ? 0x1000u + code : 0;
}

static unsigned int backend_keysym_to_keycode(void *connection,
                                              unsigned long keysym)
{
    assert(connection);
    return keysym >= 0x1008u && keysym < 0x1100u ?
        (unsigned int)(keysym - 0x1000u) : 0;
}

static int backend_next_event(void *connection, void *event)
{
    assert(connection == display);
    if (event)
        memcpy(event, &queued_event, sizeof(queued_event));
    return 73;
}

static int backend_query_keymap(void *connection, char keymap[32])
{
    assert(connection == display);
    if (keymap)
        memcpy(keymap, physical_keymap, sizeof(physical_keymap));
    return 1;
}

static int backend_lookup_string(void *event, char *buffer, int capacity,
                                  unsigned long *keysym, void *compose)
{
    (void)event;
    (void)compose;
    ++lookup_calls;
    if (buffer && capacity > 0)
        buffer[0] = 'x';
    if (keysym)
        *keysym = 0xabcdu;
    return 1;
}

/* Compile all linked sources with -Ddlsym=card_fixture_dlsym. */
void *card_fixture_dlsym(void *handle, const char *name)
{
    assert(handle == RTLD_NEXT);
    if (!strcmp(name, "fopen")) return (void *)__wrap_fopen;
    if (!strcmp(name, "XStringToKeysym")) return (void *)backend_string_to_keysym;
    if (!strcmp(name, "XKeysymToKeycode")) return (void *)backend_keysym_to_keycode;
    if (!strcmp(name, "XNextEvent")) return (void *)backend_next_event;
    if (!strcmp(name, "XQueryKeymap")) return (void *)backend_query_keymap;
    if (!strcmp(name, "XLookupString")) return (void *)backend_lookup_string;
    assert(!"unexpected RTLD_NEXT lookup");
    return NULL;
}

void event_key(const char *name, int type, int consumed)
{
    struct fixture_key_event event;

    memset(&queued_event, 0, sizeof(queued_event));
    queued_event.type = type;
    queued_event.display = display;
    queued_event.keycode = keycode(name);
    assert(queued_event.keycode);
    assert(XNextEvent(display, &event) == 73);
    assert(event.type == (consumed ? 0 : type));
    assert(event.keycode == queued_event.keycode);
}

void lookup_key(const char *name, int consumed)
{
    struct fixture_key_event event = {.type = KEY_PRESS};
    char buffer[4] = {'?', '?', '?', '?'};
    unsigned long keysym = 99;
    unsigned int before = lookup_calls;

    event.keycode = keycode(name);
    assert(XLookupString(&event, buffer, sizeof(buffer), &keysym, NULL) == !consumed);
    assert(buffer[0] == (consumed ? '\0' : 'x'));
    assert(buffer[1] == '?');
    assert(keysym == (consumed ? 0u : 0xabcdu));
    assert(lookup_calls == before + !consumed);
}

void check_keymap(int cards, int lxio)
{
    unsigned char expected[32];
    char actual[32];
    unsigned int action;

    memset(physical_keymap, 0xff, sizeof(physical_keymap));
    physical_keymap[200u >> 3] &= (unsigned char)~(1u << (200u & 7u));
    memcpy(expected, physical_keymap, sizeof(expected));
    for (action = 0; action < PT_INPUT_ACTION_COUNT; ++action) {
        unsigned int code;

        if (!(action < PT_INPUT_CARD1 ? lxio : cards))
            continue;
        code = pt_binding_get((enum pt_input_action)action)->keycode;
        assert(code >= 8 && code < 256);
        expected[code >> 3] &= (unsigned char)~(1u << (code & 7u));
    }
    assert(XQueryKeymap(display, actual) == 1);
    assert(!memcmp(actual, expected, sizeof(expected)));
}

void check_cabinet_key(int lxio)
{
    unsigned int before = lxio_calls;

    event_key("Q", KEY_PRESS, lxio);
    event_key("Q", KEY_RELEASE, lxio);
    assert(lxio_calls == before + (lxio ? 2u : 0u));
    lookup_key("Q", lxio);
}
