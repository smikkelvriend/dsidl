/* Small stand-ins for libarchive parts DSiDL doesn't need */
#include <stdlib.h>
#include <stddef.h>
int archive_random(void *buf, size_t nbytes)
{
    unsigned char *p = buf;
    for (size_t i = 0; i < nbytes; i++)
        p[i] = (unsigned char)rand();
    return 0;
}
