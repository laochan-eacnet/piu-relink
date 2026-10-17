#pragma once

void pt_log(const char *component, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

