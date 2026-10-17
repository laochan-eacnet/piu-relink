#include "gameplay/catalog.h"
#include "config/config.h"
#include "core/log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PHOENIX_SELECTABLE_CHARTS (*(volatile uint32_t **)0x09774B04u)
#define PHOENIX_SONG_COUNT (*(volatile uint32_t *)0x09774B08u)
#define PHOENIX_CHART_COUNT (*(volatile uint32_t *)0x09774B0Cu)
#define PHOENIX_SELECTABLE_COUNT (*(volatile uint32_t *)0x09774B30u)
#define PHOENIX_SONG_RECORDS (*(volatile unsigned char **)0x09774B10u)
#define PHOENIX_CHART_RECORDS (*(volatile unsigned char **)0x09774B14u)
#define PHOENIX_DEMO_BEGIN (*(volatile uint32_t **)0x09774B50u)
#define PHOENIX_DEMO_END (*(volatile uint32_t **)0x09774B54u)
#define PHOENIX_RESTRICT_BASIC_BEGIN (*(volatile uint32_t **)0x09774B5Cu)
#define PHOENIX_RESTRICT_BASIC_END (*(volatile uint32_t **)0x09774B60u)
#define PHOENIX_RESTRICT_BASIC_CAPACITY (*(volatile uint32_t **)0x09774B64u)
#define PHOENIX_RESTRICT_FULL_BEGIN (*(volatile uint32_t **)0x09774B68u)
#define PHOENIX_RESTRICT_FULL_END (*(volatile uint32_t **)0x09774B6Cu)
#define PHOENIX_RESTRICT_FULL_CAPACITY (*(volatile uint32_t **)0x09774B70u)
#define PHOENIX_SONG_STRIDE 32u
#define PHOENIX_CHART_STRIDE 36u
#define PHOENIX_ORIGINAL_MARKET_FINALIZE ((void (*)(void))0x080FEC7Au)
#define PHOENIX_MAX_SONGS 100000u
#define PHOENIX_MAX_CHARTS 1000000u

static uint32_t *restricted_basic;
static uint32_t *restricted_full;
static int policy_applied;

static int song_resources_present(uint32_t song_id)
{
    const struct pt_config *config = pt_config_get();
    const char *roots[2] = {config->overlay_root, config->plain_root};
    char path[4096];
    struct stat st;
    unsigned int index;

    for (index = 0; index < 2; ++index) {
        if (!roots[index] || !*roots[index])
            continue;
        if (snprintf(path, sizeof(path), "%s/D/%X", roots[index], song_id) < 0 ||
            strlen(path) >= sizeof(path))
            continue;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 1;
    }
    return 0;
}

static uint32_t *filter_restricted_list(volatile uint32_t *begin,
                                        volatile uint32_t *end,
                                        uint32_t *kept,
                                        uint32_t *removed)
{
    uint32_t count;
    uint32_t *filtered;
    uint32_t used = 0;
    uint32_t index;

    *kept = 0;
    *removed = 0;
    if ((!begin || !end) && begin != end)
        return NULL;
    if (begin && (end < begin ||
                  (uintptr_t)(end - begin) > PHOENIX_MAX_SONGS))
        return NULL;
    count = begin ? (uint32_t)(end - begin) : 0u;
    filtered = malloc((size_t)(count ? count : 1u) * sizeof(*filtered));
    if (!filtered)
        return NULL;
    for (index = 0; index < count; ++index) {
        uint32_t song_id = begin[index];
        if (song_id && song_resources_present(song_id)) {
            ++*removed;
            continue;
        }
        filtered[used++] = song_id;
    }
    *kept = used;
    return filtered;
}

static void apply_song_policy(void)
{
    unsigned char *songs = (unsigned char *)PHOENIX_SONG_RECORDS;
    uint32_t song_count = PHOENIX_SONG_COUNT;
    uint32_t basic_kept;
    uint32_t basic_removed;
    uint32_t full_kept;
    uint32_t full_removed;
    uint32_t local = 0;
    uint32_t missing = 0;
    uint32_t index;

    if ((!pt_config_get()->unlock_all_songs && !pt_config_get()->guest_full_mode) ||
        !songs || !song_count || song_count > PHOENIX_MAX_SONGS)
        return;

    restricted_basic = filter_restricted_list(PHOENIX_RESTRICT_BASIC_BEGIN,
                                               PHOENIX_RESTRICT_BASIC_END,
                                               &basic_kept, &basic_removed);
    restricted_full = filter_restricted_list(PHOENIX_RESTRICT_FULL_BEGIN,
                                              PHOENIX_RESTRICT_FULL_END,
                                              &full_kept, &full_removed);
    if (!restricted_basic || !restricted_full) {
        pt_log("gameplay", "could not allocate filtered restriction lists");
        return;
    }
    PHOENIX_RESTRICT_BASIC_BEGIN = restricted_basic;
    PHOENIX_RESTRICT_BASIC_END = restricted_basic + basic_kept;
    PHOENIX_RESTRICT_BASIC_CAPACITY = restricted_basic + basic_kept;
    PHOENIX_RESTRICT_FULL_BEGIN = restricted_full;
    PHOENIX_RESTRICT_FULL_END = restricted_full + full_kept;
    PHOENIX_RESTRICT_FULL_CAPACITY = restricted_full + full_kept;

    for (index = 0; index < song_count; ++index) {
        unsigned char *song = songs + index * PHOENIX_SONG_STRIDE;
        uint32_t song_id = *(uint32_t *)song;
        if (!song_id)
            continue;
        if (song_resources_present(song_id)) {
            song[0x17] &= 0x7fu;
            ++local;
        } else {
            ++missing;
        }
    }
    pt_log("gameplay",
             "offline local songs=%u/%u missing=%u; restrictions removed basic=%u full=%u, retained basic=%u full=%u; DEMO_SONG preserved=%td",
             local, song_count, missing, basic_removed, full_removed,
             basic_kept, full_kept,
             PHOENIX_DEMO_BEGIN && PHOENIX_DEMO_END ? PHOENIX_DEMO_END - PHOENIX_DEMO_BEGIN : 0);
}

static void apply_chart_policy(void)
{
    unsigned char *songs = (unsigned char *)PHOENIX_SONG_RECORDS;
    unsigned char *charts = (unsigned char *)PHOENIX_CHART_RECORDS;
    uint32_t song_count = PHOENIX_SONG_COUNT;
    uint32_t chart_count = PHOENIX_CHART_COUNT;
    uint32_t unhidden = 0;
    uint32_t index;

    if (!pt_config_get()->unlock_all_charts || !songs || !charts ||
        song_count > PHOENIX_MAX_SONGS || chart_count > PHOENIX_MAX_CHARTS)
        return;
    for (index = 1; index < chart_count; ++index) {
        unsigned char *chart = charts + index * PHOENIX_CHART_STRIDE;
        int16_t song_index = *(int16_t *)(chart + 2);
        uint32_t song_id;

        if (song_index < 0 || (uint32_t)song_index >= song_count)
            continue;
        song_id = *(uint32_t *)(songs + (uint32_t)song_index * PHOENIX_SONG_STRIDE);
        if (!song_id || !song_resources_present(song_id))
            continue;
        chart[10] |= 0x80u;
        ++unhidden;
    }
    pt_log("gameplay", "unhidden %u chart records backed by local songs", unhidden);
}

void pt_gameplay_log_catalog_state(const char *stage)
{
    volatile uint32_t *table = PHOENIX_SELECTABLE_CHARTS;
    uint32_t count = PHOENIX_SELECTABLE_COUNT;
    uint32_t first = table && count ? table[0] : 0u;
    uint32_t last = table && count ? table[count - 1u] : 0u;

    pt_log("catalog-state",
             "%s mode=%u profile=%u songs=%u charts=%u selectable=%p count=%u first=%u last=%u demo=%td restrict-basic=%td restrict-full=%td",
             stage ? stage : "(null)",
             *(volatile uint8_t *)0x0979E7BCu,
             *(volatile uint8_t *)0x0979E7BDu,
             PHOENIX_SONG_COUNT, PHOENIX_CHART_COUNT,
             (void *)table, count, first, last,
             PHOENIX_DEMO_BEGIN && PHOENIX_DEMO_END ? PHOENIX_DEMO_END - PHOENIX_DEMO_BEGIN : 0,
             PHOENIX_RESTRICT_BASIC_BEGIN && PHOENIX_RESTRICT_BASIC_END ?
                 PHOENIX_RESTRICT_BASIC_END - PHOENIX_RESTRICT_BASIC_BEGIN : 0,
             PHOENIX_RESTRICT_FULL_BEGIN && PHOENIX_RESTRICT_FULL_END ?
                 PHOENIX_RESTRICT_FULL_END - PHOENIX_RESTRICT_FULL_BEGIN : 0);
}

void hook_gameplay_finalize(void)
{
    PHOENIX_ORIGINAL_MARKET_FINALIZE();
    pt_gameplay_log_catalog_state("after-original-finalize");
    if (policy_applied)
        return;
    policy_applied = 1;
    apply_song_policy();
    apply_chart_policy();
    pt_gameplay_log_catalog_state("after-offline-policy");
}
