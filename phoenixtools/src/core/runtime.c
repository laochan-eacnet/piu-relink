#include "core/runtime.h"

int pt_patches_installed;
int (*pt_real_curl_easy_setopt)(void *, int, ...);
FILE *(*pt_real_fopen)(const char *, const char *);
int (*pt_real_open)(const char *, int, ...);
int (*pt_real_mkdir)(const char *, mode_t);
void (*pt_real_glx_swap_buffers)(void *, unsigned long);
