#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void pt_log(const char *component, const char *format, ...)
{
    char buffer[1024];
    va_list arguments;
    int prefix_length;
    int body_length;
    size_t length;

    prefix_length = snprintf(buffer, sizeof(buffer), "[phoenixtools:%s] ",
                             component ? component : "log");
    if (prefix_length < 0 || (size_t)prefix_length >= sizeof(buffer))
        return;

    va_start(arguments, format);
    body_length = vsnprintf(buffer + prefix_length,
                            sizeof(buffer) - (size_t)prefix_length,
                            format, arguments);
    va_end(arguments);
    if (body_length < 0)
        return;

    length = strnlen(buffer, sizeof(buffer));
    if (length + 1u < sizeof(buffer))
        buffer[length++] = '\n';
    if (write(STDERR_FILENO, buffer, length) < 0)
        return;
}
