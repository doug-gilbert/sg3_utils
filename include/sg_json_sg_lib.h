#ifndef SG_JSON_SG_LIB_H
#define SG_JSON_SG_LIB_H

/*
 * Copyright (c) 2023-2026 Douglas Gilbert.
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>

#include "sg_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* These JSON support functions' implementations depend on code in
 * the rest of sg_lib.c . The bulk of JSON support functions (and
 * structures) can be found in sg_json.h which does not depend on
 * sg_lib.c . Since sg_json.h is included above, once this
 * header is included, there is no need to include sg_json.h . */


/* This function only produces JSON output if jsp is non-NULL and
 * jsp->pr_as_json is true. 'sbp' is assumed to point to sense data as
 * defined by T10 with a length of 'sb_len' bytes. Returns false if an
 * issue is detected, else it returns true. */
bool sgj_js_sense(sgj_state * jsp, sgj_opaque_p jop, const uint8_t * sbp,
                  int sb_len);

/* This function decodes one designation descriptor which starts at 'ddp'
 * and is 'dd_len' bytes long. Designation descriptors are mainly found
 * in the Device identification VPD page [0x83] but do arise in other
 * situations (e.g. SCSI ports VPD page [0x86]). This function produces
 * either human readable output (to stdout) or JSON output or both. The
 * both variant is when JSON output is selected and additionally
 * jsp->pr_out_hr is true (e.g. with the '-j=o' option). In this case,
 * human readable output is placed in a JSON array called
 * 'plain_text_output' with each line in a single array element.
 * Implementation note: the designation descriptor is parsed twice in the
 * "both' case. Each line of human readable output is prefixed by (at least)
 * leadin_sp spaces. Returns true if successful, else false. */
bool sgj_haj_designation_descriptor(sgj_state * jsp, sgj_opaque_p jop,
                                    int leadin_sp, bool pr_assoc,
                                    const uint8_t * ddp, int dd_len);

/* The in-core JSON tree is printed to 'fp' (typically stdout) by this call.
 * If jsp is NULL, jsp->pr_as_json is false or jsp->basep is NULL then this
 * function does nothing. If jsp->exit_status is true then a new JSON object
 * named "exit_status" and the 'exit_status' value rendered as a JSON integer
 * is appended to jsp->basep. The in-core JSON tree with jsp->basep as its
 * root is streamed to 'fp' which is assumed to be non-NULL.
 * Uses exit_status to call sg_lib::sg_exit2str() and then calls
 * sg_json::sgj_js2file_estr() */
void sgj_js2file(sgj_state * jsp, sgj_opaque_p jop, int exit_status,
                 FILE * fp);

#ifdef __cplusplus
}
#endif

#endif          /* SG_JSON_SG_LIB_H */
