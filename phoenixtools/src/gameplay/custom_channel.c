#include "gameplay/custom_channel.h"
#include "config/config.h"
#include "phoenix/addresses.h"

#include <stdint.h>

#define PHOENIX_SONG_STRIDE 32u
#define PHOENIX_CHART_STRIDE 36u
#define PHOENIX_MAX_SONGS 100000u
#define PHOENIX_MAX_CHARTS 1000000u
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)


static int song_in_custom_range(const unsigned char *songs, uint32_t song_count,
                                int16_t song_index, const struct pt_config *config)
{
    uint32_t song_id;

    if (song_index < 0 || (uint32_t)song_index >= song_count)
        return 0;
    song_id = *(const uint32_t *)(songs + (uint32_t)song_index * PHOENIX_SONG_STRIDE);
    return song_id >= config->custom_id_min && song_id <= config->custom_id_max;
}

/* Return zero only when the adapter must replay the native NX-NXA predicate. */
__attribute__((noinline, used))
static int mark_custom_channel_charts(void)
{
    const struct pt_config *config = pt_config_get();
    unsigned char *songs;
    unsigned char *charts;
    uint32_t song_count;
    uint32_t chart_count;
    uint32_t index;

    if (!config->custom_channel)
        return 0;
    songs = *(unsigned char *volatile *)PHOENIX_SONG_RECORDS_VA;
    charts = *(unsigned char *volatile *)PHOENIX_CHART_RECORDS_VA;
    song_count = *(volatile uint32_t *)PHOENIX_SONG_COUNT_VA;
    chart_count = *(volatile uint32_t *)PHOENIX_CHART_COUNT_VA;
    if (!charts || chart_count > PHOENIX_MAX_CHARTS)
        return 1;
    if (!songs || song_count > PHOENIX_MAX_SONGS)
        song_count = 0;

    /* Chart zero is the native sentinel; change membership, not song metadata. */
    for (index = 1; index < chart_count; ++index) {
        unsigned char *chart = charts + index * PHOENIX_CHART_STRIDE;
        int16_t source = *(const int16_t *)(chart + 0x00);
        int16_t display = *(const int16_t *)(chart + 0x02);

        chart[0x0A] &= 0x7Fu;
        if (song_in_custom_range(songs, song_count, source, config) ||
            song_in_custom_range(songs, song_count, display, config))
            chart[0x0A] |= 0x80u;
    }
    return 1;
}

__attribute__((naked, noinline))
void hook_custom_channel(void)
{
    __asm__ volatile(
        "cmp $1, %eax\n\t"
        "jle 1f\n\t"
        "pushfl\n\t"
        "pushal\n\t"
        "call mark_custom_channel_charts\n\t"
        "test %eax, %eax\n\t"
        "jz 2f\n\t"
        "popal\n\t"
        "popfl\n\t"
        /* Keep native cleanup, restrictions, grouping and selector bookkeeping. */
        "push $" STRINGIFY(PHOENIX_CHANNEL_CLEANUP_ADDR) "\n\t"
        "ret\n"
        "1:\n\t"
        "push $" STRINGIFY(PHOENIX_CHANNEL_EMPTY_ADDR) "\n\t"
        "ret\n"
        "2:\n\t"
        "popal\n\t"
        "popfl\n\t"
        "push $" STRINGIFY(PHOENIX_CHANNEL_CASE_12_NATIVE_ADDR) "\n\t"
        "ret\n\t");
}
