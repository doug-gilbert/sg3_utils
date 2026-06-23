/*
 * Copyright (c) 2022-2026 Douglas Gilbert.
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#include "sg_pr2serr.h"

FILE * sg_warnings_strm = NULL;        /* would like to default to stderr */


int
pr2serr(const char * fmt, ...)
{
    va_list args;
    int n;

    va_start(args, fmt);
    n = vfprintf(stderr, fmt, args);
    va_end(args);
    return n;
}

int
pr2ws(const char * fmt, ...)
{
    va_list args;
    int n;

    va_start(args, fmt);
    n = vfprintf(sg_warnings_strm ? sg_warnings_strm : stderr, fmt, args);
    va_end(args);
    return n;
}

/* Want safe, 'n += snprintf(b + n, blen - n, ...);' pattern that can
 * be called repeatedly. However snprintf() takes an unsigned second argument
 * (size_t) that explodes if 'blen - n' goes negative. This function instead
 * uses signed integers (second argument and return value) and is safe if the
 * second argument is negative. It returns number of chars actually
 * placed in cp excluding the trailing null char. So for cp_max_len > 0 the
 * return value is always < cp_max_len; for cp_max_len <= 1 the return value
 * is 0 and no chars are written to cp. Note this means that when
 * cp_max_len = 1, this function assumes that cp[0] is the null character
 * and does nothing (and returns 0). Linux kernel has a similar function
 * called  scnprintf().  */
int
sg_scnpr(char * cp, int cp_max_len, const char * fmt, ...)
{
    va_list args;
    int n;

#ifdef DEBUG
    if (cp_max_len < 2) {
        /* stack backtrace would be good here ... */
        pr2ws("%s: buffer would overrun, 'fmt' string: %s\n", __func__, fmt);
        return 0;
    }
#else
    if (cp_max_len < 2)
        return 0;
#endif
    va_start(args, fmt);
    n = vsnprintf(cp, cp_max_len, fmt, args);
    va_end(args);
    return (n < cp_max_len) ? n : (cp_max_len - 1);
}

/* This function is similar to sg_scnpr() but takes the "n" in that pattern
 * as an extra, third argument where it is renamed 'off'. This function will
 * start writing chars at 'fcp + off' for no more than 'fcp_len - off - 1'
 * characters. The return value is the same as sg_scnpr(). */
int
sg_scn3pr(char * fcp, int fcp_len, int off, const char * fmt, ...)
{
    va_list args;
    const int cp_max_len = fcp_len - off;
    int n;

#ifdef DEBUG
    if (cp_max_len < 2) {
        /* stack backtrace would be good here ... */
        pr2ws("%s: buffer would overrun, 'fmt' string: %s\n", __func__, fmt);
        return 0;
    }
#else
    if (cp_max_len < 2)
        return 0;
#endif
    va_start(args, fmt);
    n = vsnprintf(fcp + off, fcp_len - off, fmt, args);
    va_end(args);
#ifdef DEBUG
    if (n >= cp_max_len)        /* make noise when truncation */
        pr2ws("%s: truncated [n=%d]: %s; 'fmt' string: %s\n", __func__, n,
              fcp, fmt);
#endif
    return (n < cp_max_len) ? n : (cp_max_len - 1);
}

/* Safer form of strncpy() modelled on Linux kernel strscpy() function.
 * Returns 0 if count is 1 or less (notice count is an int so that includes
 * negative values) and doesn't touch dest. Otherwise copies up to count-1
 * characters from src to dest; stopping if a null char has just been
 * copied. If count-1 copies is reached, a null character is written to src.
 * So in the case where count is 1, a null character is written to dest[0]
 * and 0 is returned. dest must be large enough to accept count characters.
 * If dest in a visible array (to the compiler) then this invocation:
 *    sg_strscpy(dest, src, sizeof(dest))
 * is safe and returns zero or a positive number. */
int
sg_strscpy(char * dest, const char * src, int count)
{
    int res = 0;

    if (count < 1)
        return res;
    /* Should there be a clamp of say 4096, 65536? Not yet */
    while (count > 1) {
        char c;

        c = src[res];
        dest[res] = c;
        if (!c)
            return res;    /* just wrote a null char */
        res++;
        count--;
    }
    /* Force NUL-termination. */
    dest[res] = '\0';
    return res;
}
