#pragma once

int pt_virtual_lxio_enabled(void);
int pt_virtual_lxio_read(unsigned char *data, int length);
int pt_virtual_lxio_write(const unsigned char *data, int length);
void *pt_virtual_lxio_device(void);
void *pt_virtual_lxio_handle(void);

