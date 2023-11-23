/*
 * Copyright (c) 2009-2023 Douglas Gilbert.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the BSD_LICENSE file.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sg_lib.h"
#include "sg_pt.h"
#include "sg_unaligned.h"
#include "sg_pr2serr.h"
#include "sg_pr2serr.h"

#if (HAVE_NVME && (! IGNORE_NVME))
#include "sg_nvme.h"
#endif

static const char * scsi_pt_version_str = "3.21 20231123";

/* List of external functions that need to be defined for each OS are
 * listed at the top of sg_pt_dummy.c   */

const char *
scsi_pt_version()
{
    return scsi_pt_version_str;
}

const char *
sg_pt_version()
{
    return scsi_pt_version_str;
}
