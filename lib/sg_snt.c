/*
 * Copyright (c) 2023 Douglas Gilbert.
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
#include <errno.h>
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
#include "sg_snt.h"

#define SAVING_PARAMS_UNSUP 0x39
#define INVALID_FIELD_IN_CDB 0x24
#define INVALID_FIELD_IN_PARAM_LIST 0x26
#define PARAMETER_LIST_LENGTH_ERR 0x1a


static const char * nvme_scsi_vendor_str = "NVMe    ";

/* Table of SCSI operation code (opcodes) supported by SNTL */
static struct sg_opcode_info_t sg_opcode_info_arr[] =
{
    {-1 /* SPC */, 0x0, 0, 0, {6,  /* TEST UNIT READY */
      0, 0, 0, 0, 0xc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    {-1, 0x3, 0, 0, {6,             /* REQUEST SENSE */
      0xe1, 0, 0, 0xff, 0xc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    {-1, 0x12, 0, 0, {6,            /* INQUIRY */
      0xe3, 0xff, 0xff, 0xff, 0xc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    {0, 0x1b, 0, 0, {6,            /* START STOP UNIT */
      0x1, 0, 0xf, 0xf7, 0xc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    {-1, 0x1c, 0, 0, {6,            /* RECEIVE DIAGNOSTIC RESULTS */
      0x1, 0xff, 0xff, 0xff, 0xc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    {-1, 0x1d, 0, 0, {6,            /* SEND DIAGNOSTIC */
      0xf7, 0x0, 0xff, 0xff, 0xc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    {0, 0x25, 0, 0, {10,            /* READ CAPACITY(10) */
      0x1, 0xff, 0xff, 0xff, 0xff, 0, 0, 0x1, 0xc7, 0, 0, 0, 0, 0, 0} },
    {0, 0x28, 0, 0, {10,            /* READ(10) */
      0xff, 0xff, 0xff, 0xff, 0xff, 0x3f, 0xff, 0xff, 0xc7, 0, 0, 0, 0,
      0, 0} },
    {0, 0x2a, 0, 0, {10,            /* WRITE(10) */
      0xfb, 0xff, 0xff, 0xff, 0xff, 0x3f, 0xff, 0xff, 0xc7, 0, 0, 0, 0,
      0, 0} },
    {0, 0x2f, 0, 0, {10,            /* VERIFY(10) */
      0xf6, 0xff, 0xff, 0xff, 0xff, 0x3f, 0xff, 0xff, 0xc7, 0, 0, 0, 0,
      0, 0} },
    {0, 0x35, 0, 0, {10,            /* SYNCHRONIZE CACHE(10) */
      0x7, 0xff, 0xff, 0xff, 0xff, 0x3f, 0xff, 0xff, 0xc7, 0, 0, 0, 0,
      0, 0} },
    {0, 0x41, 0, 0, {10,            /* WRITE SAME(10) */
      0xff, 0xff, 0xff, 0xff, 0xff, 0x3f, 0xff, 0xff, 0xc7, 0, 0, 0, 0,
      0, 0} },
    {-1, 0x55, 0, 0, {10,           /* MODE SELECT(10) */
      0x13, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xff, 0xc7, 0, 0, 0, 0, 0, 0} },
    {-1, 0x5a, 0, 0, {10,           /* MODE SENSE(10) */
      0x18, 0xff, 0xff, 0x0, 0x0, 0x0, 0xff, 0xff, 0xc7, 0, 0, 0, 0, 0, 0} },
    {0, 0x88, 0, 0, {16,            /* READ(16) */
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xc7} },
    {0, 0x8a, 0, 0, {16,            /* WRITE(16) */
      0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xc7} },
    {0, 0x8f, 0, 0, {16,            /* VERIFY(16) */
      0xf6, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0x3f, 0xc7} },
    {0, 0x91, 0, 0, {16,            /* SYNCHRONIZE CACHE(16) */
      0x7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0x3f, 0xc7} },
    {0, 0x93, 0, 0, {16,            /* WRITE SAME(16) */
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0x3f, 0xc7} },
    {0, 0x9e, 0x10, F_SA_LOW, {16,  /* READ CAPACITY(16) [service action in] */
      0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0x1, 0xc7} },
    {-1, 0xa0, 0, 0, {12,           /* REPORT LUNS */
      0xe3, 0xff, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xc7, 0, 0, 0, 0} },
    {-1, 0xa3, 0xc, F_SA_LOW, {12,  /* REPORT SUPPORTED OPERATION CODES */
      0xc, 0x87, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0xc7, 0, 0, 0,
      0} },
    {-1, 0xa3, 0xd, F_SA_LOW, {12,  /* REPORT SUPPORTED TASK MAN. FUNCTIONS */
      0xd, 0x80, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xc7, 0, 0, 0, 0} },
    {-1, 0xa3, 0xf, F_SA_LOW | F_NEED_TS_SUP, {12,  /* REPORT TIMESTAMP */
      0xf, 0x0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xc7, 0, 0, 0, 0} },
    {-1, 0xa4, 0xf, F_SA_LOW | F_NEED_TS_SUP, {12,  /* SET TIMESTAMP */
      0xf, 0x0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xc7, 0, 0, 0, 0} },

    {-127, 0xff, 0xffff, 0xffff, {0,  /* Sentinel, keep as last element */
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
};

/* Returns pointer to array of struct sg_opcode_info_t of SCSI commands
 * translated to NVMe. */
const struct sg_opcode_info_t *
sg_get_opcode_translation(void)
{
    return sg_opcode_info_arr;
}

void
sg_snt_mk_sense_asc_ascq(struct sg_snt_result_t * resp, int sk,
                         int asc, int ascq)
{
    resp->sstatus = SAM_STAT_CHECK_CONDITION;
    resp->sk = sk;
    resp->asc = asc;
    resp->ascq = ascq;
    resp->in_byte = 0;
    resp->in_bit = 255;
}

void
sg_snt_mk_sense_invalid_fld(struct sg_snt_result_t * resp, bool in_cdb,
                            int in_byte, int in_bit)
{
    resp->sstatus = SAM_STAT_CHECK_CONDITION;
    resp->sk = SPC_SK_ILLEGAL_REQUEST;
    resp->asc = in_cdb ? INVALID_FIELD_IN_CDB : INVALID_FIELD_IN_PARAM_LIST;
    resp->ascq = 0;
    resp->in_byte = in_byte;
    resp->in_bit = in_bit;
}

/* Given the NVMe Identify controller response and optionally the NVMe
 * Identify namespace response (NULL otherwise), generate the SCSI VPD
 * page 0x83 (device identification) descriptor(s) in dip. Return the
 * number of bytes written which will not exceed max_di_len. Probably use
 * Peripheral Device Type (pdt) of 0 (disk) for don't know. Transport
 * protocol (tproto) should be -1 if not known, else SCSI value.
 * N.B. Does not write total VPD page length into dip[2:3] . */
int
sg_make_vpd_devid_for_nvme(const uint8_t * nvme_id_ctl_p,
                           const uint8_t * nvme_id_ns_p, int pdt,
                           int tproto, uint8_t * dip, int max_di_len)
{
    bool have_nguid, have_eui64;
    int k, n;
    char b[4];

    if ((NULL == nvme_id_ctl_p) || (NULL == dip) || (max_di_len < 56))
        return 0;

    memset(dip, 0, max_di_len);
    dip[0] = 0x1f & pdt;  /* (PQ=0)<<5 | (PDT=pdt); 0 or 0xd (SES) */
    dip[1] = 0x83;      /* Device Identification VPD page number */
    /* Build a T10 Vendor ID based designator (desig_id=1) for controller */
    if (tproto >= 0) {
        dip[4] = ((0xf & tproto) << 4) | 0x2;
        dip[5] = 0xa1; /* PIV=1, ASSOC=2 (target device), desig_id=1 */
    } else {
        dip[4] = 0x2;  /* Prococol id=0, code_set=2 (ASCII) */
        dip[5] = 0x21; /* PIV=0, ASSOC=2 (target device), desig_id=1 */
    }
    memcpy(dip + 8, nvme_scsi_vendor_str, 8); /* N.B. this is "NVMe    " */
    memcpy(dip + 16, nvme_id_ctl_p + 24, 40);  /* MN */
    for (k = 40; k > 0; --k) {
        if (' ' == dip[15 + k])
            dip[15 + k] = '_'; /* convert trailing spaces */
        else
            break;
    }
    if (40 == k)
        --k;
    n = 16 + 1 + k;
    if (max_di_len < (n + 20))
        return 0;
    memcpy(dip + n, nvme_id_ctl_p + 4, 20); /* SN */
    for (k = 20; k > 0; --k) {  /* trim trailing spaces */
        if (' ' == dip[n + k - 1])
            dip[n + k - 1] = '\0';
        else
            break;
    }
    n += k;
    if (0 != (n % 4))
        n = ((n / 4) + 1) * 4;  /* round up to next modulo 4 */
    dip[7] = n - 8;
    if (NULL == nvme_id_ns_p)
        return n;

    /* Look for NGUID (16 byte identifier) or EUI64 (8 byte) fields in
     * NVME Identify for namespace. If found form a EUI and a SCSI string
     * descriptor for non-zero NGUID or EUI64 (prefer NGUID if both). */
    have_nguid = ! sg_all_zeros(nvme_id_ns_p + 104, 16);
    have_eui64 = ! sg_all_zeros(nvme_id_ns_p + 120, 8);
    if ((! have_nguid) && (! have_eui64))
        return n;
    if (have_nguid) {
        if (max_di_len < (n + 20))
            return n;
        dip[n + 0] = 0x1;  /* Prococol id=0, code_set=1 (binary) */
        dip[n + 1] = 0x02; /* PIV=0, ASSOC=0 (lu), desig_id=2 (eui) */
        dip[n + 3] = 16;
        memcpy(dip + n + 4, nvme_id_ns_p + 104, 16);
        n += 20;
        if (max_di_len < (n + 40))
            return n;
        dip[n + 0] = 0x3;  /* Prococol id=0, code_set=3 (utf8) */
        dip[n + 1] = 0x08; /* PIV=0, ASSOC=0 (lu), desig_id=8 (scsi string) */
        dip[n + 3] = 36;
        memcpy(dip + n + 4, "eui.", 4);
        for (k = 0; k < 16; ++k) {
            snprintf(b, sizeof(b), "%02X", nvme_id_ns_p[104 + k]);
            memcpy(dip + n + 8 + (2 * k), b, 2);
        }
        return n + 40;
    } else {    /* have_eui64 is true, 8 byte identifier */
        if (max_di_len < (n + 12))
            return n;
        dip[n + 0] = 0x1;  /* Prococol id=0, code_set=1 (binary) */
        dip[n + 1] = 0x02; /* PIV=0, ASSOC=0 (lu), desig_id=2 (eui) */
        dip[n + 3] = 8;
        memcpy(dip + n + 4, nvme_id_ns_p + 120, 8);
        n += 12;
        if (max_di_len < (n + 24))
            return n;
        dip[n + 0] = 0x3;  /* Prococol id=0, code_set=3 (utf8) */
        dip[n + 1] = 0x08; /* PIV=0, ASSOC=0 (lu), desig_id=8 (scsi string) */
        dip[n + 3] = 20;
        memcpy(dip + n + 4, "eui.", 4);
        for (k = 0; k < 8; ++k) {
            snprintf(b, sizeof(b), "%02X", nvme_id_ns_p[120 + k]);
            memcpy(dip + n + 8 + (2 * k), b, 2);
        }
        return n + 24;
    }
}

/* Disconnect-Reconnect page for mode_sense */
static int
resp_disconnect_pg(uint8_t * p, int pcontrol)
{
    uint8_t disconnect_pg[] = {0x2, 0xe, 128, 128, 0, 10, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0};

    memcpy(p, disconnect_pg, sizeof(disconnect_pg));
    if (1 == pcontrol)
        memset(p + 2, 0, sizeof(disconnect_pg) - 2);
    return sizeof(disconnect_pg);
}

static uint8_t caching_m_pg[] = {0x8, 18, 0x14, 0, 0xff, 0xff, 0, 0,
                                 0xff, 0xff, 0xff, 0xff, 0x80, 0x14, 0, 0,
                                 0, 0, 0, 0};

/* Control mode page (SBC) for mode_sense */
static int
resp_caching_m_pg(unsigned char *p, int pcontrol, bool wce)
{       /* Caching page for mode_sense */
        uint8_t ch_caching_m_pg[] = {/* 0x8, 18, */ 0x4, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t d_caching_m_pg[] = {0x8, 18, 0x14, 0, 0xff, 0xff, 0, 0,
                0xff, 0xff, 0xff, 0xff, 0x80, 0x14, 0, 0,     0, 0, 0, 0};

        if ((0 == pcontrol) || (3 == pcontrol)) {
            if (wce)
                caching_m_pg[2] |= 0x4;
            else
                caching_m_pg[2] &= ~0x4;
        }
        memcpy(p, caching_m_pg, sizeof(caching_m_pg));
        if (1 == pcontrol) {
            if (wce)
                ch_caching_m_pg[2] |= 0x4;
            else
                ch_caching_m_pg[2] &= ~0x4;
            memcpy(p + 2, ch_caching_m_pg, sizeof(ch_caching_m_pg));
        }
        else if (2 == pcontrol) {
            if (wce)
                d_caching_m_pg[2] |= 0x4;
            else
                d_caching_m_pg[2] &= ~0x4;
            memcpy(p, d_caching_m_pg, sizeof(d_caching_m_pg));
        }
        return sizeof(caching_m_pg);
}

static uint8_t ctrl_m_pg[] = {0xa, 10, 2, 0, 0, 0, 0, 0,
                              0, 0, 0x2, 0x4b};

/* Control mode page for mode_sense */
static int
resp_ctrl_m_pg(uint8_t *p, int pcontrol)
{
    uint8_t ch_ctrl_m_pg[] = {/* 0xa, 10, */ 0x6, 0, 0, 0, 0, 0,
                              0, 0, 0, 0};
    uint8_t d_ctrl_m_pg[] = {0xa, 10, 2, 0, 0, 0, 0, 0,
                             0, 0, 0x2, 0x4b};

    memcpy(p, ctrl_m_pg, sizeof(ctrl_m_pg));
    if (1 == pcontrol)
        memcpy(p + 2, ch_ctrl_m_pg, sizeof(ch_ctrl_m_pg));
    else if (2 == pcontrol)
        memcpy(p, d_ctrl_m_pg, sizeof(d_ctrl_m_pg));
    return sizeof(ctrl_m_pg);
}

static uint8_t ctrl_ext_m_pg[] = {0x4a, 0x1, 0, 0x1c,  0, 0, 0x40, 0,
                                  0, 0, 0, 0,  0, 0, 0, 0,
                                  0, 0, 0, 0,  0, 0, 0, 0,
                                  0, 0, 0, 0,  0, 0, 0, 0, };

/* Control Extension mode page [0xa,0x1] for mode_sense */
static int
resp_ctrl_ext_m_pg(uint8_t *p, int pcontrol)
{
    uint8_t ch_ctrl_ext_m_pg[] = {/* 0x4a, 0x1, 0, 0x1c, */ 0, 0, 0, 0,
                         0, 0, 0, 0,  0, 0, 0, 0,
                         0, 0, 0, 0,  0, 0, 0, 0,
                         0, 0, 0, 0,  0, 0, 0, 0, };
    uint8_t d_ctrl_ext_m_pg[] = {0x4a, 0x1, 0, 0x1c,  0, 0, 0x40, 0,
                         0, 0, 0, 0,  0, 0, 0, 0,
                         0, 0, 0, 0,  0, 0, 0, 0,
                         0, 0, 0, 0,  0, 0, 0, 0, };

    memcpy(p, ctrl_ext_m_pg, sizeof(ctrl_ext_m_pg));
    if (1 == pcontrol)
        memcpy(p + 4, ch_ctrl_ext_m_pg, sizeof(ch_ctrl_ext_m_pg));
    else if (2 == pcontrol)
        memcpy(p, d_ctrl_ext_m_pg, sizeof(d_ctrl_ext_m_pg));
    return sizeof(ctrl_ext_m_pg);
}

static uint8_t iec_m_pg[] = {0x1c, 0xa, 0x08, 0, 0, 0, 0, 0, 0, 0, 0x0, 0x0};

/* Informational Exceptions control mode page for mode_sense */
static int
resp_iec_m_pg(uint8_t *p, int pcontrol)
{
    uint8_t ch_iec_m_pg[] = {/* 0x1c, 0xa, */ 0x4, 0xf, 0, 0, 0, 0, 0, 0,
                             0x0, 0x0};
    uint8_t d_iec_m_pg[] = {0x1c, 0xa, 0x08, 0, 0, 0, 0, 0, 0, 0, 0x0, 0x0};

    memcpy(p, iec_m_pg, sizeof(iec_m_pg));
    if (1 == pcontrol)
        memcpy(p + 2, ch_iec_m_pg, sizeof(ch_iec_m_pg));
    else if (2 == pcontrol)
        memcpy(p, d_iec_m_pg, sizeof(d_iec_m_pg));
    return sizeof(iec_m_pg);
}

static uint8_t vs_ua_m_pg[] = {0x0, 0xe, 0, 0, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0};

/* Vendor specific Unit Attention mode page for mode_sense */
static int
resp_vs_ua_m_pg(uint8_t *p, int pcontrol)
{
    uint8_t ch_vs_ua_m_pg[] = {/* 0x0, 0xe, */ 0xff, 0, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t d_vs_ua_m_pg[] = {0x0, 0xe, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0};

    memcpy(p, vs_ua_m_pg, sizeof(vs_ua_m_pg));
    if (1 == pcontrol)
        memcpy(p + 2, ch_vs_ua_m_pg, sizeof(ch_vs_ua_m_pg));
    else if (2 == pcontrol)
        memcpy(p, d_vs_ua_m_pg, sizeof(d_vs_ua_m_pg));
    return sizeof(vs_ua_m_pg);
}

void
sg_snt_init_dev_stat(struct sg_snt_dev_state_t * dsp)
{
    if (dsp) {
        dsp->scsi_dsense = !! (0x4 & ctrl_m_pg[2]);
        dsp->enclosure_override = vs_ua_m_pg[2];
    }
}

static uint16_t std_inq_vers_desc[] = {
    0x00C2,             /* SAM-6 INCITS 546-2021 */
    0x05C2,             /* SPC-5 INCITS 502-2019 */
    0x1f60,             /* SNT (no version claimed) */
    UINT16_MAX,         /* end sentinel */
};

static const uint16_t inq_resp_len = 74;        /* want version descriptors */
static const uint16_t disk_vers_desc = 0x0602;  /* SBC-4 INCITS 506-2021 */
static const uint16_t ses_vers_desc = 0x0682;   /* SES-4 INCITS 555-2020 */

/* Assumes 'inq_dip' points to an array that is at least 74 bytes long
 * which will be written to. Also assume that nvme_id_ctlp points to a
 * 4096 bytes array, some of which will be read.
 * Note: Tried to use C's "arr[static 4096]" syntax but:
 *   1) it is not supported by C++
 *   2) not recommended since violation causes UB but not necessarily
 *      a compile error so it gives a false sense of security */
int
sg_snt_std_inq(const uint8_t * nvme_id_ctlp, uint8_t pdt, bool enc_serv,
               uint8_t * inq_dip)
{
    bool skip_rest = false;
    uint16_t vd;
    int k;
    char b[32];
    char bb[32];
    static const int blen = sizeof(b);
    static const int bblen = sizeof(bb);

    memset(inq_dip, 0, inq_resp_len);
    /* pdt=0 --> disk; pdt=0xd --> SES; pdt=3 --> processor (safte) */
    inq_dip[0] = (0x1f & pdt);  /* (PQ=0)<<5 */
    /* inq_dip[1] = (RMD=0)<<7 | (LU_CONG=0)<<6 | (HOT_PLUG=0)<<4; */
    inq_dip[2] = 7;   /* version: SPC-5 */
    inq_dip[3] = 2;   /* NORMACA=0, HISUP=0, response data format: 2 */
    inq_dip[4] = inq_resp_len - 5;  /* response length this_field+4+1 */
    inq_dip[6] = enc_serv ? 0x40 : 0;
    if (0x1 & nvme_id_ctlp[76]) /* is bit 0 of the ctl::CMIC field set? */
        inq_dip[6] |= 0x10;    /* then set SCSI MultiP bit */
    inq_dip[7] = 0x2;    /* CMDQUE=1 */
    memcpy(inq_dip + 8, nvme_scsi_vendor_str, 8);  /* NVMe not Intel */
    memcpy(inq_dip + 16, nvme_id_ctlp + 24, 16); /* Prod <-- MN */
    snprintf(b, blen, "%.8s", (const char *)(nvme_id_ctlp + 64));
    memcpy(inq_dip + 32,
           sg_last_n_non_blank(b, 4, bb, bblen), 4);  /* Rev <-- FR */
    for (k = 0; k < 8; ++k) {
        vd = std_inq_vers_desc[k];
        if (UINT16_MAX == vd) {
            if (PDT_SES == pdt)
                vd = ses_vers_desc;
            else if (PDT_UNKNOWN == pdt)
                break;
            else
                vd = disk_vers_desc;
            skip_rest = true;
        }
        sg_put_unaligned_be16(vd, inq_dip + 58 + (k << 1));
        if (skip_rest)
            break;
    }
    return inq_resp_len;
}

#define SG_PT_C_MAX_MSENSE_SZ 256

/* Only support MODE SENSE(10). Returns the number of bytes written to dip,
 * or -1 if error info placed in resp. */
int
sg_snt_resp_mode_sense10(const struct sg_snt_dev_state_t * dsp,
                         const uint8_t * cdbp, uint8_t * dip, int mx_di_len,
                         struct sg_snt_result_t * resp)
{
    bool dbd, llbaa, is_disk, bad_pcode;
    int pcontrol, pcode, subpcode, bd_len, alloc_len, offset, len;
    const uint32_t num_blocks = 0x100000;       /* made up */
    const uint32_t lb_size = 512;               /* guess */
    uint8_t dev_spec;
    uint8_t * ap;
    uint8_t arr[SG_PT_C_MAX_MSENSE_SZ];

    memset(resp, 0, sizeof(*resp));
    dbd = !! (cdbp[1] & 0x8);         /* disable block descriptors */
    pcontrol = (cdbp[2] & 0xc0) >> 6;
    pcode = cdbp[2] & 0x3f;
    subpcode = cdbp[3];
    llbaa = !!(cdbp[1] & 0x10);
    is_disk = sg_pdt_s_eq(sg_lib_pdt_decay(dsp->pdt), PDT_DISK_ZBC);
    if (is_disk && !dbd)
        bd_len = llbaa ? 16 : 8;
    else
        bd_len = 0;
    alloc_len = sg_get_unaligned_be16(cdbp + 7);
    memset(arr, 0, SG_PT_C_MAX_MSENSE_SZ);
    if (0x3 == pcontrol) {  /* Saving values not supported */
        sg_snt_mk_sense_asc_ascq(resp, SPC_SK_ILLEGAL_REQUEST,
                                 SAVING_PARAMS_UNSUP, 0);
        return -1;
    }
    /* for disks set DPOFUA bit and clear write protect (WP) bit */
    if (is_disk)
        dev_spec = 0x10;        /* =0x90 if WP=1 implies read-only */
    else
        dev_spec = 0x0;
    arr[3] = dev_spec;
    if (16 == bd_len)
        arr[4] = 0x1;   /* set LONGLBA bit */
    arr[7] = bd_len;        /* assume 255 or less */
    offset = 8;
    ap = arr + offset;

    if (8 == bd_len) {
        sg_put_unaligned_be32(num_blocks, ap + 0);
        sg_put_unaligned_be16((uint16_t)lb_size, ap + 6);
        offset += bd_len;
        ap = arr + offset;
    } else if (16 == bd_len) {
        sg_put_unaligned_be64(num_blocks, ap + 0);
        sg_put_unaligned_be32(lb_size, ap + 12);
        offset += bd_len;
        ap = arr + offset;
    }
    bad_pcode = false;

    switch (pcode) {
    case 0x2:       /* Disconnect-Reconnect page, all devices */
        if (0x0 == subpcode)
            len = resp_disconnect_pg(ap, pcontrol);
        else {
            len = 0;
            bad_pcode = true;
        }
        offset += len;
        break;
    case 0x8:       /* Caching Mode page, disk (like) devices */
        if (! is_disk) {
            len = 0;
            bad_pcode = true;
        } else if (0x0 == subpcode)
            len = resp_caching_m_pg(ap, pcontrol, dsp->wce);
        else {
            len = 0;
            bad_pcode = true;
        }
        offset += len;
        break;
    case 0xa:       /* Control Mode page, all devices */
        if (0x0 == subpcode)
            len = resp_ctrl_m_pg(ap, pcontrol);
        else if (0x1 == subpcode)
            len = resp_ctrl_ext_m_pg(ap, pcontrol);
        else {
            len = 0;
            bad_pcode = true;
        }
        offset += len;
        break;
    case 0x1c:      /* Informational Exceptions Mode page, all devices */
        if (0x0 == subpcode)
            len = resp_iec_m_pg(ap, pcontrol);
        else {
            len = 0;
            bad_pcode = true;
        }
        offset += len;
        break;
    case 0x3f:      /* Read all Mode pages */
        if ((0 == subpcode) || (0xff == subpcode)) {
            len = 0;
            len = resp_disconnect_pg(ap + len, pcontrol);
            if (is_disk)
                len += resp_caching_m_pg(ap + len, pcontrol, dsp->wce);
            len += resp_ctrl_m_pg(ap + len, pcontrol);
            if (0xff == subpcode)
                len += resp_ctrl_ext_m_pg(ap + len, pcontrol);
            len += resp_iec_m_pg(ap + len, pcontrol);
            len += resp_vs_ua_m_pg(ap + len, pcontrol);
            offset += len;
        } else {
            sg_snt_mk_sense_invalid_fld(resp, true, 3, 255);
            return -1;
        }
        break;
    case 0x0:       /* Vendor specific "Unit Attention" mode page */
        /* all sub-page codes ?? */
        len = resp_vs_ua_m_pg(ap, pcontrol);
        offset += len;
        break;      /* vendor is "NVMe    " (from INQUIRY field) */
    default:
        bad_pcode = true;
        break;
    }
    if (bad_pcode) {
        sg_snt_mk_sense_invalid_fld(resp, true, 2, 5);
        return -1;
    }
    sg_put_unaligned_be16(offset - 2, arr + 0);
    len = (alloc_len < offset) ? alloc_len : offset;
    len = (len < mx_di_len) ? len : mx_di_len;
    memcpy(dip, arr, len);
    return len;
}

#define SG_PT_C_MAX_MSELECT_SZ 512

/* Only support MODE SELECT(10). Returns number of bytes used from dop,
 * else -1 on error with sense code placed in resp. */
int
sg_snt_resp_mode_select10(struct sg_snt_dev_state_t * dsp,
                          const uint8_t * cdbp, const uint8_t * dop,
                          int do_len, struct sg_snt_result_t * resp)
{
    int pf, sp, ps, md_len, bd_len, off, spf, pg_len, rlen, param_len, mpage;
    int sub_mpage;
    uint8_t arr[SG_PT_C_MAX_MSELECT_SZ];

    memset(resp, 0, sizeof(*resp));
    memset(arr, 0, sizeof(arr));
    pf = cdbp[1] & 0x10;
    sp = cdbp[1] & 0x1;
    param_len = sg_get_unaligned_be16(cdbp + 7);
    if ((0 == pf) || sp || (param_len > SG_PT_C_MAX_MSELECT_SZ)) {
        int in_byte, in_bit;

        in_byte = 1;
        if (sp)
            in_bit = 0;
        else if (0 == pf)
            in_bit = 4;
        else {
            in_byte = 7;
            in_bit = 255;
        }
        sg_snt_mk_sense_invalid_fld(resp, true, in_byte, in_bit);
        return -1;
    }
    rlen = (do_len < param_len) ? do_len : param_len;
    memcpy(arr, dop, rlen);
    md_len = sg_get_unaligned_be16(arr + 0) + 2;
    bd_len = sg_get_unaligned_be16(arr + 6);
    if (md_len > 2) {
        sg_snt_mk_sense_invalid_fld(resp, false, 0, 255);
        return -1;
    }
    off = bd_len + 8;
    mpage = arr[off] & 0x3f;
    ps = !!(arr[off] & 0x80);
    if (ps) {
        sg_snt_mk_sense_invalid_fld(resp, false, off, 7);
        return -1;
    }
    spf = !!(arr[off] & 0x40);
    pg_len = spf ? (sg_get_unaligned_be16(arr + off + 2) + 4) :
                   (arr[off + 1] + 2);
    sub_mpage = spf ? arr[off + 1] : 0;
    if ((pg_len + off) > param_len) {
        sg_snt_mk_sense_asc_ascq(resp, SPC_SK_ILLEGAL_REQUEST,
                                 PARAMETER_LIST_LENGTH_ERR, 0);
        return -1;
    }
    switch (mpage) {
    case 0x8:      /* Caching Mode page */
        if (0x0 == sub_mpage) {
            if (caching_m_pg[1] == arr[off + 1]) {
                memcpy(caching_m_pg + 2, arr + off + 2,
                       sizeof(caching_m_pg) - 2);
                dsp->wce = !!(caching_m_pg[2] & 0x4);
                dsp->wce_changed = true;
                break;
            }
        }
        goto def_case;
    case 0xa:      /* Control Mode page */
        if (0x0 == sub_mpage) {
            if (ctrl_m_pg[1] == arr[off + 1]) {
                memcpy(ctrl_m_pg + 2, arr + off + 2,
                       sizeof(ctrl_m_pg) - 2);
                dsp->scsi_dsense = !!(ctrl_m_pg[2] & 0x4);
                break;
            }
        }
        goto def_case;
    case 0x1c:      /* Informational Exceptions Mode page (SBC) */
        if (0x0 == sub_mpage) {
            if (iec_m_pg[1] == arr[off + 1]) {
                memcpy(iec_m_pg + 2, arr + off + 2,
                       sizeof(iec_m_pg) - 2);
                break;
            }
        }
        goto def_case;
    case 0x0:       /* Vendor specific "Unit Attention" mode page */
        if (vs_ua_m_pg[1] == arr[off + 1]) {
            memcpy(vs_ua_m_pg + 2, arr + off + 2,
                   sizeof(vs_ua_m_pg) - 2);
            dsp->enclosure_override = vs_ua_m_pg[2];
        }
        break;
    default:
def_case:
        sg_snt_mk_sense_invalid_fld(resp, false, off, 5);
        return -1;
    }
    return rlen;
}

int
sg_snt_resp_rep_opcodes(struct sg_snt_dev_state_t * dsp, const uint8_t * cdbp,
                        uint16_t oacs, uint16_t oncs, uint8_t * dip,
                        int mx_di_len, struct sg_snt_result_t * resp)
{
    bool rctd;
    uint8_t reporting_opts, req_opcode, supp;
    uint16_t req_sa;
    uint32_t alloc_len, offset, a_len;
    uint32_t pg_sz = sg_get_page_size();
    int len, count, bump;
    int res = -1;
    const struct sg_opcode_info_t *oip;
    uint8_t *arr;
    uint8_t *free_arr;

    if (dsp->vb > 5)
        pr2ws("%s: oacs=0x%x, oncs=0x%x\n", __func__, oacs, oncs);
    rctd = !!(cdbp[2] & 0x80);      /* report command timeout desc. */
    reporting_opts = cdbp[2] & 0x7;
    req_opcode = cdbp[3];
    req_sa = sg_get_unaligned_be16(cdbp + 4);
    alloc_len = sg_get_unaligned_be32(cdbp + 6);
    if (alloc_len < 4 || alloc_len > 0xffff) {
        sg_snt_mk_sense_invalid_fld(resp, true, 6, 255);
        return -1;
    }
    a_len = pg_sz - 72;
    arr = sg_memalign(pg_sz, pg_sz, &free_arr, false);
    if (NULL == arr) {
        pr2ws("%s: calloc() failed to get memory\n", __func__);
        return sg_convert_errno(ENOMEM);
    }
    switch (reporting_opts) {
    case 0: /* all commands */
        count = 0;
        bump = rctd ? 20 : 8;
        for (offset = 4, oip = sg_get_opcode_translation();
             (oip->flags != 0xffff) && (offset < a_len); ++oip) {
            if (F_INV_OP & oip->flags)
                continue;
            ++count;
            arr[offset] = oip->opcode;
            sg_put_unaligned_be16(oip->sa, arr + offset + 2);
            if (rctd)
                arr[offset + 5] |= 0x2;
            if (FF_SA & oip->flags)
                arr[offset + 5] |= 0x1;
            sg_put_unaligned_be16(oip->len_mask[0], arr + offset + 6);
            if (rctd)
                sg_put_unaligned_be16(0xa, arr + offset + 8);
            offset += bump;
        }
        sg_put_unaligned_be32(count * bump, arr + 0);
        break;
    case 1: /* one command: opcode only */
    case 2: /* one command: opcode plus service action */
    case 3: /* one command: if sa==0 then opcode only else opcode+sa */
        for (oip = sg_get_opcode_translation(); oip->flags != 0xffff; ++oip) {
            if ((req_opcode == oip->opcode) && (req_sa == oip->sa))
                break;
        }
        if ((0xffff == oip->flags) || (F_INV_OP & oip->flags)) {
            supp = 1;
            offset = 4;
        } else {
            if (1 == reporting_opts) {
                if (FF_SA & oip->flags) {
                    sg_snt_mk_sense_invalid_fld(resp, true, 2, 2);
                    goto fini;
                }
                req_sa = 0;
            } else if ((2 == reporting_opts) && 0 == (FF_SA & oip->flags)) {
                sg_snt_mk_sense_invalid_fld(resp, true, 4, -1);
                goto fini;
            }
            if ((0 == (FF_SA & oip->flags)) && (req_opcode == oip->opcode))
                supp = 3;
            else if (0 == (FF_SA & oip->flags))
                supp = 1;
            else if (req_sa != oip->sa)
                supp = 1;
            else
                supp = 3;
            if (3 == supp) {
                uint16_t u;
                int k;

                u = oip->len_mask[0];
                sg_put_unaligned_be16(u, arr + 2);
                arr[4] = oip->opcode;
                for (k = 1; k < u; ++k)
                    arr[4 + k] = (k < 16) ?
                oip->len_mask[k] : 0xff;
                offset = 4 + u;
            } else
                offset = 4;
        }
        arr[1] = (rctd ? 0x80 : 0) | supp;
        if (rctd) {
            sg_put_unaligned_be16(0xa, arr + offset);
            offset += 12;
        }
        break;
    default:
        sg_snt_mk_sense_invalid_fld(resp, true, 2, 2);
        goto fini;
    }
    offset = (offset < a_len) ? offset : a_len;
    len = (offset < alloc_len) ? offset : alloc_len;
    len = (len < mx_di_len) ? len : mx_di_len;
    // ptp->io_hdr.din_resid = ptp->io_hdr.din_xfer_len - len;
    if (len > 0)
        memcpy(dip, arr, len);
    res = len;
fini:
    free(free_arr);
    return res;
}

int
sg_snt_resp_rep_tmfs(struct sg_snt_dev_state_t * dsp, const uint8_t * cdbp,
                     uint8_t * dip, int mx_di_len,
                     struct sg_snt_result_t * resp)
{
    bool repd;
    int len, alloc_len;
    uint8_t arr[16];

    if (dsp->vb > 5)
        pr2ws("%s: enter\n", __func__);
    memset(arr, 0, sizeof(arr));
    repd = !!(cdbp[2] & 0x80);
    alloc_len = sg_get_unaligned_be32(cdbp + 6);
    if (alloc_len < 4) {
        sg_snt_mk_sense_invalid_fld(resp, true, 6, 255);
        return -1;
    }
    arr[0] = 0xc8;          /* ATS | ATSS | LURS */
    arr[1] = 0x1;           /* ITNRS */
    if (repd) {
        arr[3] = 0xc;
        len = 16;
    } else
        len = 4;

    len = (len < alloc_len) ? len : alloc_len;
    len = (len < mx_di_len) ? len : mx_di_len;
    // ptp->io_hdr.din_resid = ptp->io_hdr.din_xfer_len - len;
    if (len > 0)
        memcpy(dip, arr, len);
    return len;
}

static const char * sg_snt_vend_s = "SG3_UTIL";         /* 8 bytes long */
static const char * sg_snt_prod_s = "SNT in sg3_utils"; /* 16 bytes long */
static const char * sg_snt_rev_s = "0100";              /* 4 bytes long */

int
sg_snt_resp_inq(struct sg_snt_dev_state_t * dsp, const uint8_t * cdbp,
                const uint8_t * nvme_id_ctlp, const uint8_t * nvme_id_nsp,
                uint8_t * dip, int mx_di_len, struct sg_snt_result_t * resp)
{
    bool evpd;
    uint16_t alloc_len, pg_cd;
    int n = 0;
    uint8_t inq_din[256];
    static const int inq_din_sz = sizeof(inq_din);

    if (0x2 & cdbp[1]) {        /* Reject CmdDt=1 */
        sg_snt_mk_sense_invalid_fld(resp, true, 1, 1);
        return -1;
    }
    alloc_len = sg_get_unaligned_be16(cdbp + 3);
    evpd = !!(0x1 & cdbp[1]);
    pg_cd = cdbp[2];
    if (evpd) {         /* VPD page responses */
        bool cp_id_ctl = false;

        memset(inq_din, 0, inq_din_sz);
        switch (pg_cd) {
        case 0:
            /* inq_din[0] = (PQ=0)<<5 | (PDT=0); prefer pdt=0xd --> SES */
            inq_din[1] = pg_cd;
            n = 12;
            sg_put_unaligned_be16(n - 4, inq_din + 2);
            inq_din[4] = 0x0;
            inq_din[5] = 0x80;
            inq_din[6] = 0x83;
            inq_din[7] = 0x86;
            inq_din[8] = 0x87;
            inq_din[9] = 0x92;
            inq_din[10] = 0xb1;
            inq_din[n - 1] = SG_NVME_VPD_NICR;     /* last VPD number */
            break;
        case 0x80:
            /* inq_din[0] = (PQ=0)<<5 | (PDT=0); prefer pdt=0xd --> SES */
            inq_din[1] = pg_cd;
            n = 24;
            sg_put_unaligned_be16(n - 4, inq_din + 2);
            memcpy(inq_din + 4, nvme_id_ctlp + 4, 20);    /* SN */
            break;
        case 0x83:
            n = sg_make_vpd_devid_for_nvme(nvme_id_ctlp, nvme_id_nsp,
                                           dsp->pdt, -1 /*tproto */,
                                           inq_din, inq_din_sz);
            if (n > 3)
                sg_put_unaligned_be16(n - 4, inq_din + 2);
            break;
        case 0x86:      /* Extended INQUIRY (per SFS SPC Discovery 2016) */
            inq_din[1] = pg_cd;
            n = 64;
            sg_put_unaligned_be16(n - 4, inq_din + 2);
            inq_din[5] = 0x1;          /* SIMPSUP=1 */
            inq_din[7] = 0x1;          /* LUICLR=1 */
            inq_din[13] = 0x40;        /* max supported sense data length */
            break;
        case 0x87:      /* Mode page policy (per SFS SPC Discovery 2016) */
            inq_din[1] = pg_cd;
            n = 8;
            sg_put_unaligned_be16(n - 4, inq_din + 2);
            inq_din[4] = 0x3f;         /* all mode pages */
            inq_din[5] = 0xff;         /*     and their sub-pages */
            inq_din[6] = 0x80;         /* MLUS=1, policy=shared */
            break;
        case 0x92:      /* SCSI Feature set: only SPC Discovery 2016 */
            inq_din[1] = pg_cd;
            n = 10;
            sg_put_unaligned_be16(n - 4, inq_din + 2);
            inq_din[9] = 0x1;  /* SFS SPC Discovery 2016 */
            break;
        case 0xb1:      /* Block Device Characteristics */
            inq_din[1] = pg_cd;
            n = 64;
            sg_put_unaligned_be16(n - 4, inq_din + 2);
            inq_din[3] = 0x3c;
            inq_din[5] = 0x01;
            break;
        case SG_NVME_VPD_NICR:  /* 0xde (vendor (sg3_utils) specific) */
            /* 16 byte page header then NVME Identify controller response */
            inq_din[1] = pg_cd;
            sg_put_unaligned_be16((64 + 4096) - 4, inq_din + 2);
            memcpy(inq_din + 8, sg_snt_vend_s, 8);
            memcpy(inq_din + 16, sg_snt_prod_s, 16);
            memcpy(inq_din + 32, sg_snt_rev_s, 4);
            n = 64 + 4096;
            cp_id_ctl = true;
            break;
        default:        /* Point to page_code field in cdb */
            sg_snt_mk_sense_invalid_fld(resp, true, 2, 7);
            return -1;
        }
        if (alloc_len > 0) {
            n = (alloc_len < n) ? alloc_len : n;
            n = (n < mx_di_len) ? n : mx_di_len;
            if (n > 0) {
                if (cp_id_ctl) {
                    memcpy(dip, inq_din, (n < 64 ? n : 64));
                    if (n > 64)
                        memcpy(dip + 64, nvme_id_ctlp, n - 64);
                } else
                    memcpy(dip, inq_din, n);
            }
        }
    } else {            /* Standard INQUIRY response */
        n = sg_snt_std_inq(nvme_id_ctlp, dsp->pdt, dsp->enc_serv, inq_din);
        if (alloc_len > 0) {
            n = (alloc_len < n) ? alloc_len : n;
            n = (n < mx_di_len) ? n : mx_di_len;
            if (n > 0)
                memcpy(dip, inq_din, n);
        }
    }
    return n;
}

int
sg_snt_resp_rluns(struct sg_snt_dev_state_t * dsp, const uint8_t * cdbp,
                  const uint8_t * nvme_id_ctlp, uint32_t nsid, uint8_t * dip,
                  int mx_di_len, struct sg_snt_result_t * resp)
{
    uint16_t sel_report, alloc_len;
    uint32_t k, num, max_nsid;
    int n = 0;
    uint8_t rl_din[256];
    uint8_t * up;
    static const int rl_din_sz = sizeof(rl_din);

    sel_report = cdbp[2];
    alloc_len = sg_get_unaligned_be32(cdbp + 6);
    max_nsid = sg_get_unaligned_le32(nvme_id_ctlp + 516);
    if (dsp->vb > 5)
        pr2ws("%s: max_nsid=%u\n", __func__, max_nsid);
    switch (sel_report) {
    case 0:
    case 2:
        num = max_nsid;
        break;
    case 1:
    case 0x10:
    case 0x12:
        num = 0;
        break;
    case 0x11:
        num = (1 == nsid) ? max_nsid :  0;
        break;
    default:
        if (dsp->vb > 1)
            pr2ws("%s: bad select_report value: 0x%x\n", __func__,
                  sel_report);
        sg_snt_mk_sense_invalid_fld(resp, true, 2, 7);
        return 0;
    }
    for (k = 0, up = rl_din + 8; k < num; ++k, up += 8) {
        if (up < (rl_din + rl_din_sz))
            sg_put_unaligned_be16(k, up);
    }
    n = num * 8;
    sg_put_unaligned_be32(n, rl_din);
    n+= 8;
    if (alloc_len > 0) {
        n = (alloc_len < n) ? alloc_len : n;
        n = (n < mx_di_len) ? n : mx_di_len;
        if (n > 0)
            memcpy(dip, rl_din, n);
    }
    return n;
}

#endif          /* (HAVE_NVME && (! IGNORE_NVME)) */
