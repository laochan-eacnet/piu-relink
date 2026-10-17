#pragma once
#include "lxrt.h"
#include "lxlocale.h"

/* lxrt owns FILE storage; no host FILE or host wchar_t crosses this interface. */
typedef struct lx_FILE lx_FILE;
extern lx_FILE *lx_stdin_storage, *lx_stdout_storage, *lx_stderr_storage;
/* R_386_COPY moves the live pointer variable into the executable. */
#define lx_stdin (*(lx_FILE **)lx_data_addr(&lx_stdin_storage))
#define lx_stdout (*(lx_FILE **)lx_data_addr(&lx_stdout_storage))
#define lx_stderr (*(lx_FILE **)lx_data_addr(&lx_stderr_storage))
void lx_flockfile(lx_FILE *f);
void lx_funlockfile(lx_FILE *f);
int lx_fgetc_unlocked(lx_FILE *f);
int lx_ungetc_unlocked(int c, lx_FILE *f);
size_t lx_fwrite_unlocked(const void *p, size_t size, size_t count, lx_FILE *f);
int lx_ferror_unlocked(lx_FILE *f);
int lx_fputc_unlocked(int c, lx_FILE *f);
uint32_t lx_fgetwc_unlocked(lx_FILE *f);
uint32_t lx_fputwc_unlocked(uint32_t c, lx_FILE *f);
