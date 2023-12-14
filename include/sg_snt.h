#ifndef SG_SNT_H
#define SG_SNT_H

/*
 * Copyright (c) 2023 Douglas Gilbert.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the BSD_LICENSE file.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <stdbool.h>

#include "sg_nvme.h"

#ifdef __cplusplus
extern "C" {
#endif


struct sg_snt_dev_state_t {
    bool wce;           /* Write Cache Enable (WCE) setting */
    bool wce_changed;   /* WCE setting has been changed */
    uint8_t scsi_dsense;
    uint8_t enclosure_override; /* ENC_OV in sdparm */
    uint8_t pdt;        /* 6 bit value in INQUIRY response */
    uint8_t enc_serv;   /* single bit in INQUIRY response */
    uint8_t id_ctl253;  /* NVMSR field of Identify controller (byte 253) */
    uint16_t oacs;      /* 2 byte field of Identify controller */
    uint16_t oncs;      /* 2 byte field of Identify controller */
    int vb;             /* carries verbose argument */
};

struct sg_snt_result_t {
    uint8_t sstatus;    /* SCSI Status value */
    uint8_t sk;         /* Sense Key */
    uint8_t asc;
    uint8_t ascq;
    uint16_t in_byte;   /* > 0 for Illeg Req sense key specific */
    uint8_t in_bit;     /* > 7 for 'no bit position given' */
};

#define F_SA_LOW                0x80    /* cdb byte 1, bits 4 to 0 */
#define F_SA_HIGH               0x100   /* as used by variable length cdbs */
#define FF_SA (F_SA_HIGH | F_SA_LOW)
#define F_INV_OP                0x200
#define F_NEED_TS_SUP           0x100000  /* Needs NVMe Timestamp support */

struct sg_opcode_info_t {
    int8_t doc_pdt;         /* -1 --> SPC; 0 --> SBC, 1 --> SSC, etc */
    uint8_t opcode;
    uint16_t sa;            /* service action, 0 for none */
    uint32_t flags;         /* OR-ed set of F_* flags */
    uint8_t len_mask[16];   /* len=len_mask[0], then mask for cdb[1]... */
                            /* ignore cdb bytes after position 15 */
};

/* Given the NVMe Identify Controller response and optionally the NVMe
 * Identify Namespace response (NULL otherwise), generate the SCSI VPD
 * page 0x83 (device identification) descriptor(s) in dop. Return the
 * number of bytes written which will not exceed max_do_len. Probably use
 * Peripheral Device Type (pdt) of 0 (disk) for don't know. Transport
 * protocol (tproto) should be -1 if not known, else SCSI value.
 * N.B. Does not write total VPD page length into dop[2:3] . */
int sg_make_vpd_devid_for_nvme(const uint8_t * nvme_id_ctl_p,
                               const uint8_t * nvme_id_ns_p, int pdt,
                               int tproto, uint8_t * dop, int max_do_len);

/* Initialize dev_stat pointed to by dsp */
void sg_snt_init_dev_stat(struct sg_snt_dev_state_t * dsp);

void sg_snt_mk_sense_asc_ascq(struct sg_snt_result_t * resp, int sk,
                              int asc, int ascq);
void sg_snt_mk_sense_invalid_fld(struct sg_snt_result_t * resp,
                                 bool in_cdb, int in_byte, int in_bit);

/* Internal function (common to all OSes) to support the SNTL SCSI MODE
 * SENSE(10) command. Has a vendor specific Unit Attention mpage which
 * has only one field currently: ENC_OV (enclosure override) */
int sg_snt_resp_mode_sense10(const struct sg_snt_dev_state_t * dsp,
                             const uint8_t * cdbp, uint8_t * dip,
                             int mx_di_len, struct sg_snt_result_t * resp);

/* Internal function (common to all OSes) to support the SNTL SCSI MODE
 * SELECT(10) command. */
int sg_snt_resp_mode_select10(struct sg_snt_dev_state_t * dsp,
                             const uint8_t * cdbp, const uint8_t * dop,
                             int do_len, struct sg_snt_result_t * resp);

/* Returns pointer to array of struct sg_opcode_info_t of SCSI commands
 * translated to NVMe. */
const struct sg_opcode_info_t * sg_get_opcode_translation(void);

int sg_snt_std_inq(const uint8_t * nvme_id_ctlp, uint8_t pdt, bool enc_serv,
                   uint8_t * inq_dout);

int sg_snt_resp_inq(struct sg_snt_dev_state_t * dsp, const uint8_t * cdbp,
                    const uint8_t * nvme_id_ctlp,
                    const uint8_t * nvme_id_nsp, uint8_t * dip, int mx_di_len,
                    struct sg_snt_result_t * resp);

int sg_snt_resp_rluns(struct sg_snt_dev_state_t * dsp, const uint8_t * cdbp,
                      const uint8_t * nvme_id_ctlp, uint32_t nsid,
                      uint8_t * dip, int mx_di_len,
                      struct sg_snt_result_t * resp);

int sg_snt_resp_rep_opcodes(struct sg_snt_dev_state_t * dsp,
                            const uint8_t * cdbp, uint16_t oacs,
                            uint16_t oncs, uint8_t * dip,
                            int mx_di_len, struct sg_snt_result_t * resp);

int sg_snt_resp_rep_tmfs(struct sg_snt_dev_state_t * dsp,
                         const uint8_t * cdbp, uint8_t * dip, int mx_di_len,
                         struct sg_snt_result_t * resp);

#ifdef __cplusplus
}
#endif

#endif          /* SG_SNT_H */
