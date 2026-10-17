#pragma once

void pt_sandbox_initialize(void);
int pt_network_private_base(const char *base);
void hook_update_state(void);
void hook_update_entry(void);
void hook_disk_id_entry(void);
int hook_lock_response(void *context, unsigned char *response);
void hook_port_io_probe(void);
void hook_mixer_helper(void);
void hook_glx_swap_buffers(void *display, unsigned long drawable);
