#pragma once

#include "config/config.h"
#include "core/limits.h"
#include "core/log.h"

#include <stdio.h>
#include <sys/types.h>

#define CURLOPT_URL_VALUE 10002
#define CURLOPTTYPE_OFF_T 30000
#define CURLOPTTYPE_BLOB 40000

extern int pt_patches_installed;
extern int (*pt_real_curl_easy_setopt)(void *, int, ...);
extern FILE *(*pt_real_fopen)(const char *, const char *);
extern int (*pt_real_open)(const char *, int, ...);
extern int (*pt_real_mkdir)(const char *, mode_t);
extern void (*pt_real_glx_swap_buffers)(void *, unsigned long);
