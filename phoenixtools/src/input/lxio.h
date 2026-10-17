#pragma once

#include "input/bindings.h"

void pt_lxio_reset(void);
void pt_lxio_set_action(enum pt_input_action action, int pressed);
int pt_input_service_pressed(void);
int pt_input_test_pressed(void);

int phoenixtools_lxio_read(unsigned char *buffer, int capacity);
int phoenixtools_lxio_write(const unsigned char *buffer, int length);
int phoenixtools_virtual_lxio_enabled(void);
void phoenixtools_virtual_lxio_test_enable(void);

