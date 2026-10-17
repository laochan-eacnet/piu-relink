#pragma once

#include "config/config.h"
#include "core/runtime.h"
#include "input/bindings.h"
#include "input/input.h"
#include "input/lxio.h"
#include "phoenix/patch_engine.h"
#include "safety/network_card.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define KEY_PRESS 2
#define KEY_RELEASE 3
#define CODE_BASE 0x0807e000u
#define CARD_BOUNDARY 0x0807e671u

/* External native keyboard-consumer ABI, not private reader state. */
struct native_binding {
    uint32_t mode, reserved, key_index, action_id, state, timestamp;
};

extern char root[PATH_MAX];
extern char card_paths[2][PATH_MAX];
extern void *display;
extern unsigned int open_calls, fopen_calls;
extern unsigned int conversion_calls, card_name_calls, lookup_calls, log_calls;
extern unsigned int lxio_calls, lxio_resets;
extern const unsigned char supported_boundary[11];

int __real_open(const char *, int, ...);
FILE *__real_fopen(const char *, const char *);
FILE *__wrap_fopen(const char *, const char *);

void write_bytes(const char *, const void *, size_t);
void write_text(const char *, const char *);
void load_config(int cards, int lxio, const char *bindings);
void setup(int cards, int lxio, const char *bindings);
void cleanup(void);
void enabled_setup(int lxio, const char *bindings);
void explicit_scan(unsigned int player);

unsigned int keycode(const char *name);
void event_key(const char *name, int type, int consumed);
void lookup_key(const char *name, int consumed);
void check_keymap(int cards, int lxio);
void check_cabinet_key(int lxio);

uint32_t *native_count(void);
struct native_binding *native_bindings(void);
void map_native_inputs(void);
void map_native_card(void);
int native_has_key(unsigned int code);
void check_native_install(int lxio);
void expect_frame(unsigned int player, const unsigned char *uid,
                  size_t length, int trampoline);

void test_file_formats(unsigned int);
void test_parallel_players(unsigned int);
void test_no_startup_scan(unsigned int);
void test_patch_mismatch(unsigned int);
void test_unmapped_patch(unsigned int);
