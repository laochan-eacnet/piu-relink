#pragma once
#include <windows.h>
#include "x11_abi.h"

void gfx_pump(void);
unsigned gfx_evdev(WPARAM vk, LPARAM scan_bits);
uint32_t gfx_modifiers(void);
void gfx_set_key(unsigned evdev, int pressed);
int gfx_key_down(unsigned evdev);
void gfx_load_extensions(void);
PROC gfx_extension(const char *name);
__declspec(noreturn) void gfx_missing_extension(const char *name);
void gfx_render_size(int game_width, int game_height, int output_width, int output_height);
void gfx_render_current(void);
void gfx_render_destroy(void);
void gfx_render_present(void);
void gfx_render_black(void);
int gfx_render_scaled(void);
