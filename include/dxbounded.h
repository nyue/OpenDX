/***********************************************************************/
/* Open Visualization Data Explorer                                    */
/* Bounded string writes for fixed-size buffers.                       */
/***********************************************************************/

#ifndef DXBOUNDED_H
#define DXBOUNDED_H

#include <stddef.h>
#include <string.h>

/*
 * Size in bytes of a char array. With GCC and Clang it does not compile when
 * given a pointer, so it cannot silently become sizeof(char *).
 */
#if defined(__GNUC__)
#define DX_ARRAY_SIZE(a) \
    (sizeof(a) + 0 * sizeof(struct { int dx_array_size_needs_an_array : \
        __builtin_types_compatible_p(__typeof__(a), __typeof__(&(a)[0])) ? -1 : 1; }))
#else
#define DX_ARRAY_SIZE(a) sizeof(a)
#endif

/* strcpy into a buffer of n bytes, truncating; returns dst like strcpy. */
static inline char *_dxf_strcpy_n(char *dst, size_t n, const char *src)
{
    size_t len;

    if (n == 0)
        return dst;
    len = strlen(src);
    if (len >= n)
        len = n - 1;
    memmove(dst, src, len);
    dst[len] = '\0';
    return dst;
}

/* strcat into a buffer of n bytes, truncating; returns dst like strcat. */
static inline char *_dxf_strcat_n(char *dst, size_t n, const char *src)
{
    size_t used = strnlen(dst, n);

    if (used < n)
        _dxf_strcpy_n(dst + used, n - used, src);
    return dst;
}

#endif /* DXBOUNDED_H */
