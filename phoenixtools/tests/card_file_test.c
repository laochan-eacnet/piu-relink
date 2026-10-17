#define _GNU_SOURCE
#include "card_input_fixture.h"

static void expect_valid(unsigned int player, const void *text, size_t text_length,
                          const unsigned char *uid, size_t uid_length)
{
    write_bytes(card_paths[player], text, text_length);
    explicit_scan(player);
    expect_frame(player, uid, uid_length, 1);
    expect_frame(player, NULL, 0, 1);
}

enum bad_file_kind { BAD_CONTENTS, BAD_MISSING, BAD_DIRECTORY, BAD_FIFO };

static void expect_bad(unsigned int player, const void *text, size_t length,
                        enum bad_file_kind kind)
{
    static const unsigned char other[] = {0xab, 0xcd};
    static const unsigned char recovered[] = {0x00};
    unsigned int logs_before;

    write_text(card_paths[0], "ABCD");
    write_text(card_paths[1], "ABCD");
    write_text(card_paths[player], "1234");
    explicit_scan(0);
    explicit_scan(1);
    if (kind == BAD_CONTENTS) {
        write_bytes(card_paths[player], text, length);
    } else {
        assert(unlink(card_paths[player]) == 0);
        if (kind == BAD_DIRECTORY)
            assert(mkdir(card_paths[player], 0700) == 0);
        if (kind == BAD_FIFO)
            assert(mkfifo(card_paths[player], 0600) == 0);
    }
    logs_before = log_calls;
    explicit_scan(player);
    assert(log_calls > logs_before);
    expect_frame(player, NULL, 0, 1);
    expect_frame(1u - player, other, sizeof(other), 1);
    if (kind == BAD_DIRECTORY)
        assert(rmdir(card_paths[player]) == 0);
    if (kind == BAD_FIFO)
        assert(unlink(card_paths[player]) == 0);
    write_text(card_paths[player], "00");
    explicit_scan(player);
    expect_frame(player, recovered, sizeof(recovered), 1);
}

void test_file_formats(unsigned int unused)
{
    static const unsigned char minimum[] = {0x00};
    static const unsigned char mixed[] = {0x00, 0xab, 0x12, 0xfe};
    static const unsigned char maximum[] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x80
    };
    static const char *const malformed[] = {
        "", " \t\r\n\v\f", "0", "000", "gg", "0x12", "+0", "-0",
        "000000000000000000000000000000000000",
        "00 11", "00\t11", "00\n11", "00\v11", "00\f11", "00\r11"
    };
    static const unsigned char embedded_nul[] = {'0', '0', 0, '1', '1'};
    static const unsigned char non_ascii[] = {0xa0, '0', '0'};
    unsigned char padded[257];
    size_t index;

    (void)unused;
    enabled_setup(0, "");
    expect_valid(0, "00", 2, minimum, sizeof(minimum));
    expect_valid(1, " \t\r\n\v\f00aB12fE\r\n", strlen(" \t\r\n\v\f00aB12fE\r\n"),
                 mixed, sizeof(mixed));
    expect_valid(0, "00112233445566778899aAbBcCdDeEfF80", 34, maximum, sizeof(maximum));
    memset(padded, ' ', sizeof(padded));
    padded[127] = '0';
    padded[128] = '0';
    expect_valid(1, padded, 256, minimum, sizeof(minimum));
    for (index = 0; index < sizeof(malformed) / sizeof(malformed[0]); ++index)
        expect_bad((unsigned int)(index & 1u), malformed[index],
                   strlen(malformed[index]), BAD_CONTENTS);
    expect_bad(0, embedded_nul, sizeof(embedded_nul), BAD_CONTENTS);
    expect_bad(1, non_ascii, sizeof(non_ascii), BAD_CONTENTS);
    expect_bad(0, padded, sizeof(padded), BAD_CONTENTS);
    memset(padded, '0', sizeof(padded));
    expect_bad(1, padded, sizeof(padded), BAD_CONTENTS);
    expect_bad(0, NULL, 0, BAD_MISSING);
    expect_bad(1, NULL, 0, BAD_MISSING);
    expect_bad(0, NULL, 0, BAD_DIRECTORY);
    expect_bad(1, NULL, 0, BAD_FIFO);
}

void test_no_startup_scan(unsigned int unused)
{
    static const unsigned char first[] = {0x00};
    static const unsigned char second[] = {0xff};

    (void)unused;
    setup(1, 0, "");
    map_native_card();
    write_text(card_paths[0], "00");
    write_text(card_paths[1], "FF");
    /* The retired environment override must not arm a scan or cause an exit. */
    assert(setenv("PHOENIXTOOLS_CARD_ID", "not-hex", 1) == 0);
    assert(pt_network_card_initialize() == 0);
    assert(open_calls == 0);
    expect_frame(0, NULL, 0, 1);
    expect_frame(1, NULL, 0, 1);
    explicit_scan(0);
    explicit_scan(1);
    expect_frame(0, first, sizeof(first), 1);
    expect_frame(1, second, sizeof(second), 1);
}

static void *player_thread(void *argument)
{
    unsigned int player = (unsigned int)(uintptr_t)argument;
    static const unsigned char uids[2][4] = {{0x00, 0xab, 0x12, 0xfe},
                                            {0xfe, 0x12, 0xab, 0x00}};
    unsigned int index;

    for (index = 0; index < 64; ++index) {
        pt_network_card_key(player, 0);
        pt_network_card_key(player, 1);
        pt_network_card_key(player, 1);
        expect_frame(player, uids[player], sizeof(uids[player]), 0);
        expect_frame(player, NULL, 0, 0);
    }
    return NULL;
}

void test_parallel_players(unsigned int unused)
{
    pthread_t threads[2];
    unsigned int before;

    (void)unused;
    enabled_setup(1, "");
    write_text(card_paths[0], "00AB12FE");
    write_text(card_paths[1], "fe12ab00");
    before = open_calls;
    assert(pthread_create(&threads[0], NULL, player_thread, (void *)(uintptr_t)0) == 0);
    assert(pthread_create(&threads[1], NULL, player_thread, (void *)(uintptr_t)1) == 0);
    assert(pthread_join(threads[0], NULL) == 0);
    assert(pthread_join(threads[1], NULL) == 0);
    assert(open_calls == before + 128u);
    expect_frame(0, NULL, 0, 1);
    expect_frame(1, NULL, 0, 1);
}
