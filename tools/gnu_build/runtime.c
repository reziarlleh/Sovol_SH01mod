#include <stddef.h>

void *memset(void *destination, int value, size_t count)
{
    unsigned char *bytes = (unsigned char *)destination;
    unsigned char fill = (unsigned char)value;

    while (count != 0u) {
        *bytes++ = fill;
        --count;
    }

    return destination;
}

void *memcpy(void *destination, const void *source, size_t count)
{
    unsigned char *destination_bytes = (unsigned char *)destination;
    const unsigned char *source_bytes = (const unsigned char *)source;

    while (count != 0u) {
        *destination_bytes++ = *source_bytes++;
        --count;
    }

    return destination;
}
