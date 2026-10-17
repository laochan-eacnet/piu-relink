#include "config/config_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const REGION_NAMES[] = {
    "International", "Korea", "China", "Latin America", "Japan",
    "Indonesia", "Thailand", "Philippines", "Europe", "MiddleEast", "USA"
};

struct pt_config pt_config_state;

void pt_config_normalize_name(const char *input, char *output, size_t capacity)
{
    size_t used = 0;

    while (*input && used + 1u < capacity) {
        unsigned char character = (unsigned char)*input++;
        if (isalnum(character))
            output[used++] = (char)tolower(character);
    }
    output[used] = '\0';
}

int pt_config_parse_region(const char *value, unsigned int *region)
{
    char normalized[64];
    char candidate[64];
    char *end = NULL;
    unsigned long numeric;
    size_t index;

    if (!value || !*value || !region)
        return -1;
    errno = 0;
    numeric = strtoul(value, &end, 10);
    if (!errno && end && *end == '\0') {
        if (numeric >= sizeof(REGION_NAMES) / sizeof(REGION_NAMES[0]))
            return -1;
        *region = (unsigned int)numeric;
        return 0;
    }
    pt_config_normalize_name(value, normalized, sizeof(normalized));
    for (index = 0; index < sizeof(REGION_NAMES) / sizeof(REGION_NAMES[0]); ++index) {
        pt_config_normalize_name(REGION_NAMES[index], candidate, sizeof(candidate));
        if (!strcmp(normalized, candidate)) {
            *region = (unsigned int)index;
            return 0;
        }
    }
    return -1;
}

int pt_config_parse_boolean(const char *value, int *result)
{
    if (!value || !result)
        return -1;
    if (!strcasecmp(value, "true") || !strcasecmp(value, "yes") ||
        !strcasecmp(value, "on") || !strcmp(value, "1")) {
        *result = 1;
        return 0;
    }
    if (!strcasecmp(value, "false") || !strcasecmp(value, "no") ||
        !strcasecmp(value, "off") || !strcmp(value, "0")) {
        *result = 0;
        return 0;
    }
    return -1;
}

int pt_config_parse_song_id(const char *value, uint32_t *result)
{
    uint32_t parsed = 0;
    unsigned int base = 10;

    if (!value || !result)
        return -1;
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        value += 2;
    }
    if (!*value)
        return -1;
    while (*value) {
        unsigned char character = (unsigned char)*value++;
        unsigned int digit;

        if (character >= '0' && character <= '9')
            digit = character - '0';
        else if (character >= 'a' && character <= 'f')
            digit = character - 'a' + 10u;
        else if (character >= 'A' && character <= 'F')
            digit = character - 'A' + 10u;
        else
            return -1;
        if (digit >= base || parsed > (UINT32_MAX - digit) / base)
            return -1;
        parsed = parsed * base + digit;
    }
    *result = parsed;
    return 0;
}

int pt_config_parse_port(const char *value, uint16_t *result)
{
    char *end;
    unsigned long port;

    if (!value || !*value || *value == '-' || !result)
        return -1;
    errno = 0;
    port = strtoul(value, &end, 10);
    if (errno || *end || port < 1 || port > 65535)
        return -1;
    *result = (uint16_t)port;
    return 0;
}

int pt_config_copy_value(char *destination, size_t capacity, const char *value)
{
    size_t length;

    if (!destination || !capacity || !value)
        return -1;
    length = strlen(value);
    if (length >= 2u && ((value[0] == '"' && value[length - 1u] == '"') ||
                         (value[0] == '\'' && value[length - 1u] == '\''))) {
        ++value;
        length -= 2u;
    }
    if (!length || length >= capacity)
        return -1;
    memcpy(destination, value, length);
    destination[length] = '\0';
    return 0;
}

void pt_config_set_defaults(void)
{
    struct pt_input_bindings *input = &pt_config_state.input;

    memset(&pt_config_state, 0, sizeof(pt_config_state));
    pt_config_state.unlock_all_songs = 1;
    pt_config_state.unlock_all_charts = 1;
    pt_config_state.guest_full_mode = 1;
    pt_config_state.custom_id_min = PT_CUSTOM_ID_MIN_DEFAULT;
    pt_config_state.custom_id_max = PT_CUSTOM_ID_MAX_DEFAULT;
    pt_config_state.virtual_lxio = 1;
    pt_config_state.game_port = 15040;
    (void)pt_config_copy_value(input->upper_left, sizeof(input->upper_left), "Q");
    (void)pt_config_copy_value(input->upper_right, sizeof(input->upper_right), "E");
    (void)pt_config_copy_value(input->center, sizeof(input->center), "S");
    (void)pt_config_copy_value(input->lower_left, sizeof(input->lower_left), "Z");
    (void)pt_config_copy_value(input->lower_right, sizeof(input->lower_right), "C");
    (void)pt_config_copy_value(input->p2_upper_left, sizeof(input->p2_upper_left), "U");
    (void)pt_config_copy_value(input->p2_upper_right, sizeof(input->p2_upper_right), "O");
    (void)pt_config_copy_value(input->p2_center, sizeof(input->p2_center), "K");
    (void)pt_config_copy_value(input->p2_lower_left, sizeof(input->p2_lower_left), "M");
    (void)pt_config_copy_value(input->p2_lower_right, sizeof(input->p2_lower_right), "period");
    (void)pt_config_copy_value(input->service, sizeof(input->service), "F1");
    (void)pt_config_copy_value(input->test, sizeof(input->test), "F2");
    (void)pt_config_copy_value(input->coin1, sizeof(input->coin1), "5");
    (void)pt_config_copy_value(input->card1, sizeof(input->card1), "F3");
    (void)pt_config_copy_value(input->card2, sizeof(input->card2), "F4");
}

const struct pt_config *pt_config_get(void)
{
    return &pt_config_state;
}

const char *pt_region_name(unsigned int region_slot)
{
    return region_slot < sizeof(REGION_NAMES) / sizeof(REGION_NAMES[0]) ?
           REGION_NAMES[region_slot] : "Unknown";
}

int pt_config_verbose(void)
{
    return pt_config_state.verbose;
}

int phoenixtools_parse_region_test(const char *value)
{
    unsigned int region;
    return pt_config_parse_region(value, &region) == 0 ? (int)region : -1;
}

int phoenixtools_config_region_test(void)
{
    return (int)pt_config_state.region_slot;
}
