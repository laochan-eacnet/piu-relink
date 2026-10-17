#define _GNU_SOURCE
#include "card_input_fixture.h"
#include "config/config_internal.h"

/*
 * Run production config, reader, binding, and X11-hook code, not source-text
 * assertions. Each fresh child owns its native maps and production static state.
 */
static void test_disabled(unsigned int lxio)
{
    unsigned char boundary_before[sizeof(supported_boundary)];
    unsigned int player;

    setup(-1, (int)lxio, "");
    assert(!strcmp(pt_config_get()->input.card1, "F3"));
    assert(!strcmp(pt_config_get()->input.card2, "F4"));
    map_native_inputs();
    map_native_card();
    memcpy(boundary_before, (void *)(uintptr_t)CARD_BOUNDARY, sizeof(boundary_before));
    assert(pt_network_card_initialize() == 0);
    assert(pt_bindings_resolve(display) == 0);
    assert(conversion_calls == PT_INPUT_CARD1 && card_name_calls == 0);
    check_native_install((int)lxio);
    pt_input_initialize();
    assert(lxio_resets == 1);
    for (player = 0; player < 2; ++player) {
        const char *name = player ? "F4" : "F3";
        unsigned int before = lxio_calls;

        event_key(name, KEY_PRESS, 0);
        event_key(name, KEY_RELEASE, 0);
        lookup_key(name, 0);
        pt_network_card_key(player, 1);
        pt_network_card_key(player, 0);
        expect_frame(player, NULL, 0, 0);
        assert(lxio_calls == before);
    }
    check_keymap(0, (int)lxio);
    check_cabinet_key((int)lxio);
    assert(open_calls == 0 && fopen_calls == 0);
    assert(!memcmp(boundary_before, (void *)(uintptr_t)CARD_BOUNDARY,
                   sizeof(boundary_before)));

    /* Disabled card names are not even resolved/validated for duplicates. */
    load_config(0, (int)lxio, "card1=invalid-card-name\ncard2=Q\n");
    display = (void *)(uintptr_t)0x2345u;
    conversion_calls = card_name_calls = 0;
    open_calls = fopen_calls = 0;
    assert(pt_network_card_initialize() == 0);
    assert(pt_bindings_resolve(display) == 0);
    assert(conversion_calls == PT_INPUT_CARD1 && card_name_calls == 0);
    assert(pt_binding_find_keycode(keycode("Q")) == PT_INPUT_UPPER_LEFT);
    assert(open_calls == 0 && fopen_calls == 0);
}

static void test_enabled(unsigned int lxio)
{
    static const unsigned char first[] = {0x00, 0x12, 0xab, 0xcd};
    static const unsigned char second[] = {0xfe, 0xdc, 0xba, 0x98};
    static const unsigned char changed[] = {0x00, 0x01};
    struct native_binding native_before[128];
    uint32_t count_before;
    unsigned int actions_before;
    unsigned int player;

    enabled_setup((int)lxio, "");
    check_native_install((int)lxio);
    count_before = *native_count();
    memcpy(native_before, native_bindings(), sizeof(native_before));
    write_text(card_paths[0], " \t0012aBcD\r\n");
    write_text(card_paths[1], "fEdCbA98\r\n");
    write_text("target/card1.txt", "aa");
    write_text("target/card2.txt", "aa");
    write_text("runtime/card1.txt", "bb");
    write_text("runtime/card2.txt", "bb");
    write_text("elsewhere/card1.txt", "cc");
    write_text("elsewhere/card2.txt", "cc");
    write_text("card1.txt", "dd");
    write_text("card2.txt", "dd");
    assert(chdir("elsewhere") == 0);
    /* Resolve once beside the INI pathname/symlink, not its target or CWD. */
    expect_frame(0, NULL, 0, 1);
    expect_frame(1, NULL, 0, 1);
    actions_before = lxio_calls;
    event_key("F3", KEY_PRESS, 1);
    event_key("F4", KEY_PRESS, 1);
    assert(open_calls == 2 && lxio_calls == actions_before);
    lookup_key("F3", 1);
    lookup_key("F4", 1);
    check_keymap(1, (int)lxio);
    assert(open_calls == 2);
    assert(*native_count() == count_before);
    assert(!memcmp(native_before, native_bindings(), sizeof(native_before)));
    /* Changing the file cannot change an already queued scan. */
    write_text(card_paths[0], "0001");
    expect_frame(1, second, sizeof(second), 1);
    expect_frame(1, NULL, 0, 1);
    expect_frame(0, first, sizeof(first), 1);
    expect_frame(0, NULL, 0, 0);

    for (player = 0; player < 2; ++player) {
        const char *name = player ? "F4" : "F3";
        unsigned int opens_before = open_calls;

        write_text(card_paths[player], "0001");
        event_key(name, KEY_PRESS, 1);
        event_key(name, KEY_PRESS, 1);
        assert(open_calls == opens_before);
        expect_frame(player, NULL, 0, 1);
        event_key(name, KEY_RELEASE, 1);
        assert(open_calls == opens_before);
        event_key(name, KEY_PRESS, 1);
        assert(open_calls == opens_before + 1u);
        expect_frame(player, changed, sizeof(changed), 0);
    }
    /* Two pending players; only an explicit new P1 scan replaces P1. */
    write_text(card_paths[0], "0012abcd");
    write_text(card_paths[1], "fedcba98");
    explicit_scan(0);
    explicit_scan(1);
    write_text(card_paths[0], "0001");
    explicit_scan(0);
    expect_frame(0, changed, sizeof(changed), 1);
    expect_frame(1, second, sizeof(second), 0);
    assert(lxio_calls == actions_before);
    assert(*native_count() == count_before);
    assert(!memcmp(native_before, native_bindings(), sizeof(native_before)));
    check_cabinet_key((int)lxio);
}

static void test_binding_rejection(unsigned int kind)
{
    static const char *const settings[] = {
        "card1=invalid-card-name\n", "card1=Q\n", "card2=F3\n"
    };

    assert(kind < sizeof(settings) / sizeof(settings[0]));
    setup(1, 0, settings[kind]);
    assert(pt_bindings_resolve(display) == -1);
    assert(log_calls > 0);
    assert(open_calls == 0);
}

static void test_remapped_bindings(unsigned int unused)
{
    static const unsigned char first[] = {0x01};
    static const unsigned char second[] = {0x02};
    unsigned int before;

    (void)unused;
    enabled_setup(0, "card1=F6\ncard2=F7\n");
    check_native_install(0);
    write_text(card_paths[0], "01");
    write_text(card_paths[1], "02");
    before = open_calls;
    event_key("F3", KEY_PRESS, 0);
    event_key("F4", KEY_PRESS, 0);
    lookup_key("F3", 0);
    lookup_key("F4", 0);
    assert(open_calls == before);
    event_key("F6", KEY_PRESS, 1);
    event_key("F7", KEY_PRESS, 1);
    lookup_key("F6", 1);
    lookup_key("F7", 1);
    check_keymap(1, 0);
    expect_frame(0, first, sizeof(first), 1);
    expect_frame(1, second, sizeof(second), 1);
    assert(!native_has_key(keycode("F6")));
    assert(!native_has_key(keycode("F7")));
    assert(lxio_calls == 0);
}

static void test_runtime_sections_ignored(unsigned int unused)
{
    (void)unused;
    setup(0, 0, "");
    write_text("target/settings.ini",
        "[input]\nvirtual_lxio=false\n"
        "[mounts]\nunknown=value\nnot a key/value line\n"
        "[immutable]\nunknown=value\nnot a key/value line\n"
        "[process]\nunknown=value\nnot a key/value line\n"
        "[display]\nunknown=value\nnot a key/value line\n"
        "[audio]\nunknown=value\nnot a key/value line\n"
        "[serial]\nunknown=value\nnot a key/value line\n"
        "[debug]\nunknown=value\nnot a key/value line\n"
        "[offline]\nregion=Japan\n");
    pt_config_load_path("configured/phoenixtools.ini");
    assert(log_calls == 0);
    assert(!pt_config_get()->virtual_lxio);
    assert(!pt_config_get()->virtual_card_reader);
    assert(pt_config_get()->region_slot == 4);
}

static void run_case(const char *name, void (*test)(unsigned int), unsigned int arg)
{
    pid_t child = fork();
    int status;

    assert(child >= 0);
    if (child == 0) {
        alarm(60);
        test(arg);
        cleanup();
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "card/input case failed: %s (%u), wait status %#x\n",
                name, arg, (unsigned int)status);
        exit(1);
    }
}

int main(void)
{
    unsigned int index;

    run_case("disabled, native cabinet", test_disabled, 0);
    run_case("disabled, virtual LXIO", test_disabled, 1);
    run_case("enabled, native cabinet", test_enabled, 0);
    run_case("enabled, virtual LXIO", test_enabled, 1);
    run_case("file formats and recovery", test_file_formats, 0);
    run_case("no startup or legacy environment scan", test_no_startup_scan, 0);
    for (index = 0; index < 3; ++index)
        run_case("enabled binding rejection", test_binding_rejection, index);
    run_case("remapped card keys", test_remapped_bindings, 0);
    run_case("silently ignored runtime sections", test_runtime_sections_ignored, 0);
    for (index = 0; index < sizeof(supported_boundary); ++index)
        run_case("exact native patch mismatch", test_patch_mismatch, index);
    run_case("unmapped native patch", test_unmapped_patch, 0);
    run_case("parallel independent players", test_parallel_players, 0);
    puts("runtime cards, X11 isolation, and native consumer routing: PASS");
    return 0;
}
