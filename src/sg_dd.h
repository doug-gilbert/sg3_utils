#ifndef SG_DD_H
#define SG_DD_H

/*
 * Copyright (c) 2026 Douglas Gilbert.
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdbool.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif


#define MAX_BPT_VALUE (1 << 24)         /* used for maximum bs as well */
#define MAX_COUNT_SKIP_SEEK (1LL << 50) /* coverity wants upper bound */

static inline bool sg_is_errno_storage_err(int unix_errno)
{
    if (unix_errno < 0)
	unix_errno = -unix_errno;
    switch (unix_errno) {
    case EIO:
#ifndef EREMOTEIO
#define EREMOTEIO EIO
#else
    case EREMOTEIO:
#endif
    case ENODATA:
	return true;
    default:
	return false;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* SG_DD_H */
