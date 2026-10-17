#include "input/lxio.h"

#include "config/config.h"
#include "core/log.h"

#include <string.h>

#define LXIO_REPORT_SIZE 16
#define LXIO_P1_FIRST 0
#define LXIO_P1_LAST 3
#define LXIO_P2_FIRST 4
#define LXIO_P2_LAST 7
#define LXIO_CABINET 8
#define LXIO_SERVICE_BIT 6u
#define LXIO_TEST_BIT 1u
#define LXIO_COIN1_BIT 2u

static volatile unsigned char input_report[LXIO_REPORT_SIZE];
static volatile unsigned char output_report[LXIO_REPORT_SIZE];
static volatile int service_pressed;
static volatile int test_pressed;
static int force_virtual_for_test;

static void set_active_low_bit(volatile unsigned char *value,
                               unsigned int bit, int pressed)
{
    unsigned char mask = (unsigned char)(1u << bit);
    unsigned char current = *value;

    *value = pressed ? (unsigned char)(current & (unsigned char)~mask)
                     : (unsigned char)(current | mask);
}

void pt_lxio_reset(void)
{
    memset((void *)input_report, 0xff, sizeof(input_report));
    memset((void *)output_report, 0, sizeof(output_report));
    service_pressed = 0;
    test_pressed = 0;
}

void pt_lxio_set_action(enum pt_input_action action, int pressed)
{
    if (action <= PT_INPUT_LOWER_RIGHT) {
        int byte_index;
        for (byte_index = LXIO_P1_FIRST; byte_index <= LXIO_P1_LAST; ++byte_index)
            set_active_low_bit(&input_report[byte_index], (unsigned int)action,
                               pressed);
        return;
    }
    if (action >= PT_INPUT_P2_UPPER_LEFT &&
        action <= PT_INPUT_P2_LOWER_RIGHT) {
        int byte_index;
        unsigned int panel_bit =
            (unsigned int)(action - PT_INPUT_P2_UPPER_LEFT);
        for (byte_index = LXIO_P2_FIRST; byte_index <= LXIO_P2_LAST; ++byte_index)
            set_active_low_bit(&input_report[byte_index], panel_bit, pressed);
        return;
    }
    if (action == PT_INPUT_SERVICE) {
        set_active_low_bit(&input_report[LXIO_CABINET], LXIO_SERVICE_BIT, pressed);
        service_pressed = pressed;
    } else if (action == PT_INPUT_TEST) {
        set_active_low_bit(&input_report[LXIO_CABINET], LXIO_TEST_BIT, pressed);
        test_pressed = pressed;
    } else if (action == PT_INPUT_COIN1) {
        set_active_low_bit(&input_report[LXIO_CABINET], LXIO_COIN1_BIT, pressed);
    }
    if (pt_config_verbose())
        pt_log("input", "%s %s", pt_input_action_name(action),
               pressed ? "pressed" : "released");
}

int phoenixtools_lxio_read(unsigned char *buffer, int capacity)
{
    int index;
    int count = capacity < LXIO_REPORT_SIZE ? capacity : LXIO_REPORT_SIZE;

    if (!buffer || capacity <= 0)
        return 0;
    for (index = 0; index < count; ++index)
        buffer[index] = input_report[index];
    return count;
}

int phoenixtools_lxio_write(const unsigned char *buffer, int length)
{
    int index;
    int count = length < LXIO_REPORT_SIZE ? length : LXIO_REPORT_SIZE;

    if (!buffer || length <= 0)
        return 0;
    for (index = 0; index < count; ++index)
        output_report[index] = buffer[index];
    return count;
}

int pt_input_service_pressed(void)
{
    return service_pressed;
}

int pt_input_test_pressed(void)
{
    return test_pressed;
}

int phoenixtools_virtual_lxio_enabled(void)
{
    return pt_config_get()->virtual_lxio || force_virtual_for_test;
}

void phoenixtools_virtual_lxio_test_enable(void)
{
    force_virtual_for_test = 1;
    pt_lxio_reset();
}
