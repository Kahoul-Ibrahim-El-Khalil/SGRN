/* strndup is POSIX — MinGW doesn't provide it.  Supply a portable
   polyfill via the xml.h redefinable macro so the library compiles
   cleanly on Windows cross-builds. */
#ifdef _WIN32
#include <stdlib.h>
#include <string.h>
static char* sgrn_strndup(const char* s, size_t n) {
    size_t len = strlen(s);
    if (len > n)
        len = n;
    char* dst = (char*)malloc(len + 1);
    if (dst) {
        memcpy(dst, s, len);
        dst[len] = '\0';
    }
    return dst;
}
#define XML_STRNDUP_FUNC sgrn_strndup
#endif

#define XML_H_IMPLEMENTATION
#include "xml.h"
