/* A utility program originally written for the Linux OS SCSI subsystem.
 * Copyright (C) 2000-2023 D. Gilbert
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program outputs information provided by a SCSI INQUIRY command.
 * It is mainly based on the SCSI SPC-6 document at https://www.t10.org .
 *
 * Acknowledgment:
 *    - Martin Schwenke <martin at meltin dot net> added the raw switch and
 *      other improvements [20020814]
 *    - Lars Marowsky-Bree <lmb at suse dot de> contributed Unit Path Report
 *      VPD page decoding for EMC CLARiiON devices [20041016]
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <getopt.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>
#include <errno.h>

#ifdef SG_LIB_LINUX
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/hdreg.h>
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sg_lib.h"
#include "sg_lib_data.h"
#include "sg_lib_names.h"
#include "sg_cmds_basic.h"
#include "sg_pt.h"
#include "sg_unaligned.h"
#include "sg_pr2serr.h"
#if (HAVE_NVME && (! IGNORE_NVME))
#include "sg_nvme.h"
#include "sg_snt.h"
#endif

#include "sg_vpd_common.h"  /* for shared VPD page processing with sg_vpd */

static const char * version_str = "2.58 20231213";  /* spc6r11, sbc5r06 */

#define MY_NAME "sg_inq"

/* INQUIRY notes:
 * It is recommended that the initial allocation length given to a
 * standard INQUIRY is 36 (bytes), especially if this is the first
 * SCSI command sent to a logical unit. This is compliant with SCSI-2
 * and another major operating system. There are devices out there
 * that use one of the SCSI commands sets and lock up if they receive
 * an allocation length other than 36. This technique is sometimes
 * referred to as a "36 byte INQUIRY".
 *
 * A "standard" INQUIRY is one that has the EVPD and the CmdDt bits
 * clear.
 *
 * When doing device discovery on a SCSI transport (e.g. bus scanning)
 * the first SCSI command sent to a device should be a standard (36
 * byte) INQUIRY.
 *
 * The allocation length field in the INQUIRY command was changed
 * from 1 to 2 bytes in SPC-3, revision 9, 17 September 2002.
 * Be careful using allocation lengths greater than 252 bytes, especially
 * if the lower byte is 0x0 (e.g. a 512 byte allocation length may
 * not be a good arbitrary choice (as 512 == 0x200) ).
 *
 * From SPC-3 revision 16 the CmdDt bit in an INQUIRY is obsolete. There
 * is now a REPORT SUPPORTED OPERATION CODES command that yields similar
 * information [MAINTENANCE IN, service action = 0xc]; see sg_opcodes.
 */

// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< TESTING
// #undef SG_SCSI_STRINGS
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< TESTING

#define VPD_NOPE_WANT_STD_INQ -2        /* request for standard inquiry */

/* Vendor specific VPD pages (typically >= 0xc0) */
#define VPD_UPR_EMC 0xc0
#define VPD_RDAC_VERS 0xc2
#define VPD_RDAC_VAC 0xc9

/* values for selection one or more associations (2**vpd_assoc),
   except _AS_IS */
#define VPD_DI_SEL_LU 1
#define VPD_DI_SEL_TPORT 2
#define VPD_DI_SEL_TARGET 4
#define VPD_DI_SEL_AS_IS 32

#define DEF_ALLOC_LEN 252       /* highest 1 byte value that is modulo 4 */
#define MX_ALLOC_LEN (0xc000 + 0x80)
#define VPD_ATA_INFO_LEN  572

#define SENSE_BUFF_LEN  64       /* Arbitrary, could be larger */
#define INQUIRY_CMD     0x12
#define INQUIRY_CMDLEN  6
#define DEF_PT_TIMEOUT  60       /* 60 seconds */


uint8_t * rsp_buff;

static uint8_t * free_rsp_buff;
static const int rsp_buff_sz = MX_ALLOC_LEN + 1;

static char xtra_buff[MX_ALLOC_LEN + 1];
static char usn_buff[MX_ALLOC_LEN + 1];

static void decode_dev_ids(const char * leadin, uint8_t * buff, int len,
                           struct opts_t * op, sgj_opaque_p jop);
static int vpd_decode(struct sg_pt_base * ptvp, struct opts_t * op,
                      sgj_opaque_p jop, int off);

// Test define that will only work for Linux
// #define HDIO_GET_IDENTITY 1

#if defined(SG_LIB_LINUX) && defined(SG_SCSI_STRINGS) && \
    defined(HDIO_GET_IDENTITY)
#include <sys/ioctl.h>

static int try_ata_identify(int ata_fd, int do_hex, int do_raw,
                            int verbose);
static void prepare_ata_identify(const struct opts_t * op, int inhex_len);
#endif


/* Note that this table is sorted by acronym */
static const struct svpd_values_name_t t10_vpd_pg[] = {
    {VPD_AUTOMATION_DEV_SN, 0, 1, "adsn", "Automation device serial "
     "number (SSC)"},
    {VPD_ATA_INFO, 0, -1, "ai", "ATA information (SAT)"},
    {VPD_BLOCK_DEV_CHARS, 0, 0, "bdc",
     "Block device characteristics (SBC)"},
    {VPD_BLOCK_DEV_C_EXTENS, 0, 0, "bdce", "Block device characteristics "
     "extension (SBC)"},
    {VPD_BLOCK_LIMITS, 0, 0, "bl", "Block limits (SBC)"},
    {VPD_BLOCK_LIMITS_EXT, 0, 0, "ble", "Block limits extension (SBC)"},
    {VPD_CAP_PROD_ID, 0, 0, "cap", "Capacity/Product identification mapping"},
    {VPD_CFA_PROFILE_INFO, 0, 0, "cfa", "CFA profile information"},
    {VPD_CON_POS_RANGE, 0, 0, "cpr", "Concurrent positioning ranges "
     "(SBC)"},
    {VPD_DEVICE_CONSTITUENTS, 0, -1, "dc", "Device constituents"},
    {VPD_DEVICE_ID, 0, -1, "di", "Device identification"},
    {VPD_DTDE_ADDRESS, 0, 1, "dtde", "Data transfer device element address "
     "(SSC)"},
    {VPD_EXT_INQ, 0, -1, "ei", "Extended inquiry data"},
    {VPD_FORMAT_PRESETS, 0, 0, "fp", "Format presets"},
    {VPD_LB_PROTECTION, 0, 0, "lbpro", "Logical block protection (SSC)"},
    {VPD_LB_PROVISIONING, 0, 0, "lbpv", "Logical block provisioning "
     "(SBC)"},
    {VPD_MAN_ASS_SN, 0, 1, "mas", "Manufacturer assigned serial number (SSC)"},
    {VPD_MAN_ASS_SN, 0, 0x12, "masa",
     "Manufacturer assigned serial number (ADC)"},
    {VPD_MAN_NET_ADDR, 0, -1, "mna", "Management network addresses"},
    {VPD_MODE_PG_POLICY, 0, -1, "mpp", "Mode page policy"},
    {SG_NVME_VPD_NICR, 0, -1, "nicr",
     "NVMe Identify Controller Response (sg3_utils for SNT)"},
    {VPD_POWER_CONDITION, 0, -1, "po", "Power condition"},/* "pc" in sg_vpd */
    {VPD_POWER_CONSUMPTION, 0, -1, "psm", "Power consumption"},
    {VPD_PROTO_LU, 0, -1, "pslu", "Protocol-specific logical unit "
     "information"},
    {VPD_PROTO_PORT, 0, -1, "pspo", "Protocol-specific port information"},
    {VPD_REFERRALS, 0, 0, "ref", "Referrals (SBC)"},
    {VPD_SA_DEV_CAP, 0, 1, "sad",
     "Sequential access device capabilities (SSC)"},
    {VPD_SUP_BLOCK_LENS, 0, 0, "sbl", "Supported block lengths and "
     "protection types (SBC)"},
    {VPD_SCSI_FEATURE_SETS, 0, -1, "sfs", "SCSI Feature sets"},
    {VPD_SOFTW_INF_ID, 0, -1, "sii", "Software interface identification"},
    {VPD_NOPE_WANT_STD_INQ, 0, -1, "sinq", "Standard inquiry data format"},
    {VPD_UNIT_SERIAL_NUM, 0, -1, "sn", "Unit serial number"},
    {VPD_SCSI_PORTS, 0, -1, "sp", "SCSI ports"},
    {VPD_SUPPORTED_VPDS, 0, -1, "sv", "Supported VPD pages"},
    {VPD_TA_SUPPORTED, 0, 1, "tas", "TapeAlert supported flags (SSC)"},
    {VPD_3PARTY_COPY, 0, -1, "tpc", "Third party copy"},
    {VPD_ZBC_DEV_CHARS, 0, 0, "zbdc", "Zoned block device characteristics"},
    {0, 0, 0, NULL, NULL},
};

/* Some alternate acronyms for T10 VPD pages (compatibility with sg_vpd) */
static const struct svpd_values_name_t alt_t10_vpd_pg[] = {
    {VPD_NOPE_WANT_STD_INQ, 0, -1, "stdinq", "Standard inquiry data format"},
    {VPD_POWER_CONDITION, 0, -1, "pc", "Power condition"},
    {0, 0, 0, NULL, NULL},
};

static const struct svpd_values_name_t vs_vpd_pg[] = {
    /* Following are vendor specific */
    {VPD_RDAC_VAC, 0, -1, "rdac_vac", "RDAC volume access control (RDAC)"},
    {VPD_RDAC_VERS, 0, -1, "rdac_vers", "RDAC software version (RDAC)"},
    {VPD_UPR_EMC, 0, -1, "upr", "Unit path report (EMC)"},
    {0, 0, 0, NULL, NULL},
};

static const struct option long_options[] = {
#if defined(SG_LIB_LINUX) && defined(SG_SCSI_STRINGS) && \
    defined(HDIO_GET_IDENTITY)
    {"ata", no_argument, 0, 'a'},
#endif
    {"block", required_argument, 0, 'B'},
    {"cmddt", no_argument, 0, 'c'},
    {"cns", required_argument, 0, 'C'},
    {"debug", no_argument, 0, 'D'},
    {"descriptors", no_argument, 0, 'd'},
    {"desc", no_argument, 0, 'd'},
    {"export", no_argument, 0, 'u'},
    {"extended", no_argument, 0, 'x'},
    {"force", no_argument, 0, 'f'},
    {"help", no_argument, 0, 'h'},
    {"hex", no_argument, 0, 'H'},
    {"id", no_argument, 0, 'i'},
    {"inhex", required_argument, 0, 'I'},
    {"json", optional_argument, 0, '^'},    /* short option is '-j' */
    {"js-file", required_argument, 0, 'J'},
    {"js_file", required_argument, 0, 'J'},
    {"len", required_argument, 0, 'l'},
    {"long", no_argument, 0, 'L'},
    {"maxlen", required_argument, 0, 'm'},
#ifdef SG_SCSI_STRINGS
    {"new", no_argument, 0, 'N'},
    {"old", no_argument, 0, 'O'},
#endif
    {"only", no_argument, 0, 'o'},
    {"page", required_argument, 0, 'p'},
    {"quiet", no_argument, 0, 'q'},
    {"raw", no_argument, 0, 'r'},
    {"sinq_inraw", required_argument, 0, 'Q'},
    {"sinq-inraw", required_argument, 0, 'Q'},
    {"vendor", no_argument, 0, 's'},
    {"verbose", no_argument, 0, 'v'},
    {"version", no_argument, 0, 'V'},
    {"vpd", no_argument, 0, 'e'},
    {0, 0, 0, 0},
};


static void
usage()
{
#if defined(SG_LIB_LINUX) && defined(SG_SCSI_STRINGS) && \
    defined(HDIO_GET_IDENTITY)

    pr2serr("Usage: sg_inq [--ata] [--block=0|1] [--cmddt] [cns=CNS] "
            "[--descriptors]\n"
            "              [--export] [--extended] [--help] [--hex] "
            "[--id]\n"
            "              [--inhex=FN] [--json[=JO]] [--js-file=JFN] "
            "[--len=LEN]\n"
            "              [--long] [--maxlen=LEN] [--only] [--page=PG] "
            "[--raw]\n"
            "              [--sinq_inraw=RFN] [--vendor] [--verbose] "
            "[--version]\n"
            "              [--vpd] DEVICE\n"
            "  where:\n"
            "    --ata|-a        treat DEVICE as (directly attached) ATA "
            "device\n");
#else
    pr2serr("Usage: sg_inq [--block=0|1] [--cmddt] [cns=CNS] "
            "[--descriptors] [--export]\n"
            "              [--extended] [--help] [--hex] [--id] "
            "[--inhex=FN]\n"
            "              [--json[=JO]] [--js-file=JFN] [--len=LEN] "
            "[--long]\n"
            "              [--maxlen=LEN] [--only] [--page=PG] [--quiet] "
            "[--raw]\n"
            "              [--sinq_inraw=RFN] [--verbose] [--version] "
            "[--vpd]\n"
            "              DEVICE\n"
            "  where:\n");
#endif
    pr2serr("    --block=0|1     0-> open(non-blocking); 1-> "
            "open(blocking)\n"
            "      -B 0|1        (def: depends on OS; Linux pt: 0)\n"
            "    --cmddt|-c      command support data mode (obsolete, "
            "see sg_opcodes)\n"
            "    --cns=CNS       value for NVMe Identify command\n"
            "    --descriptors|-d    display standard inquiry version "
            "descriptors\n"
            "    --export|-u     SCSI_IDENT_<assoc>_<type>=<ident> output "
            "format.\n"
            "                    Defaults to device id page (0x83) if --page "
            "not given,\n"
            "                    only supported for VPD pages 0x80 and 0x83\n"
            "    --extended|-E|-x    decode extended INQUIRY data VPD page "
            "(0x86)\n"
            "    --force|-f      skip VPD page 0 check; directly fetch "
            "requested page\n"
            "    --help|-h       print usage message then exit\n"
            "    --hex|-H        output response in hex\n"
            "    --id|-i         decode device identification VPD page "
            "(0x83)\n"
            "    --inhex=FN|-I FN    read ASCII hex from file FN instead of "
            "DEVICE;\n"
            "                        if used with --raw then read binary "
            "from FN\n"
            "    --json[=JO]|-j[=JO]    output in JSON instead of plain "
            "text\n"
            "                          Use --json=? for JSON help\n"
            "    --js-file=JFN|-J JFN    JFN is a filename to which JSON "
            "output is\n"
            "                            written (def: stdout); truncates "
            "then writes\n"
            "    --len=LEN|-l LEN    requested response length (def: 0 "
            "-> fetch 36\n"
            "                        bytes first, then fetch again as "
            "indicated)\n"
            "    --long|-L       supply extra information on NVMe devices\n"
            "    --maxlen=LEN|-m LEN    same as '--len='\n"
            "    --old|-O        use old interface (use as first option)\n"
            "    --only|-o       for std inquiry do not fetch serial number "
            "vpd page;\n"
            "                    for NVMe device only do Identify "
            "controller\n"
            "    --page=PG|-p PG     Vital Product Data (VPD) page number "
            "or\n"
            "                        abbreviation (opcode number if "
            "'--cmddt' given)\n"
            "    --quiet|-q      suppress some decoding and error output\n"
            "    --raw|-r        output response in binary (to stdout)\n"
            "    --sinq_inraw=RFN|-Q RFN    read raw (binary) standard "
            "INQUIRY\n"
            "                               response from the RFN filename\n"
            "    --vendor|-s     show vendor specific fields in std "
            "inquiry\n"
            "    --verbose|-v    increase verbosity\n"
            "    --version|-V    print this utility's version string then "
            "exit\n"
            "    --vpd|-e        vital product data (set page with "
            "'--page=PG')\n\n"
            "Sends a SCSI INQUIRY command to the DEVICE and decodes the "
            "response.\nAlternatively it decodes the INQUIRY response held "
            "in file FN. If no\noptions given then it sends a 'standard' "
            "INQUIRY command to DEVICE. Can\nlist VPD pages with '--vpd' or "
            "'--page=PG' option.\n");
}

#ifdef SG_SCSI_STRINGS
static void
usage_old()
{
#ifdef SG_LIB_LINUX
    pr2serr("Usage:  sg_inq [-a] [-A] [-b] [-B=0|1] [-c] [-cl] [-d] [-e] "
            "[-h]\n"
            "               [-H] [-i] [-I=FN] [-j[=JO]] [-l=LEN] [-L] [-m] "
            "[-M]\n"
            "               [-o] [-p=VPD_PG] [-P] [-r] [-s] [-u] [-U] [-v] "
            "[-V]\n"
            "               [-x] [-36] [-?] DEVICE\n"
            "  where:\n"
            "    -a    decode ATA information VPD page (0x89)\n"
            "    -A    treat <device> as (directly attached) ATA device\n");
#else
    pr2serr("Usage:  sg_inq [-a] [-b] [-B 0|1] [-c] [-cl] [-d] [-e] [-h] "
            "[-H]\n"
            "               [-i] [-l=LEN] [-L] [-m] [-M] [-o] "
            "[-p=VPD_PG]\n"
            "               [-P] [-r] [-s] [-u] [-v] [-V] [-x] [-36] "
            "[-?]\n"
            "               DEVICE\n"
            "  where:\n"
            "    -a    decode ATA information VPD page (0x89)\n");

#endif  /* SG_LIB_LINUX */
    pr2serr("    -b    decode Block limits VPD page (0xb0) (SBC)\n"
            "    -B=0|1    0-> open(non-blocking); 1->open(blocking)\n"
            "    -c    set CmdDt mode (use -o for opcode) [obsolete]\n"
            "    -cl   list supported commands using CmdDt mode [obsolete]\n"
            "    -d    decode: version descriptors or VPD page\n"
            "    -e    set VPD mode (use -p for page code)\n"
            "    -h    output in hex (ASCII to the right)\n"
            "    -H    output in hex (ASCII to the right) [same as '-h']\n"
            "    -i    decode device identification VPD page (0x83)\n"
            "    -I=FN    use ASCII hex in file FN instead of DEVICE\n"
            "    -j[=JO]    output in JSON instead of plain text\n"
            "    -l=LEN    requested response length (def: 0 "
            "-> fetch 36\n"
            "                    bytes first, then fetch again as "
            "indicated)\n"
            "    -L    supply extra information on NVMe devices\n"
            "    -m    decode management network addresses VPD page "
            "(0x85)\n"
            "    -M    decode mode page policy VPD page (0x87)\n"
            "    -N|--new   use new interface\n"
            "    -o    for std inquiry only do that, not serial number vpd "
            "page\n"
            "    -p=VPD_PG    vpd page code in hex (def: 0)\n"
            "    -P    decode Unit Path Report VPD page (0xc0) (EMC)\n"
            "    -r    output response in binary ('-rr': output for hdparm)\n"
            "    -s    decode SCSI Ports VPD page (0x88)\n"
            "    -u    SCSI_IDENT_<assoc>_<type>=<ident> output format\n"
            "    -v    verbose (output cdb and, if non-zero, resid)\n"
            "    -V    output version string\n"
            "    -x    decode extended INQUIRY data VPD page (0x86)\n"
            "    -36   perform standard INQUIRY with a 36 byte response\n"
            "    -?    output this usage message\n\n"
            "If no options given then sends a standard SCSI INQUIRY "
            "command and\ndecodes the response.\n");
}

static void
usage_for(const struct opts_t * op)
{
    if (op->opt_new)
        usage();
    else
        usage_old();
}

#else  /* SG_SCSI_STRINGS */

static void
usage_for(const struct opts_t * op)
{
    if (op) { }         /* suppress warning */
    usage();
}

#endif /* SG_SCSI_STRINGS */

/* Handles short options after '-j' including a sequence of short options
 * that include one 'j' (for JSON). Want optional argument to '-j' to be
 * prefixed by '='. Return 0 for good, SG_LIB_SYNTAX_ERROR for syntax error
 * and SG_LIB_OK_FALSE for exit with no error. */
static int
chk_short_opts(const char sopt_ch, struct opts_t * op)
{
    /* only need to process short, non-argument options */
    switch (sopt_ch) {
#if defined(SG_LIB_LINUX) && defined(SG_SCSI_STRINGS) && \
    defined(HDIO_GET_IDENTITY)
    case 'a':
        op->do_ata = true;
        break;
#endif
    case 'c':
        ++op->do_cmddt;
        break;
    case 'd':
        op->do_descriptors = true;
        break;
    case 'D':
        op->do_debug = true;
        break;
    case 'e':
        op->do_vpd = true;
        break;
    case 'E':       /* --extended */
    case 'x':
        op->do_decode = true;
        op->do_vpd = true;
        op->vpd_pn = VPD_EXT_INQ;
        op->page_given = true;
        break;
    case 'f':
        op->do_force = true;
        break;
    case 'h':
        ++op->do_help;
        break;
    case 'j':
        break;  /* simply ignore second 'j' (e.g. '-jxj') */
    case 'o':
        op->do_only = true;
        break;
    case '?':
        if (! op->do_help)
            ++op->do_help;
        break;
    case 'H':
        ++op->do_hex;
        break;
    case 'i':
        op->do_decode = true;
        op->do_vpd = true;
        op->vpd_pn = VPD_DEVICE_ID;
        op->page_given = true;
        break;
    case 'I':
        op->inhex_fn = optarg;
        break;
    case 'L':
        ++op->do_long;
        break;
#ifdef SG_SCSI_STRINGS
    case 'N':
        break;      /* ignore */
    case 'O':
        op->opt_new = false;
        return 0;
#endif
    case 'q':
        op->do_quiet = true;
        break;
    case 'r':
        ++op->do_raw;
        break;
    case 's':
        ++op->do_vendor;
        break;
    case 'u':
        op->do_export = true;
        break;
    case 'v':
        op->verbose_given = true;
        ++op->verbose;
        break;
    case 'V':
        op->version_given = true;
        break;
    default:
        pr2serr("unrecognised option code %c [0x%x]\n", sopt_ch, sopt_ch);
        if (op->do_help)
            break;
        usage_for(op);
        return SG_LIB_SYNTAX_ERROR;
    }
    return 0;
}

/* Processes command line options according to new option format. Returns
 * 0 is ok, else SG_LIB_SYNTAX_ERROR is returned. */
static int
new_parse_cmd_line(struct opts_t * op, int argc, char * argv[])
{
    int c, k, n, q;

    while (1) {
        int option_index = 0;

#ifdef SG_LIB_LINUX
#ifdef SG_SCSI_STRINGS
        c = getopt_long(argc, argv,
                        "^aB:cC:dDeEfhHiI:j::J:l:Lm:M:NoOp:qQ:rsuvVx",
                        long_options, &option_index);
#else
        c = getopt_long(argc, argv, "^B:cdDeEfhHiI:j::J:l:Lm:M:op:qQ:rsuvVx",
                        long_options, &option_index);
#endif /* SG_SCSI_STRINGS */
#else  /* SG_LIB_LINUX */
#ifdef SG_SCSI_STRINGS
        c = getopt_long(argc, argv,
                        "^B:cdDeEfhHiI:j::J:l:Lm:M:NoOp:qQ:rsuvVx",
                        long_options, &option_index);
#else
        c = getopt_long(argc, argv, "^B:cdDeEfhHiI:j::J:l:Lm:M:op:qQ:rsuvVx",
                        long_options, &option_index);
#endif /* SG_SCSI_STRINGS */
#endif /* SG_LIB_LINUX */
        if (c == -1)
            break;

        switch (c) {
#if defined(SG_LIB_LINUX) && defined(SG_SCSI_STRINGS) && \
    defined(HDIO_GET_IDENTITY)
        case 'a':
            op->do_ata = true;
            break;
#endif
        case 'B':
            if ('-' == optarg[0])
                n = -1;
            else {
                n = sg_get_num(optarg);
                if ((n < 0) || (n > 1)) {
                    pr2serr("bad argument to '--block=' want 0 or 1\n");
                    usage_for(op);
                    return SG_LIB_SYNTAX_ERROR;
                }
            }
            op->do_block = n;
            break;
        case 'c':
            ++op->do_cmddt;
            break;
        case 'C':
            n = sg_get_num(optarg);
            if ((n < 0) || (n > 255)) {
                pr2serr("bad argument to '--cns=' want 0 or 255\n");
                usage_for(op);
                return SG_LIB_SYNTAX_ERROR;
            }
            break;
        case 'd':
            op->do_descriptors = true;
            break;
        case 'D':
            op->do_debug = true;
            break;
        case 'e':
            op->do_vpd = true;
            break;
        case 'E':       /* --extended */
        case 'x':
            op->do_decode = true;
            op->do_vpd = true;
            op->vpd_pn = VPD_EXT_INQ;
            op->page_given = true;
            break;
        case 'f':
            op->do_force = true;
            break;
        case 'h':
            ++op->do_help;
            break;
        case 'j':       /* for: -j[=JO] */
        case '^':       /* for: --json[=JO] */
            op->do_json = true;
            /* Now want '=' to precede all JSON optional arguments */
            if (optarg) {
                if ('^' == c) {
                    op->json_arg = optarg;
                    break;
                } else if ('=' == *optarg) {
                    op->json_arg = optarg + 1;
                    break;
                }
                n = strlen(optarg);
                for (k = 0; k < n; ++k) {
                    q = chk_short_opts(*(optarg + k), op);
                    if (SG_LIB_SYNTAX_ERROR == q)
                        return SG_LIB_SYNTAX_ERROR;
                    if (SG_LIB_OK_FALSE == q)
                        return 0;
                }
            } else
                op->json_arg = NULL;
            break;
        case 'J':
            op->do_json = true;
            op->js_file = optarg;
            break;
        case 'o':
            op->do_only = true;
            break;
        case '?':
            if (! op->do_help)
                ++op->do_help;
            break;
        case 'H':
            ++op->do_hex;
            break;
        case 'i':
            op->do_decode = true;
            op->do_vpd = true;
            op->vpd_pn = VPD_DEVICE_ID;
            op->page_given = true;
            break;
        case 'I':
            op->inhex_fn = optarg;
            break;
        case 'l':
        case 'm':
            n = sg_get_num(optarg);
            if ((n < 0) || (n > 65532)) {
                pr2serr("bad argument to '--len='\n");
                usage_for(op);
                return SG_LIB_SYNTAX_ERROR;
            }
            if ((n > 0) && (n < 4)) {
                pr2serr("Changing that '--maxlen=' value to 4\n");
                n = 4;
            }
            op->maxlen = n;
            break;
        case 'M':
            if (op->vend_prod_arg) {
                pr2serr("only one '--vendor=' option permitted\n");
                usage();
                return SG_LIB_SYNTAX_ERROR;
            } else
                op->vend_prod_arg = optarg;
            break;
        case 'L':
            ++op->do_long;
            break;
#ifdef SG_SCSI_STRINGS
        case 'N':
            break;      /* ignore */
        case 'O':
            op->opt_new = false;
            return 0;
#endif
        case 'p':
            op->page_str = optarg;
            op->page_given = true;
            break;
        case 'q':
            op->do_quiet = true;
            break;
        case 'Q':
            op->sinq_inraw_fn = optarg;
            break;
        case 'r':
            ++op->do_raw;
            break;
        case 's':
            ++op->do_vendor;
            break;
        case 'u':
            op->do_export = true;
            break;
        case 'v':
            op->verbose_given = true;
            ++op->verbose;
            break;
        case 'V':
            op->version_given = true;
            break;
        default:
            pr2serr("unrecognised option code %c [0x%x]\n", c, c);
            if (op->do_help)
                break;
            usage_for(op);
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    if (optind < argc) {
        if (NULL == op->device_name) {
            op->device_name = argv[optind];
            ++optind;
        }
        if (optind < argc) {
            for (; optind < argc; ++optind)
                pr2serr("Unexpected extra argument: %s\n", argv[optind]);
            usage_for(op);
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    return 0;
}

#ifdef SG_SCSI_STRINGS
/* Processes command line options according to old option format. Returns
 * 0 is ok, else SG_LIB_SYNTAX_ERROR is returned. */
static int
old_parse_cmd_line(struct opts_t * op, int argc, char * argv[])
{
    bool jmp_out;
    int k, plen, num, n;
    const char * cp;

    for (k = 1; k < argc; ++k) {
        cp = argv[k];
        plen = strlen(cp);
        if (plen <= 0)
            continue;
        if ('-' == *cp) {
            for (--plen, ++cp, jmp_out = false; plen > 0; --plen, ++cp) {
                switch (*cp) {
                case '3':
                    if ('6' == *(cp + 1)) {
                        op->maxlen = 36;
                        --plen;
                        ++cp;
                    } else
                        jmp_out = true;
                    break;
                case 'a':
                    op->vpd_pn = VPD_ATA_INFO;
                    op->do_vpd = true;
                    op->page_given = true;
                    ++op->num_pages;
                    break;
#ifdef SG_LIB_LINUX
                case 'A':
                    op->do_ata = true;
                    break;
#endif
                case 'b':
                    op->vpd_pn = VPD_BLOCK_LIMITS;
                    op->do_vpd = true;
                    op->page_given = true;
                    ++op->num_pages;
                    break;
                case 'c':
                    ++op->do_cmddt;
                    if ('l' == *(cp + 1)) {
                        ++op->do_cmddt;
                        --plen;
                        ++cp;
                    }
                    break;
                case 'd':
                    op->do_descriptors = true;
                    op->do_decode = true;
                    break;
                case 'D':
                    op->do_debug = true;
                    break;
                case 'e':
                    op->do_vpd = true;
                    break;
                case 'f':
                    op->do_force = true;
                    break;
                case 'h':
                case 'H':
                    ++op->do_hex;
                    break;
                case 'i':
                    op->vpd_pn = VPD_DEVICE_ID;
                    op->do_vpd = true;
                    op->page_given = true;
                    ++op->num_pages;
                    break;
                case 'j':
                    op->do_json = true;
                    /* ignore optional argument if given */
                    break;
                case 'L':
                    ++op->do_long;
                    break;
                case 'm':
                    op->vpd_pn = VPD_MAN_NET_ADDR;
                    op->do_vpd = true;
                    ++op->num_pages;
                    op->page_given = true;
                    break;
                case 'M':
                    op->vpd_pn = VPD_MODE_PG_POLICY;
                    op->do_vpd = true;
                    op->page_given = true;
                    ++op->num_pages;
                    break;
                case 'N':
                    op->opt_new = true;
                    return 0;
                case 'o':
                    op->do_only = true;
                    break;
                case 'O':
                    break;
                case 'P':
                    op->vpd_pn = VPD_UPR_EMC;
                    op->do_vpd = true;
                    op->page_given = true;
                    ++op->num_pages;
                    break;
                case 'r':
                    ++op->do_raw;
                    break;
                case 's':
                    op->vpd_pn = VPD_SCSI_PORTS;
                    op->do_vpd = true;
                    op->page_given = true;
                    ++op->num_pages;
                    break;
                case 'u':
                    op->do_export = true;
                    break;
                case 'v':
                    op->verbose_given = true;
                    ++op->verbose;
                    break;
                case 'V':
                    op->version_given = true;
                    break;
                case 'x':
                    op->vpd_pn = VPD_EXT_INQ;
                    op->do_vpd = true;
                    op->page_given = true;
                    ++op->num_pages;
                    break;
                case '?':
                    if (! op->do_help)
                        ++op->do_help;
                    break;
                default:
                    jmp_out = true;
                    break;
                }
                if (jmp_out)
                    break;
            }
            if (plen <= 0)
                continue;
            else if (0 == strncmp("B=", cp, 2)) {
                num = sscanf(cp + 2, "%d", &n);
                if ((1 != num) || (n < 0) || (n > 1)) {
                    pr2serr("'B=' option expects 0 or 1\n");
                    usage_for(op);
                    return SG_LIB_SYNTAX_ERROR;
                }
                op->do_block = n;
            } else if (0 == strncmp("I=", cp, 2))
                op->inhex_fn = cp + 2;
            else if ('j' == *cp) { /* handle either '-j' or '-j=<JO>' */
                const char * c2p = (('=' == *(cp + 1)) ? cp + 2 : NULL);

                op->do_json = true;
                op->json_arg = c2p;
            } else if (0 == strncmp("l=", cp, 2)) {
                num = sscanf(cp + 2, "%d", &n);
                if ((1 != num) || (n < 1)) {
                    pr2serr("Inappropriate value after 'l=' option\n");
                    usage_for(op);
                    return SG_LIB_SYNTAX_ERROR;
                } else if (n > MX_ALLOC_LEN) {
                    pr2serr("value after 'l=' option too large\n");
                    return SG_LIB_SYNTAX_ERROR;
                }
                if ((n > 0) && (n < 4)) {
                    pr2serr("Changing that '-l=' value to 4\n");
                    n = 4;
                }
                op->maxlen = n;
            } else if (0 == strncmp("p=", cp, 2)) {
                op->page_str = cp + 2;
                op->page_given = true;
            } else if (0 == strncmp("-old", cp, 4))
                ;
            else if (jmp_out) {
                pr2serr("Unrecognized option: %s\n", cp);
                usage_for(op);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == op->device_name)
            op->device_name = cp;
        else {
            pr2serr("too many arguments, got: %s, not expecting: %s\n",
                    op->device_name, cp);
            usage_for(op);
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    return 0;
}

/* Process command line options. First check using new option format unless
 * the SG3_UTILS_OLD_OPTS environment variable is defined which causes the
 * old option format to be checked first. Both new and old format can be
 * countermanded by a '-O' and '-N' options respectively. As soon as either
 * of these options is detected (when processing the other format), processing
 * stops and is restarted using the other format. Clear? */
static int
parse_cmd_line(struct opts_t * op, int argc, char * argv[])
{
    int res;
    char * cp;

    cp = getenv("SG3_UTILS_OLD_OPTS");
    if (cp) {
        op->opt_new = false;
        res = old_parse_cmd_line(op, argc, argv);
        if ((0 == res) && op->opt_new)
            res = new_parse_cmd_line(op, argc, argv);
    } else {
        op->opt_new = true;
        res = new_parse_cmd_line(op, argc, argv);
        if ((0 == res) && (! op->opt_new))
            res = old_parse_cmd_line(op, argc, argv);
    }
    return res;
}

#else  /* SG_SCSI_STRINGS */

static int
parse_cmd_line(struct opts_t * op, int argc, char * argv[])
{
    return new_parse_cmd_line(op, argc, argv);
}

#endif  /* SG_SCSI_STRINGS */


static const struct svpd_values_name_t *
sdp_find_vpd_by_acron(const char * ap)
{
    const struct svpd_values_name_t * vnp;

    for (vnp = t10_vpd_pg; vnp->acron; ++vnp) {
        if (0 == strcmp(vnp->acron, ap))
            return vnp;
    }
    for (vnp = alt_t10_vpd_pg; vnp->acron; ++vnp) {
        if (0 == strcmp(vnp->acron, ap))
            return vnp;
    }
    for (vnp = vs_vpd_pg; vnp->acron; ++vnp) {
        if (0 == strcmp(vnp->acron, ap))
            return vnp;
    }
    return NULL;
}

static void
enumerate_vpds()
{
    const struct svpd_values_name_t * vnp;

    printf("T10 defined %ss:\n", vpd_pg_s);
    for (vnp = t10_vpd_pg; vnp->acron; ++vnp) {
        if (vnp->name) {
            if (vnp->value < 0)
                printf("  %-10s   -1      %s\n", vnp->acron, vnp->name);
            else
                printf("  %-10s 0x%02x      %s\n", vnp->acron, vnp->value,
                       vnp->name);
        }
    }
    printf("Vendor specific %ss:\n", vpd_pg_s);
    for (vnp = vs_vpd_pg; vnp->acron; ++vnp) {
        if (vnp->name) {
            if (vnp->value < 0)
                printf("  %-10s   -1      %s\n", vnp->acron, vnp->name);
            else
                printf("  %-10s 0x%02x      %s\n", vnp->acron, vnp->value,
                       vnp->name);
        }
    }
}

static void
dStrRaw(const char * str, int len)
{
    int k;

    for (k = 0; k < len; ++k)
        printf("%c", str[k]);
}

/* Strip initial and trailing whitespaces; convert one or repeated
 * whitespaces to a single "_"; convert non-printable characters to "."
 * and if there are no valid (i.e. printable) characters return 0.
 * Process 'str' in place (i.e. it's input and output) and return the
 * length of the output, excluding the trailing '\0'. To cover any
 * potential unicode string an intermediate zero is skipped; two
 * consecutive zeroes indicate a string termination.
 */
static int
encode_whitespaces(uint8_t *str, int inlen)
{
    int k, res;
    int j;
    bool valid = false;
    int outlen = inlen, zeroes = 0;

    /* Skip initial whitespaces */
    for (j = 0; (j < inlen) && isblank(str[j]); ++j)
        ;
    if (j < inlen) {
        /* Skip possible unicode prefix characters */
        for ( ; (j < inlen) && (str[j] < 0x20); ++j)
            ;
    }
    k = j;
    /* Strip trailing whitespaces */
    while ((outlen > k) &&
           (isblank(str[outlen - 1]) || ('\0' == str[outlen - 1]))) {
        str[outlen - 1] = '\0';
        outlen--;
    }
    for (res = 0; k < outlen; ++k) {
        if (isblank(str[k])) {
            if ((res > 0) && ('_' != str[res - 1])) {
                str[res++] = '_';
                valid = true;
            }
            zeroes = 0;
        } else if (! isprint(str[k])) {
            if (str[k] == 0x00) {
                /* Stop on more than one consecutive zero */
                if (zeroes)
                    break;
                zeroes++;
                continue;
            }
            str[res++] = '.';
            zeroes = 0;
        } else {
            str[res++] = str[k];
            valid = true;
            zeroes = 0;
        }
    }
    if (! valid)
        res = 0;
    if (res < inlen)
        str[res] = '\0';
    return res;
}

static int
encode_unicode(uint8_t *str, int inlen)
{
    int k = 0, res;
    int zeroes = 0;

    for (res = 0; k < inlen; ++k) {
        if (str[k] == 0x00) {
            if (zeroes) {
                str[res++] = '\0';
                break;
            }
            zeroes++;
        } else {
            zeroes = 0;
            if (isprint(str[k]))
                str[res++] = str[k];
            else
                str[res++] = ' ';
        }
    }

    return res;
}

static int
encode_string(char *out, const uint8_t *in, int inlen)
{
    int i, j = 0;

    for (i = 0; (i < inlen); ++i) {
        if (isblank(in[i]) || !isprint(in[i])) {
            sprintf(&out[j], "\\x%02x", in[i]);
            j += 4;
        } else {
            out[j] = in[i];
            j++;
        }
    }
    out[j] = '\0';
    return j;
}

static const struct svpd_values_name_t *
get_vpd_page_info(int vpd_page_num, int dev_pdt)
{
    int decay_pdt;
    const struct svpd_values_name_t * vnp;
    const struct svpd_values_name_t * prev_vnp;

    if (vpd_page_num < 0xb0) {  /* take T10 first match */
        for (vnp = t10_vpd_pg; vnp->acron; ++vnp) {
            if (vnp->value == vpd_page_num)
                return vnp;
        }
        return NULL;
    } else if (vpd_page_num < 0xc0) {
        for (vnp = t10_vpd_pg; vnp->acron; ++vnp) {
            if (vnp->value == vpd_page_num)
                break;
        }
        if (NULL == vnp->acron)
            return NULL;
        if (vnp->pdt == dev_pdt)        /* exact match */
            return vnp;
        prev_vnp = vnp;

        for (++vnp; vnp->acron; ++vnp) {
            if (vnp->value == vpd_page_num)
                break;
        }
        decay_pdt = sg_lib_pdt_decay(dev_pdt);
        if (NULL == vnp->acron) {
            if (decay_pdt == prev_vnp->pdt)
                return prev_vnp;
            return NULL;
        }
        if ((vnp->pdt == dev_pdt) || (vnp->pdt == decay_pdt))
            return vnp;
        if (decay_pdt == prev_vnp->pdt)
            return prev_vnp;

        for (++vnp; vnp->acron; ++vnp) {
            if (vnp->value == vpd_page_num)
                break;
        }
        if (NULL == vnp->acron)
            return NULL;
        if ((vnp->pdt == dev_pdt) || (vnp->pdt == decay_pdt))
            return vnp;
        return NULL;            /* give up */
    } else {    /* vendor specific: vpd >= 0xc0 */
        for (vnp = vs_vpd_pg; vnp->acron; ++vnp) {
            if (vnp->pdt == dev_pdt)
                return vnp;
        }
        return NULL;
    }
}

static int
svpd_inhex_decode_all(struct opts_t * op, sgj_opaque_p jop)
{
    int res, pn;
    int max_pn = 255;
    int bump, off;
    int in_len = op->maxlen;
    int prev_pn = -1;
    sgj_state * jsp = &op->json_st;

    if (op->vpd_pn > 0)
        max_pn = op->vpd_pn;

    res = 0;
    if (op->page_given && (VPD_NOPE_WANT_STD_INQ == op->vpd_pn))
        return vpd_decode(NULL, op, jop, 0);

    for (off = 0; off < in_len; off += bump) {
        uint8_t * rp = rsp_buff + off;

        pn = rp[1];
        bump = sg_get_unaligned_be16(rp + 2) + 4;
        if ((off + bump) > in_len) {
            pr2serr("%s: page 0x%x size (%d) exceeds buffer\n", __func__,
                    pn, bump);
            bump = in_len - off;
        }
        if (op->page_given && (pn != op->vpd_pn))
            continue;
        if (pn <= prev_pn) {
            pr2serr("%s: prev_pn=0x%x, this pn=0x%x, not ascending so "
                    "exit\n", __func__, prev_pn, pn);
            break;
        }
        prev_pn = pn;
        op->vpd_pn = pn;
        if (pn > max_pn) {
            if (op->verbose > 2)
                pr2serr("%s: skipping as this pn=0x%x exceeds "
                        "max_pn=0x%x\n", __func__, pn, max_pn);
            continue;
        }
        if (op->do_long) {
            if (jsp->pr_as_json)
               sgj_pr_hr(jsp, "[0x%x]:\n", pn);
            else
               sgj_pr_hr(jsp, "[0x%x] ", pn);
        }

        res = vpd_decode(NULL, op, jop, off);
        if (SG_LIB_CAT_OTHER == res) {
            if (op->verbose)
                pr2serr("Can't decode %s=0x%x\n", vpd_pg_s, pn);
        }
    }
    return res;
}

static void
decode_supported_vpd_4inq(uint8_t * buff, int len, struct opts_t * op,
                          sgj_opaque_p jap)
{
    int vpd, k, rlen, pdt;
    sgj_state * jsp = &op->json_st;
    sgj_opaque_p jo2p;
    const struct svpd_values_name_t * vnp;
    char b[64];

    if (op->do_hex > 0) {
        hex2stdout(buff, len, no_ascii_4hex(op));
        return;
    }
    if (len < 4) {
        pr2serr("%s %s=%d\n", svp_vpdp, lts_s, len);
        return;
    }
    pdt = PDT_MASK & buff[0];
    rlen = buff[3] + 4;
    if (rlen > len)
        pr2serr("%s truncated, indicates %d, got %d\n", svp_vpdp, rlen, len);
    else
        len = rlen;
    sgj_pr_hr(jsp, "   Supported %ss:\n", vpd_pg_s);
    for (k = 0; k < len - 4; ++k) {
        vpd = buff[4 + k];
        snprintf(b, sizeof(b), "0x%x", vpd);
        vnp = get_vpd_page_info(vpd, pdt);
        if (jsp->pr_as_json && jap) {
            jo2p = sgj_new_unattached_object_r(jsp);
            sgj_js_nv_i(jsp, jo2p, "i", vpd);
            sgj_js_nv_s(jsp, jo2p, "hex", b + 2);
            sgj_js_nv_s(jsp, jo2p, "name", vnp ? vnp->name : "unknown");
            sgj_js_nv_s(jsp, jo2p, "acronym", vnp ? vnp->acron : "unknown");
            sgj_js_nv_o(jsp, jap, NULL /* name */, jo2p);
        }
        if (vnp)
            sgj_pr_hr(jsp, "     %s\t%s\n", b, vnp->name);
        else
            sgj_pr_hr(jsp, "     %s\n", b);
    }
}

static bool
vpd_page_is_supported(uint8_t * vpd_pg0, int v0_len, int pg_num, int vb)
{
    int k, rlen;

    if (v0_len < 4)
        return false;

    rlen = vpd_pg0[3] + 4;
    if (rlen > v0_len)
        pr2serr("%s truncated, indicates %d, got %d\n", svp_vpdp, rlen,
                v0_len);
    else
        v0_len = rlen;
    if (vb > 1) {
        pr2serr("Supported %ss, hex list: ", vpd_pg_s);
        hex2stderr(vpd_pg0 + 4, v0_len - 4, -1);
    }
    for (k = 4; k < v0_len; ++k) {
        if(vpd_pg0[k] == pg_num)
            return true;
    }
    return false;
}

/* ASCII Information VPD pages (page numbers: 0x1 to 0x7f) */
static void
decode_ascii_inf(uint8_t * buff, int len, struct opts_t * op,
                 sgj_opaque_p jop)
{
    int al, k, n, bump;
    uint8_t * bp;
    uint8_t * p;
    sgj_state * jsp = &op->json_st;

    if (len < 4) {
        pr2serr("ASCII information %s %s=%d\n", vpd_pg_s,  lts_s, len);
        return;
    }
    if (4 == len)
        return;
    al = buff[4];
    if ((al + 5) > len)
        al = len - 5;
    for (k = 0, bp = buff + 5; k < al; k += bump, bp += bump) {
        p = (uint8_t *)memchr(bp, 0, al - k);
        if (! p) {
            sgj_pr_hr(jsp, "  %.*s\n", al - k, (const char *)bp);
            break;
        }
        sgj_pr_hr(jsp, "  %s\n", (const char *)bp);
        bump = (p - bp) + 1;
    }
    bp = buff + 5 + al;
    if (bp < (buff + len)) {
        char * cp;

        sgj_pr_hr(jsp, "Vendor specific information in hex:\n");
        n = (len * 4) + 64;
        cp = (char *)malloc(n);
        if (cp) {
            n = hex2str(bp, len - (al + 5), NULL, 1, n - 1, cp);
            if (jsp && jsp->pr_out_hr)
                sgj_hr_str_out(jsp, cp, n);
            else
                sgj_pr_hr(jsp, "%s\n", cp);
        }
    }
    if (jsp->pr_as_json)
        sgjv_js_hex_long(jsp, jop, buff, len);
}

static void
decode_id_vpd(uint8_t * buff, int len, struct opts_t * op, sgj_opaque_p jap)
{
    if (len < 4) {
        pr2serr("%s %s=%d\n", di_vpdp,  lts_s, len);
        return;
    }
    decode_dev_ids("Device identification", buff + 4, len - 4, op, jap);
}

/* VPD_SCSI_PORTS   0x88  ["sp"] */
static void
decode_scsi_ports_vpd_4inq(uint8_t * buff, int len, struct opts_t * op,
                           sgj_opaque_p jap)
{
    int k, bump, rel_port, ip_tid_len, tpd_len;
    int dhex = op->do_hex;
    uint8_t * bp;
    sgj_state * jsp = &op->json_st;
    sgj_opaque_p jo2p;

    if (len < 4) {
        pr2serr("%s %s=%d\n", sp_vpdp,  lts_s, len);
        return;
    }
    if (dhex > 0) {
        hex2stdout(buff, len, no_ascii_4hex(op));
        return;
    } else if (dhex < 0)
        dhex = -dhex;
    len -= 4;
    bp = buff + 4;
    for (k = 0; k < len; k += bump, bp += bump) {
        jo2p = sgj_new_unattached_object_r(jsp);
        rel_port = sg_get_unaligned_be16(bp + 2);
        sgj_pr_hr(jsp, "Relative port=%d\n", rel_port);
        sgj_js_nv_i(jsp, jo2p, "relative_port", rel_port);
        ip_tid_len = sg_get_unaligned_be16(bp + 6);
        bump = 8 + ip_tid_len;
        if ((k + bump) > len) {
            pr2serr("%s, short descriptor length=%d, left=%d\n", sp_vpdp,
                    bump, (len - k));
            sgj_js_nv_o(jsp, jap, NULL /* name */, jo2p);
            return;
        }
        if (ip_tid_len > 0) {
            if (dhex) {
                printf(" Initiator port transport id:\n");
                hex2stdout((bp + 8), ip_tid_len, no_ascii_4hex(op));
            } else {
                char b[1024];

                sg_decode_transportid_str("    ", bp + 8, ip_tid_len,
                                          true, sizeof(b), b);
                if (jsp->pr_as_json)
                    sgj_js_nv_s(jsp, jo2p, "initiator_port_transport_id", b);
                sgj_pr_hr(jsp, "%s",
                          sg_decode_transportid_str("    ", bp + 8,
                                            ip_tid_len, true, sizeof(b), b));
            }
        }
        tpd_len = sg_get_unaligned_be16(bp + bump + 2);
        if ((k + bump + tpd_len + 4) > len) {
            pr2serr("%s, short descriptor(tgt) length=%d, left=%d\n", sp_vpdp,
                    bump, (len - k));
            sgj_js_nv_o(jsp, jap, NULL /* name */, jo2p);
            return;
        }
        if (tpd_len > 0) {
            sgj_pr_hr(jsp, " Target port descriptor(s):\n");
            if (dhex > 0)
                hex2stdout(bp + bump + 4, tpd_len, no_ascii_4hex(op));
            else {
                sgj_opaque_p ja2p = sgj_named_subarray_r(jsp, jo2p,
                                        "target_port_descriptor_list");

                decode_dev_ids("SCSI Ports", bp + bump + 4, tpd_len,
                               op, ja2p);
            }
        }
        bump += tpd_len + 4;
        sgj_js_nv_o(jsp, jap, NULL /* name */, jo2p);
    }
}

/* These are target port, device server (i.e. target) and LU identifiers */
static void
decode_dev_ids(const char * leadin, uint8_t * buff, int len,
               struct opts_t * op, sgj_opaque_p jap)
{
    int u, j, m, id_len, p_id, c_set, piv, assoc, desig_type, i_len;
    int off, ci_off, c_id, d_id, naa, vsi, k, n;
    uint64_t vsei, id_ext, ccc_id;
    const uint8_t * bp;
    const uint8_t * ip;
    const char * cp;
    sgj_state * jsp = &op->json_st;
    char b[256];
    char d[64];
    static const int blen = sizeof(b);
    static const int dlen = sizeof(d);

    if (jsp->pr_as_json) {
        int ret = filter_json_dev_ids(buff, len, -1, op, jap);

        if (ret || (! jsp->pr_out_hr))
            return;
    }
    if (buff[2] > 2) {  /* SPC-3,4,5 buff[2] is upper byte of length */
        /*
         * Reference the 3rd byte of the first Identification descriptor
         * of a page 83 reply to determine whether the reply is compliant
         * with SCSI-2 or SPC-2/3 specifications.  A zero value in the
         * 3rd byte indicates an SPC-2/3 conforming reply ( the field is
         * reserved ).  This byte will be non-zero for a SCSI-2
         * conforming page 83 reply from these EMC Symmetrix models since
         * the 7th byte of the reply corresponds to the 4th and 5th
         * nibbles of the 6-byte OUI for EMC, that is, 0x006048.
         */
        i_len = len;
        ip = bp = buff;
        c_set = 1;
        assoc = 0;
        piv = 0;
        p_id = 0xf;
        desig_type = 3;
        j = 1;
        off = 16;
        sgj_pr_hr(jsp, "  Pre-SPC descriptor, descriptor length: %d\n",
                  i_len);
        goto decode;
    }

    for (j = 1, off = -1;
         (u = sg_vpd_dev_id_iter(buff, len, &off, -1, -1, -1)) == 0;
         ++j) {
        bp = buff + off;
        i_len = bp[3];
        id_len = i_len + 4;
        sgj_pr_hr(jsp, "  Designation descriptor number %d, "
                  "descriptor length: %d\n", j, id_len);
        if ((off + id_len) > len) {
            pr2serr("%s %s error: designator length longer than\n"
                    "     remaining response length=%d\n", leadin, vpd_pg_s,
                    (len - off));
            return;
        }
        ip = bp + 4;
        p_id = ((bp[0] >> 4) & 0xf);   /* protocol identifier */
        c_set = (bp[0] & 0xf);         /* code set */
        piv = ((bp[1] & 0x80) ? 1 : 0); /* protocol identifier valid */
        assoc = ((bp[1] >> 4) & 0x3);
        desig_type = (bp[1] & 0xf);
  decode:
        if (piv && ((1 == assoc) || (2 == assoc)))
            sgj_pr_hr(jsp, "    transport: %s\n",
                      sg_get_trans_proto_str(p_id, dlen, d));
        n = 0;
        cp = sg_get_desig_type_str(desig_type);
        sg_scn3pr(b, blen, n, "    designator_type: %s,  ", cp ? cp : "-");
        cp = sg_get_desig_code_set_str(c_set);
        sgj_pr_hr(jsp, "%scode_set: %s\n", b, cp ? cp : "-");
        cp = sg_get_desig_assoc_str(assoc);
        sgj_pr_hr(jsp, "    associated with the %s\n", cp ? cp : "-");
        if (op->do_hex) {
            sgj_pr_hr(jsp, "    designator header(hex): %.2x %.2x %.2x %.2x\n",
                   bp[0], bp[1], bp[2], bp[3]);
            sgj_pr_hr(jsp, "    designator:\n");
            hex2stdout(ip, i_len, -1);
            continue;
        }
        switch (desig_type) {
        case 0: /* vendor specific */
            k = 0;
            if ((2 == c_set) || (3 == c_set)) { /* ASCII or UTF-8 */
                for (k = 0; (k < i_len) && isprint(ip[k]); ++k)
                    ;
                if (k >= i_len)
                    k = 1;
            }
            if (k)
                sgj_pr_hr(jsp, "      vendor specific: %.*s\n", i_len, ip);
            else {
                sgj_pr_hr(jsp, "      vendor specific:\n");
                /* plain text output only here */
                hex2stdout(ip, i_len, no_ascii_4hex(op));
            }
            break;
        case 1: /* T10 vendor identification */
            sgj_pr_hr(jsp, "      vendor id: %.8s\n", ip);
            if (i_len > 8) {
                if ((2 == c_set) || (3 == c_set)) { /* ASCII or UTF-8 */
                    sgj_pr_hr(jsp, "      vendor specific: %.*s\n", i_len - 8,
                              ip + 8);
                } else {
                    n = 0;
                    n += sg_scn3pr(b, blen, n, "      vendor specific: 0x");
                    for (m = 8; m < i_len; ++m)
                        n += sg_scn3pr(b, blen, n, "%02x", ip[m]);
                    sgj_pr_hr(jsp, "%s\n", b);
                }
            }
            break;
        case 2: /* EUI-64 based */
            sgj_pr_hr(jsp, "      EUI-64 based %d byte identifier\n", i_len);
            if (1 != c_set) {
                pr2serr("      << expected binary code_set (1)>>\n");
                hex2stderr(ip, i_len, -1);
                break;
            }
            ci_off = 0;
            n = 0;
            b[0] = '\0';
            if (16 == i_len) {
                ci_off = 8;
                id_ext = sg_get_unaligned_be64(ip);
                sg_scn3pr(b, blen, n, "      Identifier extension: 0x%"
                          PRIx64 "\n", id_ext);
            } else if ((8 != i_len) && (12 != i_len)) {
                pr2serr("      << can only decode 8, 12 and 16 "
                        "byte ids>>\n");
                hex2stderr(ip, i_len, -1);
                break;
            }
            ccc_id = sg_get_unaligned_be64(ip + ci_off);
            sgj_pr_hr(jsp, "%s      IEEE identifier: 0x%" PRIx64 "\n", b,
                      ccc_id);
            if (12 == i_len) {
                d_id = sg_get_unaligned_be32(ip + 8);
                sgj_pr_hr(jsp, "      Directory ID: 0x%x\n", d_id);
            }
            n = sg_scnpr(b, blen, "      [0x");
            for (m = 0; m < i_len; ++m)
                n += sg_scn3pr(b, blen, n, "%02x", ip[m]);
            sgj_pr_hr(jsp, "%s]\n", b);
            break;
        case 3: /* NAA <n> */
            naa = (ip[0] >> 4) & 0xff;
            if (1 != c_set) {
                pr2serr("      << expected binary code_set (1), got %d for "
                        "NAA=%d>>\n", c_set, naa);
                hex2stderr(ip, i_len, -1);
                break;
            }
            switch (naa) {
            case 2:     /* NAA 2: IEEE Extended */
                if (8 != i_len) {
                    pr2serr("      << unexpected NAA 2 identifier "
                            "length: 0x%x>>\n", i_len);
                    hex2stderr(ip, i_len, -1);
                    break;
                }
                d_id = (((ip[0] & 0xf) << 8) | ip[1]);
                c_id = sg_get_unaligned_be24(ip + 2);
                vsi = sg_get_unaligned_be24(ip + 5);
                sgj_pr_hr(jsp, "      NAA 2, vendor specific identifier A: "
                          "0x%x\n", d_id);
                sgj_pr_hr(jsp, "      AOI: 0x%x\n", c_id);
                sgj_pr_hr(jsp, "      vendor specific identifier B: 0x%x\n",
                          vsi);
                n = sg_scnpr(b, blen, "      [0x");
                for (m = 0; m < 8; ++m)
                    n += sg_scn3pr(b, blen, n, "%02x", ip[m]);
                sgj_pr_hr(jsp, "%s]\n", b);
                break;
            case 3:     /* NAA 3: Locally assigned */
                if (8 != i_len) {
                    pr2serr("      << unexpected NAA 3 identifier "
                            "length: 0x%x>>\n", i_len);
                    hex2stderr(ip, i_len, -1);
                    break;
                }
                sgj_pr_hr(jsp, "      NAA 3, Locally assigned:\n");
                n = sg_scnpr(b, blen, "      [0x");
                for (m = 0; m < 8; ++m)
                    n += sg_scn3pr(b, blen, n, "%02x", ip[m]);
                sgj_pr_hr(jsp, "%s]\n", b);
                break;
            case 5:     /* NAA 5: IEEE Registered */
                if (8 != i_len) {
                    pr2serr("      << unexpected NAA 5 identifier "
                            "length: 0x%x>>\n", i_len);
                    hex2stderr(ip, i_len, -1);
                    break;
                }
                c_id = (((ip[0] & 0xf) << 20) | (ip[1] << 12) |
                        (ip[2] << 4) | ((ip[3] & 0xf0) >> 4));
                vsei = ip[3] & 0xf;
                for (m = 1; m < 5; ++m) {
                    vsei <<= 8;
                    vsei |= ip[3 + m];
                }
                sgj_pr_hr(jsp, "      NAA 5, AOI: 0x%x\n", c_id);
                n = sg_scnpr(b, blen, "      Vendor Specific Identifier: "
                             "0x%" PRIx64 "\n", vsei);
                n += sg_scn3pr(b, blen, n, "      [0x");
                for (m = 0; m < 8; ++m)
                    n += sg_scn3pr(b, blen, n, "%02x", ip[m]);
                sgj_pr_hr(jsp, "%s]\n", b);
                break;
            case 6:     /* NAA 6: IEEE Registered extended */
                if (16 != i_len) {
                    pr2serr("      << unexpected NAA 6 identifier "
                            "length: 0x%x>>\n", i_len);
                    hex2stderr(ip, i_len, -1);
                    break;
                }
                c_id = (((ip[0] & 0xf) << 20) | (ip[1] << 12) |
                        (ip[2] << 4) | ((ip[3] & 0xf0) >> 4));
                vsei = ip[3] & 0xf;
                for (m = 1; m < 5; ++m) {
                    vsei <<= 8;
                    vsei |= ip[3 + m];
                }
                sgj_pr_hr(jsp, "      NAA 6, AOI: 0x%x\n", c_id);
                sgj_pr_hr(jsp, "      Vendor Specific Identifier: 0x%"
                          PRIx64 "\n", vsei);
                vsei = sg_get_unaligned_be64(ip + 8);
                sgj_pr_hr(jsp, "      Vendor Specific Identifier Extension: "
                          "0x%" PRIx64 "\n", vsei);
                n = sg_scnpr(b, blen, "      [0x");
                for (m = 0; m < 16; ++m)
                    n += sg_scn3pr(b, blen, n, "%02x", ip[m]);
                sgj_pr_hr(jsp, "%s]\n", b);
                break;
            default:
                pr2serr("      << bad NAA nibble , expect 2, 3, 5 or 6, "
                        "got %d>>\n", naa);
                hex2stderr(ip, i_len, -1);
                break;
            }
            break;
        case 4: /* Relative target port */
            if ((1 != c_set) || (1 != assoc) || (4 != i_len)) {
                pr2serr("      << expected binary code_set, target "
                        "port association, length 4>>\n");
                hex2stderr(ip, i_len, -1);
                break;
            }
            d_id = sg_get_unaligned_be16(ip + 2);
            sgj_pr_hr(jsp, "      Relative target port: 0x%x\n", d_id);
            break;
        case 5: /* (primary) Target port group */
            if ((1 != c_set) || (1 != assoc) || (4 != i_len)) {
                pr2serr("      << expected binary code_set, target "
                        "port association, length 4>>\n");
                hex2stderr(ip, i_len, -1);
                break;
            }
            d_id = sg_get_unaligned_be16(ip + 2);
            sgj_pr_hr(jsp, "      Target port group: 0x%x\n", d_id);
            break;
        case 6: /* Logical unit group */
            if ((1 != c_set) || (0 != assoc) || (4 != i_len)) {
                pr2serr("      << expected binary code_set, logical "
                        "unit association, length 4>>\n");
                hex2stderr(ip, i_len, -1);
                break;
            }
            d_id = sg_get_unaligned_be16(ip + 2);
            sgj_pr_hr(jsp, "      Logical unit group: 0x%x\n", d_id);
            break;
        case 7: /* MD5 logical unit identifier */
            if ((1 != c_set) || (0 != assoc)) {
                pr2serr("      << expected binary code_set, logical "
                        "unit association>>\n");
                hex2stderr(ip, i_len, -1);
                break;
            }
            sgj_pr_hr(jsp, "      MD5 logical unit identifier:\n");
            if (jsp->pr_out_hr)
                sgj_hr_str_out(jsp, (const char *)ip, i_len);
            else
                hex2stdout(ip, i_len, -1);
            break;
        case 8: /* SCSI name string */
            if (3 != c_set) {
                if (2 == c_set) {
                    if (op->verbose)
                        pr2serr("      << expected UTF-8, use ASCII>>\n");
                } else {
                    pr2serr("      << expected UTF-8 code_set>>\n");
                    hex2stderr(ip, i_len, -1);
                    break;
                }
            }
            sgj_pr_hr(jsp, "      SCSI name string:\n");
            /* does %s print out UTF-8 ok??
             * Seems to depend on the locale. Looks ok here with my
             * locale setting: en_AU.UTF-8
             */
            sgj_pr_hr(jsp, "      %.*s\n", i_len, (const char *)ip);
            break;
        case 9: /* Protocol specific port identifier */
            /* added in spc4r36, PIV must be set, proto_id indicates */
            /* whether UAS (USB) or SOP (PCIe) or ... */
            if (! piv)
                pr2serr("      >>>> Protocol specific port identifier "
                        "expects protocol\n"
                        "           identifier to be valid and it is not\n");
            if (TPROTO_UAS == p_id) {
                sgj_pr_hr(jsp, "      USB device address: 0x%x\n",
                          0x7f & ip[0]);
                sgj_pr_hr(jsp, "      USB interface number: 0x%x\n", ip[2]);
            } else if (TPROTO_SOP == p_id) {
                sgj_pr_hr(jsp, "      PCIe routing ID, bus number: 0x%x\n",
                          ip[0]);
                sgj_pr_hr(jsp, "          function number: 0x%x\n", ip[1]);
                sgj_pr_hr(jsp, "          [or device number: 0x%x, function "
                          "number: 0x%x]\n", (0x1f & (ip[1] >> 3)),
                          0x7 & ip[1]);
            } else
                sgj_pr_hr(jsp, "      >>>> unexpected protocol identifier: "
                          "%s\n           with Protocol specific port "
                          "identifier\n", sg_get_trans_proto_str(p_id, dlen,
                                                                 d));
            break;
        case 0xa: /* UUID identifier [spc5r08] RFC 4122 */
            if (1 != c_set) {
                pr2serr("      << expected binary code_set >>\n");
                hex2stderr(ip, i_len, no_ascii_4hex(op));
                break;
            }
            if ((1 != ((ip[0] >> 4) & 0xf)) || (18 != i_len)) {
                pr2serr("      << expected locally assigned UUID, 16 bytes "
                        "long >>\n");
                hex2stderr(ip, i_len, no_ascii_4hex(op));
                break;
            }
            n = sg_scnpr(b, blen, "      Locally assigned UUID: ");
            for (m = 0; m < 16; ++m) {
                if ((4 == m) || (6 == m) || (8 == m) || (10 == m))
                    n += sg_scn3pr(b, blen, n, "-");
                n += sg_scn3pr(b, blen, n, "%02x", ip[2 + m]);
            }
            sgj_pr_hr(jsp, "%s\n", b);
            break;
        default: /* reserved */
            pr2serr("      reserved designator=0x%x\n", desig_type);
            hex2stderr(ip, i_len, -1);
            break;
        }
    }
    if (-2 == u)
        pr2serr("%s %s error: around offset=%d\n", leadin, vpd_pg_s, off);
}

/* The --export and --json options are assumed to be mutually exclusive.
 * Here the former takes precedence. */
static void
export_dev_ids(uint8_t * buff, int len, int verbose)
{
    int u, m, id_len, c_set, assoc, desig_type, i_len;
    int off, d_id, naa, k, p_id;
    uint8_t * bp;
    uint8_t * ip;
    const char * assoc_str;
    const char * suffix;

    if (buff[2] != 0) {
        /*
         * Cf decode_dev_ids() for details
         */
        i_len = len;
        ip = buff;
        c_set = 1;
        assoc = 0;
        p_id = 0xf;
        desig_type = 3;
        off = 16;
        goto decode;
    }

    for (off = -1;
         (u = sg_vpd_dev_id_iter(buff, len, &off, -1, -1, -1)) == 0; ) {
        bp = buff + off;
        i_len = bp[3];
        id_len = i_len + 4;
        if ((off + id_len) > len) {
            if (verbose)
                pr2serr("Device Identification %s error: designator length "
                        "longer than\n     remaining response length=%d\n",
                        vpd_pg_s, (len - off));
            return;
        }
        ip = bp + 4;
        p_id = ((bp[0] >> 4) & 0xf);   /* protocol identifier */
        c_set = (bp[0] & 0xf);
        assoc = ((bp[1] >> 4) & 0x3);
        desig_type = (bp[1] & 0xf);
  decode:
        switch (assoc) {
            case 0:
                assoc_str = "LUN";
                break;
            case 1:
                assoc_str = "PORT";
                break;
            case 2:
                assoc_str = "TARGET";
                break;
            default:
                if (verbose)
                    pr2serr("    Invalid association %d\n", assoc);
                return;
        }
        switch (desig_type) {
        case 0: /* vendor specific */
            if (i_len == 0 || i_len > 128)
                break;
            if ((2 == c_set) || (3 == c_set)) { /* ASCII or UTF-8 */
                k = encode_whitespaces(ip, i_len);
                /* udev-conforming character encoding */
                if (k > 0) {
                    printf("SCSI_IDENT_%s_VENDOR=", assoc_str);
                    for (m = 0; m < k; ++m) {
                        if ((ip[m] >= '0' && ip[m] <= '9') ||
                            (ip[m] >= 'A' && ip[m] <= 'Z') ||
                            (ip[m] >= 'a' && ip[m] <= 'z') ||
                            strchr("#+-.:=@_", ip[m]) != NULL)
                            printf("%c", ip[m]);
                        else
                            printf("\\x%02x", ip[m]);
                    }
                    printf("\n");
                }
            } else {
                printf("SCSI_IDENT_%s_VENDOR=", assoc_str);
                for (m = 0; m < i_len; ++m)
                    printf("%02x", (unsigned int)ip[m]);
                printf("\n");
            }
            break;
        case 1: /* T10 vendor identification */
            printf("SCSI_IDENT_%s_T10=", assoc_str);
            if ((2 == c_set) || (3 == c_set)) {
                k = encode_whitespaces(ip, i_len);
                /* udev-conforming character encoding */
                for (m = 0; m < k; ++m) {
                    if ((ip[m] >= '0' && ip[m] <= '9') ||
                        (ip[m] >= 'A' && ip[m] <= 'Z') ||
                        (ip[m] >= 'a' && ip[m] <= 'z') ||
                        strchr("#+-.:=@_", ip[m]) != NULL)
                        printf("%c", ip[m]);
                    else
                        printf("\\x%02x", ip[m]);
                }
                printf("\n");
                if (!memcmp(ip, "ATA_", 4)) {
                    printf("SCSI_IDENT_%s_ATA=%.*s\n", assoc_str,
                           k - 4, ip + 4);
                }
            } else {
                for (m = 0; m < i_len; ++m)
                    printf("%02x", (unsigned int)ip[m]);
                printf("\n");
            }
            break;
        case 2: /* EUI-64 based */
            if (1 != c_set) {
                if (verbose) {
                    pr2serr("      << expected binary code_set (1)>>\n");
                    hex2stderr(ip, i_len, -1);
                }
                break;
            }
            printf("SCSI_IDENT_%s_EUI64=", assoc_str);
            for (m = 0; m < i_len; ++m)
                printf("%02x", (unsigned int)ip[m]);
            printf("\n");
            break;
        case 3: /* NAA */
            if (1 != c_set) {
                if (verbose) {
                    pr2serr("      << expected binary code_set (1)>>\n");
                    hex2stderr(ip, i_len, -1);
                }
                break;
            }
            /*
             * Unfortunately, there are some (broken) implementations
             * which return _several_ NAA descriptors.
             * So add a suffix to differentiate between them.
             */
            naa = (ip[0] >> 4) & 0xff;
            switch (naa) {
                case 6:
                    suffix="REGEXT";
                    break;
                case 5:
                    suffix="REG";
                    break;
                case 2:
                    suffix="EXT";
                    break;
                case 3:
                default:
                    suffix="LOCAL";
                    break;
            }
            printf("SCSI_IDENT_%s_NAA_%s=", assoc_str, suffix);
            for (m = 0; m < i_len; ++m)
                printf("%02x", (unsigned int)ip[m]);
            printf("\n");
            break;
        case 4: /* Relative target port */
            if ((1 != c_set) || (1 != assoc) || (4 != i_len)) {
                if (verbose) {
                    pr2serr("      << expected binary code_set, target "
                            "port association, length 4>>\n");
                    hex2stderr(ip, i_len, -1);
                }
                break;
            }
            d_id = sg_get_unaligned_be16(ip + 2);
            printf("SCSI_IDENT_%s_RELATIVE=%d\n", assoc_str, d_id);
            break;
        case 5: /* (primary) Target port group */
            if ((1 != c_set) || (1 != assoc) || (4 != i_len)) {
                if (verbose) {
                    pr2serr("      << expected binary code_set, target "
                            "port association, length 4>>\n");
                    hex2stderr(ip, i_len, -1);
                }
                break;
            }
            d_id = sg_get_unaligned_be16(ip + 2);
            printf("SCSI_IDENT_%s_TARGET_PORT_GROUP=0x%x\n", assoc_str, d_id);
            break;
        case 6: /* Logical unit group */
            if ((1 != c_set) || (0 != assoc) || (4 != i_len)) {
                if (verbose) {
                    pr2serr("      << expected binary code_set, logical "
                            "unit association, length 4>>\n");
                    hex2stderr(ip, i_len, -1);
                }
                break;
            }
            d_id = sg_get_unaligned_be16(ip + 2);
            printf("SCSI_IDENT_%s_LOGICAL_UNIT_GROUP=0x%x\n", assoc_str, d_id);
            break;
        case 7: /* MD5 logical unit identifier */
            if ((1 != c_set) || (0 != assoc)) {
                if (verbose) {
                    pr2serr("      << expected binary code_set, logical "
                            "unit association>>\n");
                    hex2stderr(ip, i_len, -1);
                }
                break;
            }
            printf("SCSI_IDENT_%s_MD5=", assoc_str);
            hex2stdout(ip, i_len, -1);
            break;
        case 8: /* SCSI name string */
            if (3 != c_set) {
                if (verbose) {
                    pr2serr("      << expected UTF-8 code_set>>\n");
                    hex2stderr(ip, i_len, -1);
                }
                break;
            }
            if (! (strncmp((const char *)ip, "eui.", 4) ||
                   strncmp((const char *)ip, "EUI.", 4) ||
                   strncmp((const char *)ip, "naa.", 4) ||
                   strncmp((const char *)ip, "NAA.", 4) ||
                   strncmp((const char *)ip, "iqn.", 4))) {
                if (verbose) {
                    pr2serr("      << expected name string prefix>>\n");
                    hex2stderr(ip, i_len, -1);
                }
                break;
            }

            printf("SCSI_IDENT_%s_NAME=%.*s\n", assoc_str, i_len,
                   (const char *)ip);
            break;
        case 9: /*  Protocol specific port identifier */
            if (TPROTO_UAS == p_id) {
                if ((4 != i_len) || (1 != assoc)) {
                    if (verbose) {
                        pr2serr("      << UAS (USB) expected target "
                                "port association>>\n");
                        hex2stderr(ip, i_len, -1);
                    }
                    break;
                }
                printf("SCSI_IDENT_%s_UAS_DEVICE_ADDRESS=0x%x\n", assoc_str,
                       ip[0] & 0x7f);
                printf("SCSI_IDENT_%s_UAS_INTERFACE_NUMBER=0x%x\n", assoc_str,
                       ip[2]);
            } else if (TPROTO_SOP == p_id) {
                if ((4 != i_len) && (8 != i_len)) {   /* spc4r36h confused */
                    if (verbose) {
                        pr2serr("      << SOP (PCIe) descriptor "
                                "length=%d >>\n", i_len);
                        hex2stderr(ip, i_len, -1);
                    }
                    break;
                }
                printf("SCSI_IDENT_%s_SOP_ROUTING_ID=0x%x\n", assoc_str,
                       sg_get_unaligned_be16(ip + 0));
            } else {
                pr2serr("      << Protocol specific port identifier "
                        "protocol_id=0x%x>>\n", p_id);
            }
            break;
        case 0xa: /* UUID based */
            if (1 != c_set) {
                if (verbose) {
                    pr2serr("      << expected binary code_set (1)>>\n");
                    hex2stderr(ip, i_len, 0);
                }
                break;
            }
            if (i_len < 18) {
                if (verbose) {
                    pr2serr("      << short UUID field expected 18 or more, "
                            "got %d >>\n", i_len);
                    hex2stderr(ip, i_len, 0);
                }
                break;
            }
            printf("SCSI_IDENT_%s_UUID=", assoc_str);
            for (m = 2; m < i_len; ++m) {
                if ((6 == m) || (8 == m) || (10 == m) || (12 == m))
                    printf("-%02x", (unsigned int)ip[m]);
                else
                    printf("%02x", (unsigned int)ip[m]);
            }
            printf("\n");
            break;
        default: /* reserved */
            if (verbose) {
                pr2serr("      reserved designator=0x%x\n", desig_type);
                hex2stderr(ip, i_len, 0);
            }
            break;
        }
    }
    if (-2 == u && verbose)
        pr2serr("%s error: around offset=%d\n", di_vpdp, off);
}

/* VPD_BLOCK_LIMITS  0xb0 ["bl"]  (SBC) */
/* VPD_SA_DEV_CAP  0xb0 ["sad"]  (SSC) */
/* Sequential access device characteristics,  ssc+smc */
/* OSD information, osd */
static void
decode_b0_vpd(uint8_t * buff, int len, struct opts_t * op, sgj_opaque_p jop)
{
    int pdt = PDT_MASK & buff[0];
    int dhex = op->do_hex;
    const char * vpd_pp = NULL;
    sgj_state * jsp = &op->json_st;

    if (dhex < 0)
        dhex = -dhex;
    if ((dhex > 0) && (dhex < 3)) {
        hex2stdout(buff, len, no_ascii_4hex(op));
        return;
    }

    switch (pdt) {
    case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
        /* done by decode_block_limits_vpd() */
        break;
    case PDT_TAPE: case PDT_MCHANGER:
        if (dhex > 2) {
            vpd_pp = sad_vpdp;
            break;
        }
        sgj_haj_vi_nex(jsp, jop, 2, "TSMC", SGJ_SEP_EQUAL_NO_SPACE,
                       !!(buff[4] & 0x2), false, "Tape Stream Mirror "
                       "Capable");
        sgj_haj_vi_nex(jsp, jop, 2, "WORM", SGJ_SEP_EQUAL_NO_SPACE,
                       !!(buff[4] & 0x1), false, "Write Once Read Multiple "
                       "supported");

        break;
    case PDT_OSD:
        if (dhex > 2) {
            vpd_pp = osdi_vpdp;
            break;
        }
        /* fall-through */
    default:
        pr2serr("  Unable to decode pdt=0x%x, in hex:\n", pdt);
        hex2stderr(buff, len, 0);
        break;
    }
    if (vpd_pp)
        named_hhh_output(vpd_pp, buff, len, op);
}

/* VPD_BLOCK_DEV_CHARS sbc  0xb1 ["bdc"] */
/* VPD_MAN_ASS_SN ssc */
static void
decode_b1_vpd(uint8_t * buff, int len, struct opts_t * op, sgj_opaque_p jop)
{
    int pdt;
    sgj_state * jsp = &op->json_st;

    if (op->do_hex > 0) {
        hex2stdout(buff, len, no_ascii_4hex(op));
        return;
    }
    pdt = PDT_MASK & buff[0];
    switch (pdt) {
    case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
        /* now done by decode_block_dev_ch_vpd() in sg_vpd_common.c */
        break;
    case PDT_TAPE: case PDT_MCHANGER: case PDT_ADC:
        sgj_pr_hr(jsp, "  Manufacturer-assigned serial number: %.*s\n",
                  len - 4, buff + 4);
        sgj_js_nv_s_len(jsp, jop, "manufacturer_assigned_serial_number",
                        (const char *)buff + 4, len - 4);
        break;
    default:
        pr2serr("  Unable to decode pdt=0x%x, in hex:\n", pdt);
        hex2stderr(buff, len, 0);
        break;
    }
}

/* VPD_REFERRALS sbc          0xb3 ["ref"] */
/* VPD_AUTOMATION_DEV_SN ssc  0xb3 ["adsn"] */
static void
decode_b3_vpd(uint8_t * buff, int len, struct opts_t * op, sgj_opaque_p jop)
{
    int pdt;
    sgj_state * jsp = &op->json_st;

    if (op->do_hex > 0) {
        hex2stdout(buff, len, no_ascii_4hex(op));
        return;
    }
    pdt = buff[0] & PDT_MASK;
    switch (pdt) {
    case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
        /* now done in decode_referrals_vpd() in sg_vpd_common.c */
        break;
    case PDT_TAPE: case PDT_MCHANGER:
        sgj_pr_hr(jsp, "  Automation device serial number: %.*s\n",
                  len - 4, buff + 4);
        sgj_js_nv_s_len(jsp, jop, "automation_device_serial_number",
                        (const char *)buff + 4, len - 4);
        break;
    default:
        pr2serr("  Unable to decode pdt=0x%x, in hex:\n", pdt);
        hex2stderr(buff, len, 0);
        break;
    }
}

static const char *
find_version_descriptor_str(int value)
{
#ifdef SG_SCSI_STRINGS
    int k;
    const struct sg_lib_simple_value_name_t * vdp;

    for (k = 0; ((vdp = sg_lib_version_descriptor_arr + k) && vdp->name);
         ++k) {
        if (value == vdp->value)
            return vdp->name;
        if (value < vdp->value)
            break;
    }
    return NULL;
#else
    if (value) { }      /* defeat warning */
    return "    ";
#endif
}

static void
std_inq_decode(const uint8_t * rp, int len, struct opts_t * op,
               sgj_opaque_p jop)
{
    uint8_t ansi_version;
    int pqual, pdt, k, j, rsp_len, gv_len;
    int vb = op->verbose;
    sgj_state * jsp = &op->json_st;
    bool as_json = jsp->pr_as_json;
    const char * cp;
    int vdesc_arr[8];
    char b[128];
    static const int blen = sizeof(b);

    if (len < 4) {
        pr2serr("%s: len [%d] too short\n", __func__, len);
        return;
    }
    memset(vdesc_arr, 0, sizeof(vdesc_arr));
    if (op->do_raw) {
        dStrRaw((const char *)rp, len);
        return;
    } else if (op->do_hex > 0) {
        /* with -H, print with address, -HH without */
        hex2stdout(rp, len, no_ascii_4hex(op));
        return;
    }
    pqual = (rp[0] & 0xe0) >> 5;
    if (! op->do_raw && ! op->do_export) {
        strcpy(b, "standard INQUIRY:");
        if (0 == pqual)
            sgj_pr_hr(jsp, "%s\n", b);
        else if (1 == pqual)
            sgj_pr_hr(jsp, "%s [PQ indicates LU temporarily unavailable]\n",
                      b);
        else if (3 == pqual)
            sgj_pr_hr(jsp, "%s [PQ indicates LU not accessible via this "
                      "port]\n", b);
        else
            sgj_pr_hr(jsp, "%s [reserved or vendor specific qualifier "
                      "[%d]]\n", b, pqual);
    }
    gv_len = len;
    if (rp[4] > 0)
        rsp_len = rp[4] + 5;
    else {      /* not looking good if we are here */
        if (len >= SINQ_COMMON_RESP_LEN) {
            if (vb > 1)
                pr2serr("%s: malformed but got enough, assume 36 bytes "
                        "long\n", __func__);
            rsp_len = SINQ_COMMON_RESP_LEN;
        } else
            rsp_len = 5;
    }
    if (vb > 2)
        pr2serr(">> requested %d bytes, %d bytes available\n", len, rsp_len);
    if (rsp_len < len)
        len = rsp_len;

    /* N.B. rp[2] full byte is 'version' in SPC-2,3,4 but in SPC [spc-r11a "
     * (1997)] bits 6,7: ISO/IEC version; bits 3-5: ECMA version; bits 0-2:
     * SCSI version. Pattern broken by SPC-6 which is 0xd .*/
    ansi_version = rp[2] & 0xf;
    pdt = rp[0] & PDT_MASK;
    if (op->do_export) {
        printf("SCSI_TPGS=%d\n", (rp[5] & 0x30) >> 4);
        cp = sg_get_pdt_str(pdt, blen, b);
        if (strlen(cp) > 0)
            printf("SCSI_TYPE=%s\n", cp);
    } else {
        sgj_pr_hr(jsp, "  PQual=%d  PDT=%d  RMB=%d  LU_CONG=%d  "
                  "hot_pluggable=%d  version=0x%02x ", pqual, pdt,
                  !!(rp[1] & 0x80), !!(rp[1] & 0x40), (rp[1] >> 4) & 0x3,
                  (unsigned int)rp[2]);
        sgj_pr_hr(jsp, " [%s]\n",
                  sg_get_scsi_ansi_version_str(ansi_version, blen, b));
        sgj_pr_hr(jsp, "  [AERC=%d]  [TrmTsk=%d]  NormACA=%d  HiSUP=%d "
                  " Resp_data_format=%d\n  SCCS=%d  ", !!(rp[3] & 0x80),
                  !!(rp[3] & 0x40), !!(rp[3] & 0x20), !!(rp[3] & 0x10),
                  rp[3] & 0x0f, !!(rp[5] & 0x80));
        sgj_pr_hr(jsp, "ACC=%d  TPGS=%d  3PC=%d  Protect=%d ",
                  !!(rp[5] & 0x40), ((rp[5] & 0x30) >> 4), !!(rp[5] & 0x08),
                  !!(rp[5] & 0x01));
        sgj_pr_hr(jsp, " [BQue=%d]\n  EncServ=%d  ", !!(rp[6] & 0x80),
                  !!(rp[6] & 0x40));
        if (rp[6] & 0x10)
            sgj_pr_hr(jsp, "MultiP=1 (VS=%d)  ", !!(rp[6] & 0x20));
        else
            sgj_pr_hr(jsp, "MultiP=0  ");
        sgj_pr_hr(jsp, "[MChngr=%d]  [ACKREQQ=%d]  Addr16=%d\n  "
                  "[RelAdr=%d]  ", !!(rp[6] & 0x08), !!(rp[6] & 0x04),
                  !!(rp[6] & 0x01), !!(rp[7] & 0x80));
        sgj_pr_hr(jsp, "WBus16=%d  Sync=%d  [Linked=%d]  [TranDis=%d]  ",
                  !!(rp[7] & 0x20), !!(rp[7] & 0x10), !!(rp[7] & 0x08),
                  !!(rp[7] & 0x04));
        sgj_pr_hr(jsp, "CmdQue=%d\n", !!(rp[7] & 0x02));
        if (len > 56)
            sgj_pr_hr(jsp, "  [SPI: Clocking=0x%x  QAS=%d  IUS=%d]\n",
                      (rp[56] & 0x0c) >> 2, !!(rp[56] & 0x2),
                      !!(rp[56] & 0x1));
        if (gv_len >= len)
            sgj_pr_hr(jsp, "    length=%d (0x%x)", len, len);
        else
            sgj_pr_hr(jsp, "    length=%d (0x%x), but only fetched %d bytes",
                      len, len, gv_len);
        if ((ansi_version >= 2) && (len < SINQ_COMMON_RESP_LEN))
            sgj_pr_hr(jsp, "\n  [for SCSI>=2, len>=36 is expected]");
        cp = sg_get_pdt_str(pdt, blen, b);
        if (strlen(cp) > 0)
            sgj_pr_hr(jsp, "   Peripheral device type: %s\n", cp);
    }
    if (len <= 8) {
        if (! op->do_export)
            sgj_pr_hr(jsp, " Inquiry response length=%d, no vendor, product "
                      "or revision data\n", len);
    } else {
        int i;

        memcpy(xtra_buff, &rp[8], 8);
        xtra_buff[8] = '\0';
        /* Fixup any tab characters */
        for (i = 0; i < 8; ++i)
            if (xtra_buff[i] == 0x09)
                xtra_buff[i] = ' ';
        if (op->do_export) {
            int vlen = encode_whitespaces((uint8_t *)xtra_buff, 8);
            if (vlen > 0) {
                printf("SCSI_VENDOR=%s\n", xtra_buff);
                encode_string(xtra_buff, &rp[8], 8);
                printf("SCSI_VENDOR_ENC=%s\n", xtra_buff);
            }
        } else
            sgj_pr_hr(jsp, "  Vendor identification: %s\n", xtra_buff);
        if (len <= 16) {
            if (! op->do_export)
                sgj_pr_hr(jsp, "  Product identification: <none>\n");
        } else {
            memcpy(xtra_buff, &rp[16], 16);
            xtra_buff[16] = '\0';
            if (op->do_export) {
                int mlen = encode_whitespaces((uint8_t *)xtra_buff, 16);
                if (mlen > 0) {
                    printf("SCSI_MODEL=%s\n", xtra_buff);
                    encode_string(xtra_buff, &rp[16], 16);
                    printf("SCSI_MODEL_ENC=%s\n", xtra_buff);
                }
            } else
                sgj_pr_hr(jsp, "  Product identification: %s\n", xtra_buff);
        }
        if (len <= 32) {
            if (! op->do_export)
                sgj_pr_hr(jsp, "  Product revision level: <none>\n");
        } else {
            memcpy(xtra_buff, &rp[32], 4);
            xtra_buff[4] = '\0';
            if (op->do_export) {
                int rlen = encode_whitespaces((uint8_t *)xtra_buff, 4);
                if (rlen > 0)
                    printf("SCSI_REVISION=%s\n", xtra_buff);
            } else
                sgj_pr_hr(jsp, "  Product revision level: %s\n", xtra_buff);
        }
        if (op->do_vendor && (len > 36) && ('\0' != rp[36]) &&
            (' ' != rp[36])) {
            memcpy(xtra_buff, &rp[36], len < 56 ? len - 36 : 20);
            if (op->do_export) {
                int vlen = encode_whitespaces((uint8_t *)xtra_buff, 20);
                if (vlen > 0)
                    printf("VENDOR_SPECIFIC=%s\n", xtra_buff);
            } else
                sgj_pr_hr(jsp, "  Vendor specific: %s\n", xtra_buff);
        }
        if (op->do_descriptors) {
            for (j = 0, k = 58; ((j < 8) && ((k + 1) < len));
                 k +=2, ++j)
                vdesc_arr[j] = sg_get_unaligned_be16(rp + k);
        }
        if ((op->do_vendor > 1) && (len > 96)) {
            memcpy(xtra_buff, &rp[96], len - 96);
            if (op->do_export) {
                int vlen = encode_whitespaces((uint8_t *)xtra_buff,
                                         len - 96);
                if (vlen > 0)
                    printf("VENDOR_SPECIFIC=%s\n", xtra_buff);
            } else
                sgj_pr_hr(jsp, "  Vendor specific: %s\n", xtra_buff);
        }
        if (op->do_vendor && (len > 243) &&
            (0 == strncmp("OPEN-V", (const char *)&rp[16], 6))) {
           memcpy(xtra_buff, &rp[212], 32);
           if (op->do_export) {
                int vlen = encode_whitespaces((uint8_t *)xtra_buff, 32);
                if (vlen > 0)
                    printf("VENDOR_SPECIFIC_OPEN-V_LDEV_NAME=%s\n", xtra_buff);
            } else
                sgj_pr_hr(jsp, "  Vendor specific OPEN-V LDEV Name: %s\n",
                          xtra_buff);
        }
    }
    if (! op->do_export) {
        sgj_opaque_p jo2p = NULL;

        if (as_json)
            jo2p = std_inq_decode_js(rp, len, op, jop);
        if ((0 == len) && usn_buff[0])
            sgj_pr_hr(jsp, "  Unit serial number: %s\n", usn_buff);
        if (op->do_descriptors) {
            sgj_opaque_p jap = sgj_named_subarray_r(jsp, jo2p,
                                                "version_descriptor_list");
            if (0 == vdesc_arr[0]) {
                sgj_pr_hr(jsp, "\n");
                sgj_pr_hr(jsp, "  No version descriptors available\n");
            } else {
                sgj_pr_hr(jsp, "\n");
#ifdef SG_SCSI_STRINGS
                sgj_pr_hr(jsp, "  Version descriptors:\n");
#else
                sgj_pr_hr(jsp, "  Version descriptors [--disable-scsistrings "
                          "active]:\n");
#endif
                for (k = 0; k < 8; ++k) {
                    sgj_opaque_p jo3p = sgj_new_unattached_object_r(jsp);
                    int vdv = vdesc_arr[k];

                    if (0 == vdv)
                        break;
                    cp = find_version_descriptor_str(vdv);
                    if (cp) {
                        if (' ' == *cp)
                            sgj_pr_hr(jsp, "    code: 0x%x\n", vdv);
                        else
                            sgj_pr_hr(jsp, "    %s\n", cp);
                    } else
                        sgj_pr_hr(jsp, "    [unrecognised version descriptor "
                                  "code: 0x%x]\n", vdv);
                    sgj_js_nv_ihexstr(jsp, jo3p, "version_descriptor", vdv,
                                      NULL, cp ? cp : "unknown");
                    sgj_js_nv_o(jsp, jap, NULL /* name */, jo3p);
                }
            }
        }
    }
}

/* Returns 0 if Unit Serial Number VPD page contents found, else see
 * sg_ll_inquiry_pt() return values */
static int
fetch_unit_serial_num(struct sg_pt_base * ptvp, char * obuff, int obuff_len,
                      int vb)
{
    int len, k, res, c;
    uint8_t * b;
    uint8_t * free_b;

    b = sg_memalign(DEF_ALLOC_LEN, 0, &free_b, false);
    if (NULL == b) {
        if (vb > 0)
            pr2serr("%s: unable to allocate on heap\n", __func__);
        return sg_convert_errno(ENOMEM);
    }
    res = vpd_fetch_page(ptvp, b, VPD_SUPPORTED_VPDS,
                         -1 /* 1 byte alloc_len */, true, vb, &len);
    if (res) {
        if (vb > 2)
            pr2serr("%s: no supported VPDs page\n", __func__);
        res = SG_LIB_CAT_MALFORMED;
        goto fini;
    }
    if (! vpd_page_is_supported(b, len, VPD_UNIT_SERIAL_NUM, vb)) {
        res = sg_convert_errno(EDOM); /* was SG_LIB_CAT_ILLEGAL_REQ */
        goto fini;
    }

    memset(b, 0xff, 4); /* guard against empty response, always quiet */
    res = vpd_fetch_page(ptvp, b, VPD_UNIT_SERIAL_NUM, -1, true, vb, &len);
    if ((0 == res) && (len > 3)) {
        len -= 4;
        len = (len < (obuff_len - 1)) ? len : (obuff_len - 1);
        if (len > 0) {
            /* replace non-printable characters (except NULL) with space */
            for (k = 0; k < len; ++k) {
                c = b[4 + k];
                if (c)
                    obuff[k] = isprint(c) ? c : ' ';
                else
                    break;
            }
            obuff[k] = '\0';
            res = 0;
            goto fini;
        } else {
            if (vb > 2)
                pr2serr("%s: bad sn %s\n", __func__, vpd_pg_s);
            res = SG_LIB_CAT_MALFORMED;
        }
    } else {
        if (vb > 2)
            pr2serr("%s: no supported VPDs page\n", __func__);
        res = SG_LIB_CAT_MALFORMED;
    }
fini:
    if (free_b)
        free(free_b);
    return res;
}


/* Process a standard INQUIRY data format (response).
 * Returns 0 if successful */
static int
std_inq_process(struct sg_pt_base * ptvp, struct opts_t * op,
                sgj_opaque_p jop, int off)
{
    int res, len, rlen, act_len;
    int vb, resid;
    char buff[48];

    rlen = (op->maxlen > 0) ? op->maxlen : SINQ_COMMON_RESP_LEN;
    vb = op->verbose;
    if (NULL == ptvp) {    /* assume --inhex=FD usage */
        std_inq_decode(rsp_buff + off, rlen, op, jop);
        return 0;
    }
    res = sg_ll_inquiry_pt(ptvp, false, 0, rsp_buff, rlen, DEF_PT_TIMEOUT,
                           &resid, false, vb);
    if (0 == res) {
        if ((vb > 4) && ((rlen - resid) > 0)) {
            pr2serr("Safe (36 byte) Inquiry response:\n");
            hex2stderr(rsp_buff, rlen - resid, 0);
        }
        len = rsp_buff[4] + 5;
        if ((len > SINQ_COMMON_RESP_LEN) && (len < 256) &&
            (0 == op->maxlen)) {
            rlen = len;
            memset(rsp_buff, 0, rlen);
            if (sg_ll_inquiry_pt(ptvp, false, 0, rsp_buff, rlen,
                                 DEF_PT_TIMEOUT, &resid, true, vb)) {
                pr2serr("second INQUIRY (%d byte) failed\n", len);
                return SG_LIB_CAT_OTHER;
            }
            if (len != (rsp_buff[4] + 5)) {
                pr2serr("strange, consecutive INQUIRYs yield different "
                        "'additional lengths'\n");
                len = rsp_buff[4] + 5;
            }
        }
        if (op->maxlen > 0)
            act_len = rlen;
        else
            act_len = (rlen < len) ? rlen : len;
        /* don't use more than HBA's resid says was transferred from LU */
        if (act_len > (rlen - resid))
            act_len = rlen - resid;
        if (act_len < SINQ_COMMON_RESP_LEN)
            rsp_buff[act_len] = '\0';
        if ((! op->do_only) && (! op->do_export) && (0 == op->maxlen)) {
            if (fetch_unit_serial_num(ptvp, usn_buff, sizeof(usn_buff), vb))
                usn_buff[0] = '\0';
        }
        std_inq_decode(rsp_buff, act_len, op, jop);
        return 0;
    } else if (res < 0) { /* could be an ATA device */
#if defined(SG_LIB_LINUX) && defined(SG_SCSI_STRINGS) && \
    defined(HDIO_GET_IDENTITY)
        /* Try an ATA Identify Device command */
        res = try_ata_identify(ptvp, op->do_hex, op->do_raw, vb);
        if (0 != res) {
            pr2serr("SCSI INQUIRY, NVMe Identify and fetching ATA "
                    "information failed on %s\n", op->device_name);
            return (res < 0) ? SG_LIB_CAT_OTHER : res;
        }
#else
        pr2serr("SCSI INQUIRY failed on %s, res=%d\n",
                op->device_name, res);
        return res;
#endif
    } else {
        char b[80];

        if (vb > 0) {
            pr2serr("    inquiry: failed requesting %d byte response: ", rlen);
            if (resid && (vb > 1))
                snprintf(buff, sizeof(buff), " [resid=%d]", resid);
            else
                buff[0] = '\0';
            sg_get_category_sense_str(res, sizeof(b), b, vb);
            pr2serr("%s%s\n", b, buff);
        }
        return res;
    }
    return 0;
}

#ifdef SG_SCSI_STRINGS
/* Returns 0 if successful */
static int
cmddt_process(int sg_fd, const struct opts_t * op)
{
    int k, j, num, len, pdt, reserved_cmddt, support_num, res;
    char op_name[128];

    memset(rsp_buff, 0, rsp_buff_sz);
    if (op->do_cmddt > 1) {
        printf("Supported command list:\n");
        for (k = 0; k < 256; ++k) {
            res = sg_ll_inquiry(sg_fd, true /* cmddt */, false, k, rsp_buff,
                                DEF_ALLOC_LEN, true, op->verbose);
            if (0 == res) {
                pdt = rsp_buff[0] & PDT_MASK;
                support_num = rsp_buff[1] & 7;
                reserved_cmddt = rsp_buff[4];
                if ((3 == support_num) || (5 == support_num)) {
                    num = rsp_buff[5];
                    for (j = 0; j < num; ++j)
                        printf(" %.2x", (int)rsp_buff[6 + j]);
                    if (5 == support_num)
                        printf("  [vendor specific manner (5)]");
                    sg_get_opcode_name((uint8_t)k, pdt,
                                       sizeof(op_name) - 1, op_name);
                    op_name[sizeof(op_name) - 1] = '\0';
                    printf("  %s\n", op_name);
                } else if ((4 == support_num) || (6 == support_num))
                    printf("  opcode=0x%.2x vendor specific (%d)\n",
                           k, support_num);
                else if ((0 == support_num) && (reserved_cmddt > 0)) {
                    printf("  opcode=0x%.2x ignored cmddt bit, "
                           "given standard INQUIRY response, stop\n", k);
                    break;
                }
            } else if (SG_LIB_CAT_ILLEGAL_REQ == res)
                break;
            else {
                pr2serr("CmdDt INQUIRY on opcode=0x%.2x: failed\n", k);
                break;
            }
        }
    }
    else {
        res = sg_ll_inquiry(sg_fd, true /* cmddt */, false, op->vpd_pn,
                            rsp_buff, DEF_ALLOC_LEN, true, op->verbose);
        if (0 == res) {
            pdt = rsp_buff[0] & PDT_MASK;
            if (! op->do_raw) {
                printf("CmdDt INQUIRY, opcode=0x%.2x:  [", op->vpd_pn);
                sg_get_opcode_name((uint8_t)op->vpd_pn, pdt,
                                   sizeof(op_name) - 1, op_name);
                op_name[sizeof(op_name) - 1] = '\0';
                printf("%s]\n", op_name);
            }
            len = rsp_buff[5] + 6;
            reserved_cmddt = rsp_buff[4];
            if (op->do_raw)
                dStrRaw((const char *)rsp_buff, len);
            else if (op->do_hex > 0)
                hex2stdout(rsp_buff, len, no_ascii_4hex(op));
            else {
                bool prnt_cmd = false;
                const char * desc_p;

                support_num = rsp_buff[1] & 7;
                num = rsp_buff[5];
                switch (support_num) {
                case 0:
                    if (0 == reserved_cmddt)
                        desc_p = "no data available";
                    else
                        desc_p = "ignored cmddt bit, standard INQUIRY "
                                 "response";
                    break;
                case 1: desc_p = "not supported"; break;
                case 2: desc_p = "reserved (2)"; break;
                case 3: desc_p = "supported as per standard";
                        prnt_cmd = true;
                        break;
                case 4: desc_p = "vendor specific (4)"; break;
                case 5: desc_p = "supported in vendor specific way";
                        prnt_cmd = true;
                        break;
                case 6: desc_p = "vendor specific (6)"; break;
                case 7: desc_p = "reserved (7)"; break;
                }
                if (prnt_cmd) {
                    printf("  Support field: %s [", desc_p);
                    for (j = 0; j < num; ++j)
                        printf(" %.2x", (int)rsp_buff[6 + j]);
                    printf(" ]\n");
                } else
                    printf("  Support field: %s\n", desc_p);
            }
        } else if (SG_LIB_CAT_ILLEGAL_REQ != res) {
            if (! op->do_raw) {
                printf("CmdDt INQUIRY, opcode=0x%.2x:  [", op->vpd_pn);
                sg_get_opcode_name((uint8_t)op->vpd_pn, 0,
                                   sizeof(op_name) - 1, op_name);
                op_name[sizeof(op_name) - 1] = '\0';
                printf("%s]\n", op_name);
            }
            pr2serr("CmdDt INQUIRY on opcode=0x%.2x: failed\n", op->vpd_pn);
        }
    }
    return res;
}

#else /* SG_SCSI_STRINGS */

/* Returns 0. */
static int
cmddt_process(int sg_fd, const struct opts_t * op)
{
    if (sg_fd) { }      /* suppress warning */
    if (op) { }         /* suppress warning */
    pr2serr("'--cmddt' not implemented, use sg_opcodes\n");
    return 0;
}

#endif /* SG_SCSI_STRINGS */


/* Returns 0 if successful */
static int
vpd_mainly_hex(struct sg_pt_base * ptvp, struct opts_t * op, sgj_opaque_p jop,
               int off)
{
    bool as_json;
    int res, len, n;
    int dhex = op->do_hex;
    sgj_state * jsp = &op->json_st;
    const char * cp;
    uint8_t * rp;
    char b[144];
    static const int blen = sizeof(b);

    if (dhex < 0)
        dhex = -dhex;
    as_json = jsp->pr_as_json;
    rp = rsp_buff + off;
    if ((! op->do_raw) && (dhex < 3)) {
        if (dhex > 0)
            printf("VPD INQUIRY, page code=0x%.2x:\n", op->vpd_pn);
        else
            sgj_pr_hr(jsp, "VPD INQUIRY, page code=0x%.2x:\n", op->vpd_pn);
    }
    if (NULL == ptvp) {
        len = sg_get_unaligned_be16(rp + 2) + 4;
        res = 0;
    } else {
        memset(rp, 0, DEF_ALLOC_LEN);
        res = vpd_fetch_page(ptvp, rp, op->vpd_pn, op->maxlen,
                             op->do_quiet, op->verbose, &len);
    }
    if (0 == res) {
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            int pdt = rp[0] & PDT_MASK;

            if (0 == op->vpd_pn)
                decode_supported_vpd_4inq(rp, len, op, NULL);
            else {
                if (op->verbose) {
                    cp = sg_get_pdt_str(pdt, blen, b);
                    if (dhex > 0)
                        printf("   [PQual=%d  Peripheral device type: %s]\n",
                               (rp[0] & 0xe0) >> 5, cp);
                    else
                        sgj_pr_hr(jsp, "   [PQual=%d  Peripheral device "
                                  "type: %s]\n", (rp[0] & 0xe0) >> 5, cp);
                }
                if ((0 == dhex) && (len > 0) && (len <= UINT16_MAX)) {
                    char * p;

                    n = (len * 4) + 64;
                    p = (char *)malloc(n);
                    if (p) {
                        n = hex2str(rp, len, NULL, 0, n, p);
                        if (jsp->pr_out_hr)
                            sgj_hr_str_out(jsp, p, n);
                        else
                            sgj_pr_hr(jsp, "%s\n", p);
                        free(p);
                    }
                    if (as_json)
                        sgjv_js_hex_long(jsp, jop, rp, len);
                } else {
                    if (dhex > 2)
                        named_hhh_output(NULL, rp, len, op);
                    else
                        hex2stdout(rp, len, no_ascii_4hex(op));
                }
            }
        }
    } else {
        if (SG_LIB_CAT_ILLEGAL_REQ == res)
            pr2serr("    inquiry: field in cdb illegal (page not "
                    "supported)\n");
        else {
            sg_get_category_sense_str(res, blen, b, op->verbose);
            pr2serr("    inquiry: %s\n", b);
        }
    }
    return res;
}

static int
recurse_vpd_decode(struct opts_t * op, sgj_opaque_p jop, int off)
{
    return vpd_decode(NULL, op, jop, off);
}

/* Returns 0 if successful */
static int
vpd_decode(struct sg_pt_base * ptvp, struct opts_t * op, sgj_opaque_p jop,
           int off)
{
    bool bad = false;
    bool qt = op->do_quiet;
    int len, pdt, pn, vb /*, pqual */;
    int res = 0;
    int dhex = op->do_hex;
    sgj_state * jsp = &op->json_st;
    bool as_json = jsp->pr_as_json;
    sgj_opaque_p jo2p = NULL;
    sgj_opaque_p jap = NULL;
    const char * np;
    const char * ep = "";
    uint8_t * rp;
    char b[144];
    static const int blen = sizeof(b);

    if (dhex < 0)
        dhex = -dhex;
    rp = rsp_buff + off;
    vb = op->verbose;
    if ((off > 0) && (VPD_NOPE_WANT_STD_INQ != op->vpd_pn))
        pn = rp[1];
    else
        pn = op->vpd_pn;
    if (ptvp && (! op->do_force) && (pn != VPD_SUPPORTED_VPDS)) {
        res = vpd_fetch_page(ptvp, rp, VPD_SUPPORTED_VPDS, op->maxlen,
                             qt, vb, &len);
        if (res)
            goto out;
        if (! vpd_page_is_supported(rp, len, pn, vb)) {
            if (vb)
                pr2serr("Given %s not in supported list, use --force to "
                        "override this check\n", vpd_pg_s);
            res = sg_convert_errno(EDOM); /* was SG_LIB_CAT_ILLEGAL_REQ */
            goto out;
        }
    }
    switch (pn) {
    case VPD_SUPPORTED_VPDS:            /* 0x0  ["sv"] */
        np = svp_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else if (dhex)
            hex2stdout(rp, len, no_ascii_4hex(op));
        else {
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "supported_vpd_page_list");
            }
            decode_supported_vpd_4inq(rp, len, op, jap);
        }
        break;
    case VPD_UNIT_SERIAL_NUM:           /* 0x80  ["sn"] */
        np = usn_vpdp;
        if (! op->do_raw && ! op->do_export && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else if (dhex > 0)
            hex2stdout(rp, len, no_ascii_4hex(op));
        else {
            char obuff[DEF_ALLOC_LEN];
            int k, m;

            memset(obuff, 0, sizeof(obuff));
            len -= 4;
            if (len >= (int)sizeof(obuff))
                len = sizeof(obuff) - 1;
            memcpy(obuff, rp + 4, len);
            if (op->do_export) {
                k = encode_whitespaces((uint8_t *)obuff, len);
                if (k > 0) {
                    printf("SCSI_IDENT_SERIAL=");
                    /* udev-conforming character encoding */
                    for (m = 0; m < k; ++m) {
                        if ((obuff[m] >= '0' && obuff[m] <= '9') ||
                            (obuff[m] >= 'A' && obuff[m] <= 'Z') ||
                            (obuff[m] >= 'a' && obuff[m] <= 'z') ||
                            strchr("#+-.:=@_", obuff[m]) != NULL)
                            printf("%c", obuff[m]);
                        else
                            printf("\\x%02x", obuff[m]);
                    }
                    printf("\n");
                }
            } else {
                if (as_json)
                    jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                k = encode_unicode((uint8_t *)obuff, len);
                if (k > 0) {
                    sgj_pr_hr(jsp, "  Unit serial number: %s\n", obuff);
                    sgj_js_nv_s(jsp, jo2p, "unit_serial_number", obuff);
                }
            }
        }
        break;
    case VPD_DEVICE_ID:         /* 0x83  ["di"] */
        np = "Device Identification VPD page";
        if (! op->do_raw && ! op->do_export && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else if (dhex > 2)
            hex2stdout(rp, len, -1);
        else if (op->do_export && (! as_json))
            export_dev_ids(rp + 4, len - 4, op->verbose);
        else {
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "designation_descriptor_list");
            }
            decode_id_vpd(rp, len, op, jap);
        }
        break;
    case VPD_SOFTW_INF_ID:      /* 0x84  ["sii"] */
        np = "Software interface identification VPD page";
        if (! op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "software_interface_identifier_list");
            }
            decode_softw_inf_id(rp, len, op, jap);
        }
        break;
    case VPD_MAN_NET_ADDR:    /* 0x85 ["mna"] */
        np = mna_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            // pdt = rp[0] & PDT_MASK;
            // pdt_str = sg_get_pdt_str(pdt, sizeof(d), d);
            // pqual = (rp[0] & 0xe0) >> 5;
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "network_services_descriptor_list");
            }
            decode_man_net_vpd(rp, len, op, jap);
        }
        break;
    case VPD_EXT_INQ:           /* 0x86  ["ei"] */
        np = eid_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s page\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            bool protect = false;

            op->protect_not_sure = false;
            if (op->std_inq_a_valid)
                 protect = !! (0x1 & op->std_inq_a[5]);
            else if (ptvp && (! op->do_force)) {
                struct sg_simple_inquiry_resp sir;

                res = sg_simple_inquiry(get_pt_file_handle(ptvp), &sir,
                                        false, vb);
                if (res) {
                    if (op->verbose)
                        pr2serr("%s: sg_simple_inquiry() failed, res=%d\n",
                                __func__, res);
                    op->protect_not_sure = true;
                } else
                    protect = !!(sir.byte_5 & 0x1); /* SPC-3 and later */
            } else
                op->protect_not_sure = true;
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            decode_x_inq_vpd(rp, len, protect, op, jo2p);
        }
        break;
    case VPD_MODE_PG_POLICY:            /*  0x87  ["mpp"] */
        np = mpp_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "mode_page_policy_descriptor_list");
            }
            decode_mode_policy_vpd(rp, len, op, jap);
        }
        break;
    case VPD_SCSI_PORTS:        /* 0x88  ["sp"] */
        np = sp_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "scsi_ports_descriptor_list");
            }
            decode_scsi_ports_vpd_4inq(rp, len, op, jap);
        }
        break;
    case VPD_ATA_INFO:          /* 0x89  ["ai"] */
        np = ai_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        /* format output for 'hdparm --Istdin' with '-rr' or '-HHH' */
        if ((2 == op->do_raw) || (3 == dhex)) {
            if (len < 572)
                pr2serr("%s is too short (%d < 572)\n", np, len);
            else
                dWordHex((const unsigned short *)(rp + 60), 256, -2,
                         sg_is_big_endian());
        } else if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            else
                op->do_long = true;
            decode_ata_info_vpd(rp, len, op, jo2p);
        }
        break;
    case VPD_POWER_CONDITION:   /* 0x8a   ["pc"] */
        np = pc_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            decode_power_condition(rp, len, op, jo2p);
        }
        break;
    case VPD_DEVICE_CONSTITUENTS:       /* 0x8b  ["dc"] */
        np = dc_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "constituent_descriptor_list");
            }
            decode_dev_constit_vpd(rp, len, op, jap, recurse_vpd_decode);
        }
        break;
    case VPD_CFA_PROFILE_INFO:    /* 0x8c ["cfa"] */
        np = cpi_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "%s:\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            if (op->do_raw)
                dStrRaw((const char *)rp, len);
            else {
                if (as_json) {
                    jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                    jap = sgj_named_subarray_r(jsp, jo2p,
                                      "cfa_profile_descriptor_list");
                }
                decode_cga_profile_vpd(rp, len, op, jap);
            }
        }
        break;
    case VPD_POWER_CONSUMPTION:   /* 0x8d   ["psm"] */
        np = psm_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "power_consumption_descriptor_list");
            }
            decode_power_consumption(rp, len, op, jap);
        }
        break;
    case VPD_3PARTY_COPY:       /* 0x8f  ["tpc"] */
        np = tpc_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "third_party_copy_descriptor_list");
            }
            decode_3party_copy_vpd(rp, len, op, jap);
        }
        break;
    case VPD_PROTO_LU:          /* 0x90  ["pslu"] */
        np = pslu_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "logical_unit_information_descriptor_list");
            }
            decode_proto_lu_vpd(rp, len, op, jap);
        }
        break;
    case VPD_PROTO_PORT:        /* 0x91  ["pspo"] */
        np = pspo_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "port_information_descriptor_list");
            }
            decode_proto_port_vpd(rp, len, op, jap);
        }
        break;
    case VPD_SCSI_FEATURE_SETS:         /* 0x92  ["sfs"] */
        np = sfs_vpdp;
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s\n", np);
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                                  "feature_set_code_list");
            }
            decode_feature_sets_vpd(rp, len, op, jap);
        }
        break;
    case 0xb0:  /* VPD pages in B0h to BFh range depend on pdt */
        np = NULL;
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            bool bl = false;
            bool sad = false;
            bool oi = false;

            ep = "";
            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                break;
            }
            pdt = rp[0] & PDT_MASK;
            switch (pdt) {
            case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
                np = bl_vpdp;
                ep = "(SBC)";
                bl = true;
                break;
            case PDT_TAPE: case PDT_MCHANGER:
                np = sad_vpdp;
                ep = "(SSC)";
                sad = true;
                break;
            case PDT_OSD:
                np = "OSD information VPD page";
                ep = "(OSD)";
                oi = true;
                break;
            default:
                np = NULL;
                break;
            }
            if (dhex < 3) {
                if (NULL == np)
                    sgj_pr_hr(jsp, "%s=0x%x, pdt=0x%x:\n", vpd_pg_s, pn, pdt);
                else
                    sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            }
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            if (bl)
                decode_block_limits_vpd(rp, len, op, jo2p);
            else if (sad || oi)
                decode_b0_vpd(rp, len, op, jop);
        } else if (! op->do_raw)
            pr2serr("VPD INQUIRY: page=0xb0\n");
        break;
    case 0xb1:  /* VPD pages in B0h to BFh range depend on pdt */
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            bool bdc = false;

            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                break;
            }
            pdt = rp[0] & PDT_MASK;
            switch (pdt) {
            case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
                np = bdc_vpdp;
                ep = "(SBC)";
                bdc = true;
                break;
            case PDT_TAPE: case PDT_MCHANGER:
                np = masn_vpdp;
                ep = "(SSC)";
                break;
            case PDT_OSD:
                np = st_vpdp;
                ep = "(OSD)";
                break;
            case PDT_ADC:
                np = masn_vpdp;
                ep = "(ADC)";
                break;
            default:
                np = NULL;
                printf("VPD INQUIRY: page=0x%x, pdt=0x%x\n", 0xb1, pdt);
                break;
            }
            if (dhex < 3) {
                if (NULL == np)
                    sgj_pr_hr(jsp, "%s=0x%x, pdt=0x%x:\n", vpd_pg_s, pn, pdt);
                else
                    sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            }
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            if (bdc)
                decode_block_dev_ch_vpd(rp, len, op, jo2p);
            else
                decode_b1_vpd(rp, len, op, jo2p);
        } else if (! op->do_raw)
            pr2serr("VPD INQUIRY: page=0xb1\n");
        break;
    case 0xb2:  /* VPD pages in B0h to BFh range depend on pdt */
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            bool lbpv = false;
            bool tas = false;

            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                break;
            }
            pdt = rp[0] & PDT_MASK;
            switch (pdt) {
            case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
                np = lbpv_vpdp;
                ep = "(SBC)";
                lbpv = true;
                break;
            case PDT_TAPE: case PDT_MCHANGER:
                np = tas_vpdp;
                ep = "(SSC)";
                tas = true;
                break;
            default:
                np = NULL;
                break;
            }
            if (dhex < 3) {
                if (NULL == np)
                    sgj_pr_hr(jsp, "%s=0x%x, pdt=0x%x:\n", vpd_pg_s, pn, pdt);
                else
                    sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            }
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            if (lbpv)
                return decode_block_lb_prov_vpd(rp, len, op, jo2p);
            else if (tas)
                decode_tapealert_supported_vpd(rp, len, op, jo2p);
            else
                return vpd_mainly_hex(ptvp, op, jo2p, off);
        } else if (! op->do_raw)
            pr2serr("VPD INQUIRY: page=0xb2\n");
        break;
    case 0xb3:
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            bool ref = false;

            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                break;
            }
            pdt = rp[0] & PDT_MASK;
            switch (pdt) {
            case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
                np = ref_vpdp;
                ep = "(SBC)";
                ref = true;
                break;
            case PDT_TAPE: case PDT_MCHANGER:
                np = adsn_vpdp;
                ep = "(SSC)";
                break;
            default:
                np = NULL;
                break;
            }
            if (dhex < 3) {
                if (NULL == np)
                    sgj_pr_hr(jsp, "%s=0x%x, pdt=0x%x:\n", vpd_pg_s, pn, pdt);
                else
                    sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            }
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            if (ref)
                decode_referrals_vpd(rp, len, op, jo2p);
            else
                decode_b3_vpd(rp, len, op, jo2p);
            return 0;
        } else if (! op->do_raw)
            pr2serr("VPD INQUIRY: page=0xb3\n");
        break;
    case 0xb4:
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            bool sbl = false;
            bool dtde = false;

            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                break;
            }
            pdt = rp[0] & PDT_MASK;
            switch (pdt) {
            case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
                np = sbl_vpdp;
                ep = "(SBC)";
                sbl = true;
                break;
            case PDT_TAPE: case PDT_MCHANGER:
                np = "Device transfer data element VPD page";
                ep = "(SSC)";
                dtde = true;
                break;
            default:
                np = NULL;
                break;
            }
            if (dhex < 3) {
                if (NULL == np)
                    sgj_pr_hr(jsp, "%s=0x%x, pdt=0x%x:\n", vpd_pg_s, pn, pdt);
                else
                    sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            }
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            if (sbl) {
                if (as_json)
                    jap = sgj_named_subarray_r(jsp, jo2p, "logical_block_"
                            "length_and_protection_types_descriptor_list");
                decode_sup_block_lens_vpd(rp, len, op, jap);
            } else if (dtde) {
                if (! jsp->pr_as_json)
                    hex2stdout(rp + 4, len - 4, 1);
                sgj_js_nv_hex_bytes(jsp, jo2p, "device_transfer_data_element",
                                    rp + 4, len - 4);
            } else
                return vpd_mainly_hex(ptvp, op, jo2p, off);
            return 0;
        } else if (! op->do_raw)
            pr2serr("VPD INQUIRY: page=0xb4\n");
        break;
    case 0xb5:
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            bool bdce = false;
            bool lbp = false;

            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                break;
            }
            pdt = rp[0] & PDT_MASK;
            switch (pdt) {
            case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
                np = bdce_vpdp;
                ep = "(SBC)";
                bdce = true;
                break;
            case PDT_TAPE: case PDT_MCHANGER:
                np = lbpro_vpdp;
                ep = "(SSC)";
                lbp = true;
                break;
            default:
                np = NULL;
                break;
            }
            if (dhex < 3) {
                if (NULL == np)
                    sgj_pr_hr(jsp, "%s=0x%x, pdt=0x%x:\n", vpd_pg_s, pn, pdt);
                else
                    sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            }
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            if (bdce)
                decode_block_dev_char_ext_vpd(rp, len, op, jo2p);
            else if (lbp) {     /* VPD_LB_PROTECTION  0xb5 ["lbpro"] (SSC) */
                if (as_json)
                    jap = sgj_named_subarray_r(jsp, jo2p,
                     "logical_block_protection_method_descriptor_list");
                decode_lb_protection_vpd(rp, len, op, jap);
            } else
                return vpd_mainly_hex(ptvp, op, jo2p, off);
            return 0;
        } else if (! op->do_raw)
            pr2serr("VPD INQUIRY: page=0xb5\n");
        break;
    case 0xb6:
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            bool zbdch = false;

            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                break;
            }
            pdt = rp[0] & PDT_MASK;
            switch (pdt) {
            case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
                np = zbdc_vpdp;
                ep = "(SBC, ZBC)";
                zbdch = true;
                break;
            default:
                np = NULL;
                break;
            }
            if (dhex < 3) {
                if (NULL == np)
                    sgj_pr_hr(jsp, "%s=0x%x, pdt=0x%x:\n", vpd_pg_s, pn, pdt);
                else
                    sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            }
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            if (zbdch)
                decode_zbdch_vpd(rp, len, op, jo2p);
            else
                return vpd_mainly_hex(ptvp, op, jo2p, off);
            return 0;
        } else if (! op->do_raw)
            pr2serr("VPD INQUIRY: page=0xb6\n");
        break;
    case 0xb7:
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            bool ble = false;

            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                break;
            }
            pdt = rp[0] & PDT_MASK;
            switch (pdt) {
            case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
                np = ble_vpdp;
                ep = "(SBC)";
                ble = true;
                break;
            default:
                np = NULL;
                break;
            }
            if (dhex < 3) {
                if (NULL == np)
                    sgj_pr_hr(jsp, "%s=0x%x, pdt=0x%x:\n", vpd_pg_s, pn, pdt);
                else
                    sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            }
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            if (ble)
                decode_block_limits_ext_vpd(rp, len, op, jo2p);
            else
                return vpd_mainly_hex(ptvp, op, jo2p, off);
            return 0;
        } else if (! op->do_raw)
            pr2serr("VPD INQUIRY: page=0xb7\n");
        break;
    case 0xb8:
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            bool fp = false;

            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                break;
            }
            pdt = rp[0] & PDT_MASK;
            switch (pdt) {
            case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
                np = fp_vpdp;
                ep = "(SBC)";
                fp = true;
                break;
            default:
                np = NULL;
                break;
            }
            if (dhex < 3) {
                if (NULL == np)
                    sgj_pr_hr(jsp, "%s=0x%x, pdt=0x%x:\n", vpd_pg_s, pn, pdt);
                else
                    sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            }
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p, "format_preset_"
                            "descriptor_list");
            }
            if (fp)
                decode_format_presets_vpd(rp, len, op, jap);
            else
                return vpd_mainly_hex(ptvp, op, jo2p, off);
            return 0;
        } else if (! op->do_raw)
            pr2serr("VPD INQUIRY: page=0xb8\n");
        break;
    case 0xb9:
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            bool cpr = false;

            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                break;
            }
            pdt = rp[0] & PDT_MASK;
            switch (pdt) {
            case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
                np = cpr_vpdp;
                ep = "(SBC)";
                cpr = true;
                break;
            default:
                np = NULL;
                break;
            }
            if (dhex < 3) {
                if (NULL == np)
                    sgj_pr_hr(jsp, "%s=0x%x, pdt=0x%x:\n", vpd_pg_s, pn, pdt);
                else
                    sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            }
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p, "lba_range_"
                                           "descriptor_list");
            }
            if (cpr)
                decode_con_pos_range_vpd(rp, len, op, jap);
            else
                return vpd_mainly_hex(ptvp, op, jo2p, off);
            return 0;
        } else if (! op->do_raw)
            pr2serr("VPD INQUIRY: page=0xb9\n");
        break;
    case 0xba:
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (0 == res) {
            bool cap = false;

            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                break;
            }
            pdt = rp[0] & PDT_MASK;
            switch (pdt) {
            case PDT_DISK: case PDT_WO: case PDT_OPTICAL: case PDT_ZBC:
                np = cap_vpdp;          /* VPD_CAP_PROD_ID */
                ep = "(SBC)";
                cap = true;
                break;
            default:
                np = NULL;
                break;
            }
            if (dhex < 3) {
                if (NULL == np)
                    sgj_pr_hr(jsp, "%s=0x%x, pdt=0x%x:\n", vpd_pg_s, pn, pdt);
                else
                    sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            }
            if (as_json) {
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
                jap = sgj_named_subarray_r(jsp, jo2p,
                         "capacity_product_identification_descriptors_list");
            }
            if (cap)
                decode_cap_prod_id_vpd(rp, len, op, jap);
            else
                return vpd_mainly_hex(ptvp, op, jo2p, off);
            return 0;
        } else if (! op->do_raw)
            pr2serr("VPD INQUIRY: page=0xba\n");
        break;
    /* Vendor specific VPD pages (>= 0xc0) */
    case VPD_UPR_EMC:   /* 0xc0 */
        np = "Unit path report VPD page";
        ep = "(EMC)";
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
        res = vpd_fetch_page(ptvp, rp, pn, -1, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            decode_upr_vpd_c0_emc(rp, len, op, jo2p);
        }
        break;
    case VPD_RDAC_VERS:         /* 0xc2 */
        np = "Software Version VPD page";
        ep = "(RDAC)";
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
        res = vpd_fetch_page(ptvp, rp, pn, -1, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            decode_rdac_vpd_c2(rp, len, op, jo2p);
        }
        break;
    case VPD_RDAC_VAC:          /* 0xc9 */
        np = "Volume access control VPD page";
        ep = "(RDAC)";
        if (!op->do_raw && (dhex < 3))
            sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
        res = vpd_fetch_page(ptvp, rp, pn, -1, qt, vb, &len);
        if (res)
            break;
        if (op->do_raw)
            dStrRaw((const char *)rp, len);
        else {
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            decode_rdac_vpd_c9(rp, len, op, jo2p);
        }
        break;
    case SG_NVME_VPD_NICR:    /* 0xde, model on ATA Info VPD page [0x89] */
        np = "NVMe Identify Controller Response VPD page";
        /* NVMe: Identify Controller data structure (CNS 01h) */
        ep = "(sg3_utils)";
        res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
        if (res) {
            sgj_pr_hr(jsp, "VPD INQUIRY: %s %s\n", np, ep);
            break;
        }
        if (op->do_raw) {
            dStrRaw((const char *)rp, len);
            break;
        } else {
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            decode_snt_nvme_info_vpd(rp, len, op, jo2p);
        }
        break;
    default:
        bad = true;
        break;
    }
    if (bad) {
        if ((pn > 0) && (pn < 0x80)) {
            np = "ASCII information VPD page";
            if (!op->do_raw && (dhex < 3))
                sgj_pr_hr(jsp, "VPD INQUIRY: %s, FRU code=0x%x\n", np, pn);
            res = vpd_fetch_page(ptvp, rp, pn, op->maxlen, qt, vb, &len);
            if (res)
                goto out;
            if (op->do_raw) {
                dStrRaw((const char *)rp, len);
                return 0;
            } else if (dhex > 0) {
                if (dhex > 2) {
                    snprintf(b, blen, "%s 0x%x", np, pn);
                    named_hhh_output(b, rp, len, op);
                } else
                    hex2stdout(rp, len, no_ascii_4hex(op));
                return 0;
            }
            if (as_json)
                jo2p = sg_vpd_js_hdr(jsp, jop, np, rp);
            decode_ascii_inf(rp, len, op, jo2p);
        } else {
            if (as_json) {
                int rlen = sg_get_unaligned_be16(rp + 2) + 4;

                snprintf(b, blen, "vpd_page_%02x", pn);
                jo2p = sg_vpd_js_hdr(jsp, jop, b, rp);
                snprintf(b, blen, "%d bytes long when 4 byte header included",
                         rlen);
                sgj_js_nv_ihexstr(jsp, jo2p, "page_length", rlen - 4, NULL,
                                  b);
            } else if (dhex < 3)
                pr2serr(" Only hex output supported.\n");
            return vpd_mainly_hex(ptvp, op, jo2p, off);
        }
    }
out:
    if (res) {

        if (SG_LIB_CAT_ILLEGAL_REQ == res)
            pr2serr("    inquiry: field in cdb illegal (page not "
                    "supported)\n");
        else {
            sg_get_category_sense_str(res, blen, b, vb);
            pr2serr("    inquiry: %s\n", b);
        }
    }
    return res;
}

#if (HAVE_NVME && (! IGNORE_NVME))

static void
nvme_hex_raw(const uint8_t * b, int b_len, const struct opts_t * op)
{
    if (op->do_raw)
        dStrRaw((const char *)b, b_len);
    else if (op->do_hex) {
        if (op->do_hex < 3) {
            printf("data_in buffer:\n");
            hex2stdout(b, b_len, (2 == op->do_hex));
        } else
            hex2stdout(b, b_len, -1);
    }
}

static const char * rperf[] = {"Best", "Better", "Good", "Degraded"};

static void
show_nvme_id_ns(const uint8_t * dinp, uint32_t nsid, struct opts_t * op,
                sgj_opaque_p jop)
{
    bool as_json, active;
    bool got_eui_128 = false;
    uint32_t u, k, n, off, num_lbaf, flbas, flba_info, md_size, lb_size;
    uint32_t format_ind, lb_sz_exp;
    uint64_t ns_sz, ns_cap, ns_util, eui_64;
    sgj_state * jsp = &op->json_st;
    sgj_opaque_p jo2p = NULL;
    sgj_opaque_p jo3p = NULL;
    sgj_opaque_p jap = NULL;
    char b[144];
    static const int blen = sizeof(b);
    static const char * u_lb_s = "[unit: logical block]";

    as_json = jsp->pr_as_json;
    if (as_json) {
        sg_scnpr(b, blen, "identify_namespace_%u_data_structure", nsid);
        jo2p = sgj_named_subobject_r(jsp, jop, b);
    }
    num_lbaf = dinp[25] + 1;  /* spec says this is "0's based value" */
    ns_sz = sg_get_unaligned_le64(dinp + 0);
    ns_cap = sg_get_unaligned_le64(dinp + 8);
    ns_util = sg_get_unaligned_le64(dinp + 16);
    eui_64 = sg_get_unaligned_be64(dinp + 120);  /* N.B. big endian */
    if (! sg_all_zeros(dinp + 104, 16))
        got_eui_128 = true;
    sgj_pr_hr(jsp, "    Namespace size/capacity: %" PRIu64 "/%" PRIu64
              " blocks\n", ns_sz, sg_get_unaligned_le64(dinp + 8));
    sgj_pr_hr(jsp, "    Namespace utilization: %" PRIu64 " blocks\n",
              ns_util);
    if (as_json) {
        sgj_js_nv_ihex_nex(jsp, jo2p, "namespace_size", ns_sz, true, u_lb_s);
        sgj_js_nv_ihex_nex(jsp, jo2p, "namespace_capacity", ns_cap, true,
                           u_lb_s);
        sgj_js_nv_ihex_nex(jsp, jo2p, "namespace_utilization", ns_util, true,
                           u_lb_s);
    }
    if (got_eui_128) {          /* N.B. big endian */
        n = sg_scnpr(b, blen, "0x%02x", dinp[104]);
        for (k = 1; k < 16; ++k)
            n += sg_scn3pr(b, blen, n, "%02x", dinp[104 + k]);
        sgj_haj_vs(jsp, jo2p, 4, "NGUID", SGJ_SEP_COLON_1_SPACE, b);
    } else if (op->do_long)
        sgj_pr_hr(jsp, "    NGUID: 0x0\n");
    if (eui_64) { /* EUI_64 if given is BIG endian */
        sgj_pr_hr(jsp, "    EUI-64: 0x%" PRIx64 "\n", eui_64);
        sgj_js_nv_ihex(jsp, jo2p, "eui_64", eui_64);
    }
    sgj_haj_vi(jsp, jo2p, 4, "Number of LBA formats", SGJ_SEP_COLON_1_SPACE,
               num_lbaf, false);
    flbas = dinp[26];
    format_ind = flbas & 0xf;
    if (num_lbaf > 16)          /* hack upon hack */
        format_ind = ((flbas & 0x60) >> 1) | format_ind;
    sgj_haj_vi(jsp, jo2p, 4, "Format index", SGJ_SEP_COLON_1_SPACE,
               format_ind, false);
    if (as_json)
        jap = sgj_named_subarray_r(jsp, jo2p, "lba_format_list");

    for (k = 0, off = 128; k < num_lbaf; ++k, off += 4) {
        sg_scnpr(b, blen, "    LBA format %u support:", k);
        active = (k == format_ind);
        if (active)
            sgj_pr_hr(jsp, "%s <-- active\n", b);
        else
            sgj_pr_hr(jsp, "%s\n", b);
        flba_info = sg_get_unaligned_le32(dinp + off);
        md_size = flba_info & 0xffff;
        lb_sz_exp = (flba_info >> 16) & 0xff;
        if (lb_sz_exp > 31) {
            pr2serr("%s: logical block size exponent of %u implies a LB "
                    "size larger than 4 billion bytes, ignore\n", __func__,
                    lb_sz_exp);
            continue;
        }
        lb_size = 1U << lb_sz_exp;
        ns_sz *= lb_size;
        ns_sz /= 500*1000*1000;
        if (ns_sz & 0x1)
            ns_sz = (ns_sz / 2) + 1;
        else
            ns_sz = ns_sz / 2;
        u = (flba_info >> 24) & 0x3;
        sgj_pr_hr(jsp, "      Logical block size: %u bytes\n", lb_size);
        sgj_pr_hr(jsp, "      Approximate namespace size: %" PRIu64 " GB\n",
                  ns_sz);
        sgj_pr_hr(jsp, "      Metadata size: %u bytes\n", md_size);
        sgj_pr_hr(jsp, "      Relative performance: %s [0x%x]\n", rperf[u],
                  u);
        if (as_json) {
            jo3p = sgj_new_unattached_object_r(jsp);
            sgj_js_nv_ihexstr(jsp, jo3p, "relative_performance", u, NULL,
                              rperf[u]);
            sgj_js_nv_ihex_nex(jsp, jo3p, "lba_data_size", lb_sz_exp, false,
                               "power of 2");
            sgj_js_nv_ihex_nex(jsp, jo3p, "logical_block_size", lb_size, true,
                               "[unit: byte]");
            sgj_js_nv_ihex_nex(jsp, jo3p, "metadata_size", md_size, true,
                               "[unit: byte]");
            sgj_js_nv_ihex_nex(jsp, jo3p, "active", (int)active, false,
                               "most recent format used this");
            sgj_js_nv_ihex_nex(jsp, jo3p, "approximate_namespace_size", ns_sz,
                               true, "[unit: GigaByte]");
            sgj_js_nv_o(jsp, jap, NULL /* name */, jo3p);
        }
    }
}

/* Send Identify(CNS=0, nsid) and decode the Identify namespace response */
static int
nvme_id_namespace(struct sg_pt_base * ptvp, uint32_t nsid,
                  struct sg_nvme_passthru_cmd * id_cmdp, uint8_t * id_dinp,
                  int id_din_len, struct opts_t * op, sgj_opaque_p jop)
{
    int ret = 0;
    int vb = op->verbose;
    uint8_t resp[16];

    clear_scsi_pt_obj(ptvp);
    id_cmdp->nsid = nsid;
    id_cmdp->cdw10 = 0x0;       /* CNS=0x0 Identify NS (CNTID=0) */
    id_cmdp->cdw11 = 0x0;       /* NVMSETID=0 (only valid when CNS=0x4) */
    id_cmdp->cdw14 = 0x0;       /* UUID index (assume not supported) */
    set_scsi_pt_data_in(ptvp, id_dinp, id_din_len);
    set_scsi_pt_sense(ptvp, resp, sizeof(resp));
    set_scsi_pt_cdb(ptvp, (const uint8_t *)id_cmdp, sizeof(*id_cmdp));
    ret = do_scsi_pt(ptvp, -1, 0 /* timeout (def: 1 min) */, vb);
    if (vb > 2)
        pr2serr("%s: do_scsi_pt() result is %d\n", __func__, ret);
    if (ret) {
        if (SCSI_PT_DO_BAD_PARAMS == ret)
            ret = SG_LIB_SYNTAX_ERROR;
        else if (SCSI_PT_DO_TIMEOUT == ret)
            ret = SG_LIB_CAT_TIMEOUT;
        else if (ret < 0)
            ret = sg_convert_errno(-ret);
        return ret;
    }
    if (op->do_hex || op->do_raw) {
        nvme_hex_raw(id_dinp, id_din_len, op);
        return 0;
    }
    show_nvme_id_ns(id_dinp, nsid, op, jop);
    return 0;
}

static const char * const oacs_a[11] = {
    "Security send and receive",
    "Format NVM",
    "Firmware download and commit",
    "Namespace management and attachment",
    "Device self-test",
    "Directive send and directive receive",
    "NVMe-MI send and NVMe-MI receive",
    "Virtualization management",
    "Doorbell buffer config",
    "Get LBA status",                   /* NVMe 1.4 */
    "Command and feature lockdown",     /* NVMe 2.0 */
};

static const char * const oncs_a[9] = {
    "Compare",
    "Write uncorrectable",
    "Dataset management",
    "Write zeroes",
    "Save and Select fields non-zero",
    "Reservations",
    "Timestamp feature",
    "Verify and Verify size limit",     /* NVMe 1.4 */
    "Copy",                             /* NVMe 2.0 */
};

static void
show_nvme_id_ctrl(const uint8_t *dinp, struct opts_t * op, sgj_opaque_p jop)
{
    bool got_fguid, as_json;
    uint8_t ver_min, ver_ter, mtds, cmic;
    uint16_t ver_maj, oacs, oncs;
    uint32_t k, ver, max_nsid, npss, j, n, m;
    int h;
    uint64_t sz1, sz2;
    const uint8_t * up;
    const char * ccp;
    sgj_opaque_p jap = NULL;
    sgj_opaque_p jo2p = NULL;
    sgj_opaque_p jo3p = NULL;
    sgj_state * jsp = &op->json_st;
    char b[144];
    char bb[32];
    static const int blen = sizeof(b);
    static const int bblen = sizeof(bb);
    static const char * cmic_s = "Controller Multi-Path I/O and Namespace "
                                 "Sharing Capabilities";

    as_json = jsp->pr_as_json;
    if (as_json)
        jo2p = sgj_named_subobject_r(jsp, jop,
                                     "identify_controller_data_structure");
    max_nsid = sg_get_unaligned_le32(dinp + 516); /* NN */
    sgj_pr_hr(jsp, "Identify controller for %s:\n", op->device_name);
    snprintf(b, blen, "%.40s", (const char *)(dinp + 24));
    sgj_haj_vs(jsp, jo2p, 2, "Model number", SGJ_SEP_COLON_1_SPACE, b);
    snprintf(b, blen, "%.20s", (const char *)(dinp + 4));
    sgj_haj_vs(jsp, jo2p, 2, "Serial number", SGJ_SEP_COLON_1_SPACE, b);
    snprintf(bb, bblen, "%.8s", (const char *)(dinp + 64));
    sgj_haj_vs(jsp, jo2p, 2, "Firmware revision", SGJ_SEP_COLON_1_SPACE, bb);
    ver = sg_get_unaligned_le32(dinp + 80);
    ver_maj = (ver >> 16);
    ver_min = (ver >> 8) & 0xff;
    ver_ter = (ver & 0xff);
    h = sg_scnpr(b, blen, "%u.%u", ver_maj, ver_min);
    if ((ver_maj > 1) || ((1 == ver_maj) && (ver_min > 2)) ||
        ((1 == ver_maj) && (2 == ver_min) && (ver_ter > 0)))
        sg_scnpr(b + h, blen - h, ".%u", ver_ter);
    sgj_haj_vs(jsp, jo2p, 2, "Version", SGJ_SEP_COLON_1_SPACE, b);
    oacs = sg_get_unaligned_le16(dinp + 256);
    if (0x7ff & oacs) {
        sgj_pr_hr(jsp, "  Optional admin command support:\n");
        if (as_json)
            jap = sgj_named_subarray_r(jsp, jo2p,
                                       "optional_admin_command_support_list");
        for (k = 0x1, h = 0; h < 11; k <<= 1, ++h) {
            if (k & oacs) {
                ccp = oacs_a[h];
                sgj_pr_hr(jsp, "    %s\n", ccp);
                if (as_json) {
                    jo3p = sgj_new_unattached_string_r(jsp, ccp);
                    sgj_js_nv_o(jsp, jap, NULL /* name */, jo3p);
                }
            }
        }
    } else
        sgj_pr_hr(jsp, "  No optional admin command support\n");
    oncs = sg_get_unaligned_le16(dinp + 256);
    if (0x1ff & oncs) {
        sgj_pr_hr(jsp, "  Optional NVM command support:\n");
        if (as_json)
            jap = sgj_named_subarray_r(jsp, jo2p,
                                       "optional_nvm_command_support_list");
        for (k = 0x1, h = 0; h < 9; k <<= 1, ++h) {
            if (k & oncs) {
                ccp = oncs_a[h];
                sgj_pr_hr(jsp, "    %s\n", ccp);
                if (as_json) {
                    jo3p = sgj_new_unattached_string_r(jsp, ccp);
                    sgj_js_nv_o(jsp, jap, NULL /* name */, jo3p);
                }
            }
        }
    } else
        sgj_pr_hr(jsp, "  No optional NVM command support\n");
    sgj_pr_hr(jsp, "  PCI vendor ID VID/SSVID: 0x%x/0x%x\n",
              sg_get_unaligned_le16(dinp + 0),
              sg_get_unaligned_le16(dinp + 2));
    sgj_pr_hr(jsp, "  IEEE OUI Identifier: 0x%x\n", /* has been renamed AOI */
              sg_get_unaligned_le24(dinp + 73));
    cmic = dinp[76];
    if (as_json)
        sgj_js_nv_ihex_nex(jsp, jo2p, "cmic", cmic, true, cmic_s);
    else {
        if (cmic)
            sgj_pr_hr(jsp, "  %s: 0x%x\n", cmic_s, cmic);
        else
            sgj_pr_hr(jsp, "  No %s\n", cmic_s);
    }
    got_fguid = ! sg_all_zeros(dinp + 112, 16);
    if (got_fguid) {
        n = sg_scnpr(b, blen, "  FGUID: 0x%02x", dinp[112]);
        for (k = 1; k < 16; ++k)
            n += sg_scn3pr(b, blen, n, "%02x", dinp[112 + k]);
        sgj_pr_hr(jsp, "%s\n", b);
    } else if (op->do_long)
        sgj_pr_hr(jsp, "  FGUID: 0x0\n");
    sgj_pr_hr(jsp, "  Controller ID: 0x%x\n",
              sg_get_unaligned_le16(dinp + 78));
    if (op->do_long) {      /* Bytes 240 to 255 are reserved for NVME-MI */
        sgj_pr_hr(jsp, "  NVMe Management Interface [MI] settings:\n");
        sgj_pr_hr(jsp, "    Enclosure: %d [NVMEE]\n", !! (0x2 & dinp[253]));
        sgj_pr_hr(jsp, "    NVMe Storage device: %d [NVMESD]\n",
                  !! (0x1 & dinp[253]));
        sgj_pr_hr(jsp, "    Management endpoint capabilities, over a PCIe "
                  "port: %d [PCIEME]\n", !! (0x2 & dinp[255]));
        sgj_pr_hr(jsp, "    Management endpoint capabilities, over a "
                  "SMBus/I2C port: %d [SMBUSME]\n", !! (0x1 & dinp[255]));
    }
    sgj_pr_hr(jsp, "  Number of namespaces: %u\n", max_nsid);
    sz1 = sg_get_unaligned_le64(dinp + 280);  /* lower 64 bits */
    sz2 = sg_get_unaligned_le64(dinp + 288);  /* upper 64 bits */
    if (sz2)
        sgj_pr_hr(jsp, "  Total NVM capacity: huge ...\n");
    else if (sz1)
        sgj_pr_hr(jsp, "  Total NVM capacity: %" PRIu64 " bytes\n", sz1);
    mtds = dinp[77];
    sg_scnpr(b, blen, "  Maximum data transfer size: ");
    if (mtds)
        sgj_pr_hr(jsp, "%s%u pages\n", b, 1U << mtds);
    else
        sgj_pr_hr(jsp, "%s<unlimited>\n", b);

    if (op->do_long) {
        int q;
        const char * const non_op = "does not process I/O";
        const char * const operat = "processes I/O";
        const char * cp;

        sgj_pr_hr(jsp, "  Total NVM capacity: 0 bytes\n");
        npss = dinp[263] + 1;
        up = dinp + 2048;
        for (k = 0; k < npss; ++k, up += 32) {
            n = sg_get_unaligned_le16(up + 0);
            n *= (0x1 & up[3]) ? 1 : 100;    /* unit: 100 microWatts */
            j = n / 10;                      /* unit: 1 milliWatts */
            m = j % 1000;
            j /= 1000;
            cp = (0x2 & up[3]) ? non_op : operat;
            sg_scnpr(b, blen, "  Power state %u: Max power: ", k);
            if (0 == j) {
                m = n % 10;
                n /= 10;
                sgj_pr_hr(jsp, "%s%u.%u milliWatts, %s\n", b, n, m, cp);
            } else
                sgj_pr_hr(jsp, "%s%u.%03u Watts, %s\n", b, j, m, cp);
            n = sg_get_unaligned_le32(up + 4);
            if (0 == n)
                q = sg_scnpr(b, blen, "    [ENLAT], ");
            else
                q = sg_scnpr(b, blen, "    ENLAT=%u, ", n);
            n = sg_get_unaligned_le32(up + 8);
            if (0 == n)
                q += sg_scn3pr(b, blen, q, "[EXLAT], ");
            else
                q += sg_scn3pr(b, blen, q, "EXLAT=%u, ", n);
            n = 0x1f & up[12];
            q += sg_scn3pr(b, blen, q, "RRT=%u, ", n);
            n = 0x1f & up[13];
            q += sg_scn3pr(b, blen, q, "RRL=%u, ", n);
            n = 0x1f & up[14];
            sg_scn3pr(b, blen, q, "RWT=%u, ", n);
            n = 0x1f & up[15];
            sgj_pr_hr(jsp, "%sRWL=%u\n", b, n);
        }
    }
}

/* Send a NVMe Identify(CNS=1) and decode Controller info. If the
 * device name includes a namespace indication (e.g. /dev/nvme0ns1) then
 * an Identify namespace command is sent to that namespace (e.g. 1). If the
 * device name does not contain a namespace indication (e.g. /dev/nvme0)
 * and --only is not given then nvme_id_namespace() is sent for each
 * namespace in the controller. Namespaces number sequentially starting at
 * 1 . The CNS (Controller or Namespace Structure) field is CDW10 7:0, was
 * only bit 0 in NVMe 1.0 and bits 1:0 in NVMe 1.1, thereafter 8 bits. */
static int
do_nvme_identify_ctrl(struct sg_pt_base * ptvp, struct opts_t * op,
                      sgj_opaque_p jop)
{
    int ret = 0;
    int vb = op->verbose;
    uint32_t k, nsid, max_nsid;
    struct sg_nvme_passthru_cmd identify_cmd;
    struct sg_nvme_passthru_cmd * id_cmdp = &identify_cmd;
    uint8_t * id_dinp = NULL;
    uint8_t * free_id_dinp = NULL;
    sgj_state * jsp = &op->json_st;
    const uint32_t pg_sz = sg_get_page_size();
    uint8_t resp[16];

    if (op->do_raw) {
        if (sg_set_binary_mode(STDOUT_FILENO) < 0) {
            perror("sg_set_binary_mode");
            return SG_LIB_FILE_ERROR;
        }
    }
    if (NULL == ptvp) {
        pr2serr("%s: memory problem\n", __func__);
        return sg_convert_errno(ENOMEM);
    }
    memset(id_cmdp, 0, sizeof(*id_cmdp));
    id_cmdp->opcode = 0x6;
    nsid = get_pt_nvme_nsid(ptvp);
    id_cmdp->cdw10 = 0x1;       /* CNS=0x1 --> Identify controller */
    /* id_cmdp->nsid is a "don't care" when CNS=1, so leave as 0 */
    id_dinp = sg_memalign(pg_sz, pg_sz, &free_id_dinp, false);
    if (NULL == id_dinp) {
        pr2serr("%s: sg_memalign problem\n", __func__);
        return sg_convert_errno(ENOMEM);
    }
    set_scsi_pt_data_in(ptvp, id_dinp, pg_sz);
    set_scsi_pt_cdb(ptvp, (const uint8_t *)id_cmdp, sizeof(*id_cmdp));
    set_scsi_pt_sense(ptvp, resp, sizeof(resp));
    ret = do_scsi_pt(ptvp, -1, 0 /* timeout (def: 1 min) */, vb);
    if (vb > 2)
        pr2serr("%s: do_scsi_pt result is %d\n", __func__, ret);
    if (ret) {
        if (SCSI_PT_DO_BAD_PARAMS == ret)
            ret = SG_LIB_SYNTAX_ERROR;
        else if (SCSI_PT_DO_TIMEOUT == ret)
            ret = SG_LIB_CAT_TIMEOUT;
        else if (ret < 0)
            ret = sg_convert_errno(-ret);
        goto err_out;
    }
    max_nsid = sg_get_unaligned_le32(id_dinp + 516); /* NN */
    if (op->do_raw || op->do_hex) {
        if (op->do_only || (SG_NVME_CTL_NSID == nsid ) ||
            (SG_NVME_BROADCAST_NSID == nsid)) {
            nvme_hex_raw(id_dinp, pg_sz, op);
            goto fini;
        }
        goto skip1;
    }
    show_nvme_id_ctrl(id_dinp, op, jop);
skip1:
    if (op->do_only)
        goto fini;
    if (nsid > 0) {
        if (! (op->do_raw || (op->do_hex > 2))) {
            sgj_pr_hr(jsp, "  Namespace %u (deduced from device name):\n",
                      nsid);
            if (nsid > max_nsid)
                pr2serr("NSID from device (%u) should not exceed number of "
                        "namespaces (%u)\n", nsid, max_nsid);
        }
        ret = nvme_id_namespace(ptvp, nsid, id_cmdp, id_dinp, pg_sz, op, jop);
        if (ret)
            goto err_out;

    } else {        /* nsid=0 so char device; loop over all namespaces */
        for (k = 1; k <= max_nsid; ++k) {
            if ((! op->do_raw) || (op->do_hex < 3))
                sgj_pr_hr(jsp, "  Namespace %u (of %u):\n", k, max_nsid);
            ret = nvme_id_namespace(ptvp, k, id_cmdp, id_dinp, pg_sz, op,
                                    jop);
            if (ret)
                goto err_out;
            if (op->do_raw || op->do_hex)
                goto fini;
        }
    }
fini:
    ret = 0;
err_out:
    free(free_id_dinp);
    return ret;
}
#endif          /* (HAVE_NVME && (! IGNORE_NVME)) */


int
main(int argc, char * argv[])
{
    bool as_json = false;
    int res, err, vb;
    int sg_fd = -1;
    int ret = 0;
    int subvalue = 0;
    int inhex_len = 0;
    int inraw_len = 0;
    const char * cp;
    const struct svpd_values_name_t * vnp;
    struct sg_pt_base * ptvp = NULL;
    sgj_state * jsp;
    sgj_opaque_p jop = NULL;
    struct opts_t opts SG_C_CPP_ZERO_INIT;
    struct opts_t * op;

    op = &opts;
    op->cns = -1;
    op->vpd_pn = -1;
    op->vend_prod_num = -1;
    op->page_pdt = -1;
    op->do_block = -1;         /* use default for OS */
    if (getenv("SG3_UTILS_INVOCATION"))
        sg_rep_invocation(MY_NAME, version_str, argc, argv, stderr);

    res = parse_cmd_line(op, argc, argv);
    if (res)
        return SG_LIB_SYNTAX_ERROR;
    if (op->do_help) {
        usage_for(op);
        if (op->do_help > 1) {
            pr2serr("\n>>> Available %s abbreviations:\n", vpd_pg_s);
            enumerate_vpds();
        }
        return 0;
    }

#ifdef DEBUG
    pr2serr("In DEBUG mode, ");
    if (op->verbose_given && op->version_given) {
        pr2serr("but override: '-vV' given, zero verbose and continue\n");
        op->verbose_given = false;
        op->version_given = false;
        op->verbose = 0;
    } else if (! op->verbose_given) {
        pr2serr("set '-vv'\n");
        op->verbose = 2;
    } else
        pr2serr("keep verbose=%d\n", op->verbose);
#else
    if (op->verbose_given && op->version_given)
        pr2serr("Not in DEBUG mode, so '-vV' has no special action\n");
#endif
    if (op->version_given) {
        pr2serr("Version string: %s\n", version_str);
        return 0;
    }
    vb = op->verbose;
    if (op->do_debug) {
        if (vb > 0)
            pr2serr("debug option set: changes meaning of some --hex "
                    "options\n");
        if (op->do_hex > 0)
            op->do_hex = -op->do_hex;
    }

    jsp = &op->json_st;
    if (op->do_json) {
        if (! sgj_init_state(jsp, op->json_arg)) {
            int bad_char = jsp->first_bad_char;
            char e[1500];

            if (bad_char) {
                pr2serr("bad argument to --json= option, unrecognized "
                        "character '%c'\n\n", bad_char);
            }
            sg_json_usage(0, e, sizeof(e));
            pr2serr("%s", e);
            ret = SG_LIB_SYNTAX_ERROR;
            goto err_out;
        }
        jop = sgj_start_r(MY_NAME, version_str, argc, argv, jsp);
    }
    as_json = jsp->pr_as_json;
    if (op->page_str) {
        if (op->vpd_pn >= 0) {
            pr2serr("Given '-p' option and another option that "
                    "implies a page\n");
            return SG_LIB_CONTRADICT;
        }
        if ('-' == op->page_str[0])
            op->vpd_pn = VPD_NOPE_WANT_STD_INQ;
        else if (isalpha((uint8_t)op->page_str[0])) {
            vnp = sdp_find_vpd_by_acron(op->page_str);
            if (NULL == vnp) {
#ifdef SG_SCSI_STRINGS
                if (op->opt_new)
                    pr2serr("abbreviation %s given to '--page=' "
                            "not recognized\n", op->page_str);
                else
                    pr2serr("abbreviation %s given to '-p=' "
                            "not recognized\n", op->page_str);
#else
                pr2serr("abbreviation %s given to '--page=' "
                        "not recognized\n", op->page_str);
#endif
                pr2serr(">>> Available abbreviations:\n");
                enumerate_vpds();
                return SG_LIB_SYNTAX_ERROR;
            }
            // if ((1 != op->do_hex) && (0 == op->do_raw))
            if (0 == op->do_raw)
                op->do_decode = true;
            op->vpd_pn = vnp->value;
            subvalue = vnp->subvalue;
            op->page_pdt = vnp->pdt;
        } else {
            cp = strchr(op->page_str, ',');
            if (cp && op->vend_prod_arg) {
                pr2serr("the --page=pg,vp and the --vendor=vp forms overlap, "
                        "choose one or the other\n");
                ret = SG_LIB_SYNTAX_ERROR;
                goto err_out;
            }
            op->vpd_pn = sg_get_num_nomult(op->page_str);
            if ((op->vpd_pn < 0) || (op->vpd_pn > 255)) {
                pr2serr("Bad page code value after '-p' option\n");
                printf("Available standard %s:\n", vpd_pg_s);
                enumerate_vpds(/* 1, 1 */);
                ret = SG_LIB_SYNTAX_ERROR;
                goto err_out;
            }
            if (cp) {
                if (isdigit((uint8_t)*(cp + 1)))
                    op->vend_prod_num = sg_get_num_nomult(cp + 1);
                else
                    op->vend_prod_num = svpd_find_vp_num_by_acron(cp + 1);
                if ((op->vend_prod_num < 0) || (op->vend_prod_num > 255)) {
                    pr2serr("Bad vendor/product acronym after comma in '-p' "
                            "option\n");
                    if (op->vend_prod_num < 0)
                        svpd_enumerate_vendor(-1);
                    ret = SG_LIB_SYNTAX_ERROR;
                    goto err_out;
                }
                subvalue = op->vend_prod_num;
            } else if (op->vend_prod_arg) {
                if (isdigit((uint8_t)op->vend_prod_arg[0]))
                    op->vend_prod_num = sg_get_num_nomult(op->vend_prod_arg);
                else
                    op->vend_prod_num =
                        svpd_find_vp_num_by_acron(op->vend_prod_arg);
                if ((op->vend_prod_num < 0) || (op->vend_prod_num > 255)) {
                    pr2serr("Bad vendor/product acronym after '--vendor=' "
                            "option\n");
                    svpd_enumerate_vendor(-1);
                    ret = SG_LIB_SYNTAX_ERROR;
                    goto err_out;
                }
                subvalue = op->vend_prod_num;
            }
        }
        if ((0 == op->do_raw) && (0 == op->do_hex))
            op->do_decode = true;
        if (vb > 3)
            pr2serr("'--page=' matched pn=%d [0x%x], subvalue=%d\n",
                    op->vpd_pn, op->vpd_pn, subvalue);
    } else if (op->vend_prod_arg) {
        if (isdigit((uint8_t)op->vend_prod_arg[0]))
            op->vend_prod_num = sg_get_num_nomult(op->vend_prod_arg);
        else
            op->vend_prod_num = svpd_find_vp_num_by_acron(op->vend_prod_arg);
        if ((op->vend_prod_num < 0) || (op->vend_prod_num > 255)) {
            pr2serr("Bad vendor/product acronym after '--vendor=' "
                    "option\n");
            svpd_enumerate_vendor(-1);
            ret = SG_LIB_SYNTAX_ERROR;
            goto err_out;
        }
        // subvalue = op->vend_prod_num;
    }
    if (as_json)
        jop = sgj_start_r(MY_NAME, version_str, argc, argv, jsp);

    rsp_buff = sg_memalign(rsp_buff_sz, 0 /* page align */, &free_rsp_buff,
                           false);
    if (NULL == rsp_buff) {
        pr2serr("Unable to allocate %d bytes on heap\n", rsp_buff_sz);
        return sg_convert_errno(ENOMEM);
    }
    if (op->sinq_inraw_fn) {
        if (op->do_cmddt) {
            pr2serr("Don't support --cmddt with --sinq-inraw= option\n");
            ret = SG_LIB_CONTRADICT;
            goto err_out;
        }
        /* Note: want to support both --sinq_inraw= and --inhex= options */
        if ((ret = sg_f2hex_arr(op->sinq_inraw_fn, true, false, rsp_buff,
                                &inraw_len, rsp_buff_sz))) {
            goto err_out;
        }
        if (inraw_len < 36) {
            pr2serr("Unable to read 36 or more bytes from %s\n",
                    op->sinq_inraw_fn);
            ret = SG_LIB_FILE_ERROR;
            goto err_out;
        }
        memcpy(op->std_inq_a,  rsp_buff, 36);
        op->std_inq_a_valid = true;
    }
    if (op->inhex_fn) {
        if (op->device_name) {
            pr2serr("Cannot have both a DEVICE and --inhex= option\n");
            ret = SG_LIB_CONTRADICT;
            goto err_out;
        }
        if (op->do_cmddt) {
            pr2serr("Don't support --cmddt with --inhex= option\n");
            ret = SG_LIB_CONTRADICT;
            goto err_out;
        }
        err = sg_f2hex_arr(op->inhex_fn, !!op->do_raw, false, rsp_buff,
                           &inhex_len, rsp_buff_sz);
        if (err) {
            if (err < 0)
                err = sg_convert_errno(-err);
            ret = err;
            goto err_out;
        }
        op->do_raw = 0;         /* don't want raw on output with --inhex= */
        if (-1 == op->vpd_pn) {       /* may be able to deduce VPD page */
            if (op->page_pdt < 0)
                op->page_pdt = PDT_MASK & rsp_buff[0];
            if ((0x2 == (0xf & rsp_buff[3])) && (rsp_buff[2] > 2)) {
                if (vb)
                    pr2serr("Guessing from --inhex= this is a standard "
                            "INQUIRY\n");
            } else if (rsp_buff[2] <= 2) {
                /*
                 * Removable devices have the RMB bit set, which would
                 * present itself as vpd page 0x80 output if we're not
                 * careful
                 *
                 * Serial number must be right-aligned ASCII data in
                 * bytes 5-7; standard INQUIRY will have flags here.
                 */
                if (rsp_buff[1] == 0x80 &&
                    (rsp_buff[5] < 0x20 || rsp_buff[5] > 0x80 ||
                     rsp_buff[6] < 0x20 || rsp_buff[6] > 0x80 ||
                     rsp_buff[7] < 0x20 || rsp_buff[7] > 0x80)) {
                    if (vb)
                        pr2serr("Guessing from --inhex= this is a "
                                "standard INQUIRY\n");
                } else {
                    if (vb)
                        pr2serr("Guessing from --inhex= this is VPD "
                                "page 0x%x\n", rsp_buff[1]);
                    op->vpd_pn = rsp_buff[1];
                    op->do_vpd = true;
                    if ((1 != op->do_hex) && (0 == op->do_raw))
                        op->do_decode = true;
                }
            } else {
                if (vb)
                    pr2serr("page number unclear from --inhex, hope it's a "
                            "standard INQUIRY\n");
            }
        } else
            op->do_vpd = true;
        if (op->do_vpd) {   /* Allow for multiple VPD pages from 'sg_vpd -a' */
            op->maxlen = inhex_len;
            ret = svpd_inhex_decode_all(op, jop);
            goto fini2;
        }
    } else if ((NULL == op->device_name) && (! op->std_inq_a_valid)) {
        pr2serr("No DEVICE argument given\n\n");
        pr2serr("Use '-h' or '--help' option for usage summary\n");
        ret = SG_LIB_SYNTAX_ERROR;
        goto err_out;
    }
    if (VPD_NOPE_WANT_STD_INQ == op->vpd_pn)
        op->vpd_pn = -1;  /* now past guessing, set to normal indication */

    if (op->do_export) {
        if (op->vpd_pn != -1) {
            if (op->vpd_pn != VPD_DEVICE_ID &&
                op->vpd_pn != VPD_UNIT_SERIAL_NUM) {
                pr2serr("Option '--export' only supported for %ss 0x80 and "
                        "0x83\n", vpd_pg_s);
                usage_for(op);
                ret = SG_LIB_CONTRADICT;
                goto err_out;
            }
            op->do_decode = true;
            op->do_vpd = true;
        }
    }

    if ((0 == op->do_cmddt) && (op->vpd_pn >= 0) && op->page_given)
        op->do_vpd = true;

    if (op->do_raw && op->do_hex) {
        pr2serr("Can't do hex and raw at the same time\n");
        usage_for(op);
        ret = SG_LIB_CONTRADICT;
        goto err_out;
    }
    if (op->do_vpd && op->do_cmddt) {
#ifdef SG_SCSI_STRINGS
        if (op->opt_new)
            pr2serr("Can't use '--cmddt' with %ss\n", vpd_pg_s);
        else
            pr2serr("Can't have both '-e' and '-c' (or '-cl')\n");
#else
        pr2serr("Can't use '--cmddt' with %ss\n", vpd_pg_s);
#endif
        usage_for(op);
        ret = SG_LIB_CONTRADICT;
        goto err_out;
    }
    if (((op->do_vpd || op->do_cmddt)) && (op->vpd_pn < 0))
        op->vpd_pn = 0;
    if (op->num_pages > 1) {
        pr2serr("Can only fetch one page (VPD or Cmd) at a time\n");
        usage_for(op);
        ret = SG_LIB_SYNTAX_ERROR;
        goto err_out;
    }
    if (op->do_descriptors) {
        if ((op->maxlen > 0) && (op->maxlen < 60)) {
            pr2serr("version descriptors need INQUIRY response "
                    "length >= 60 bytes\n");
            ret = SG_LIB_SYNTAX_ERROR;
            goto err_out;
        }
        if (op->do_vpd || op->do_cmddt) {
            pr2serr("version descriptors require standard INQUIRY\n");
            ret = SG_LIB_CONTRADICT;
            goto err_out;
        }
    }
    if (op->num_pages && op->do_ata) {
        pr2serr("Can't use '-A' with an explicit decode %s option\n",
                 vpd_pg_s);
        ret = SG_LIB_CONTRADICT;
        goto err_out;
    }

    if (op->do_raw) {
        if (sg_set_binary_mode(STDOUT_FILENO) < 0) {
            perror("sg_set_binary_mode");
            ret = SG_LIB_FILE_ERROR;
            goto err_out;
        }
    }
    if (op->inhex_fn) {
        if (op->do_vpd) {
            if (op->do_decode)
                ret = vpd_decode(NULL, op, jop, 0);
            else
                ret = vpd_mainly_hex(NULL, op, jop, 0);
            goto err_out;
        }
#if defined(SG_LIB_LINUX) && defined(SG_SCSI_STRINGS) && \
    defined(HDIO_GET_IDENTITY)
        else if (op->do_ata) {
            prepare_ata_identify(op, inhex_len);
            ret = 0;
            goto err_out;
        }
#endif
        else {
            op->maxlen = inhex_len;
            ret = std_inq_process(NULL, op, jop, 0);
            goto err_out;
        }
    } else if (op->std_inq_a_valid && (NULL == op->device_name)) {
        /* --sinq_inraw=RFN contents still in rsp_buff */
        if (op->do_raw)
            dStrRaw((const char *)rsp_buff, inraw_len);
        else if (op->do_hex) {
            if (! op->do_quiet && (op->do_hex < 3))
                sgj_pr_hr(jsp, "Standard Inquiry data format:\n");
            hex2stdout(rsp_buff, inraw_len, (1 == op->do_hex) ? 0 : -1);
        } else
            std_inq_decode(rsp_buff, inraw_len, op, jop);
        ret = 0;
        goto fini2;
    }

#if defined(O_NONBLOCK) && defined(O_RDONLY)
    if (op->do_block >= 0) {
        int n = O_RDONLY | (op->do_block ? 0 : O_NONBLOCK);

        if ((sg_fd = sg_cmds_open_flags(op->device_name, n, vb)) < 0) {
            pr2serr("sg_inq: error opening file: %s: %s\n",
                    op->device_name, safe_strerror(-sg_fd));
            ret = sg_convert_errno(-sg_fd);
            if (ret < 0)
                ret = SG_LIB_FILE_ERROR;
            goto err_out;
        }
    } else {
        if ((sg_fd = sg_cmds_open_device(op->device_name, true /* ro */,
                                         vb)) < 0) {
            pr2serr("sg_inq: error opening file: %s: %s\n",
                    op->device_name, safe_strerror(-sg_fd));
            ret = sg_convert_errno(-sg_fd);
            if (ret < 0)
                ret = SG_LIB_FILE_ERROR;
            goto err_out;
        }
    }
#else
    if ((sg_fd = sg_cmds_open_device(op->device_name, true /* ro */, vb)) <
        0) {
        pr2serr("sg_inq: error opening file: %s: %s\n",
                op->device_name, safe_strerror(-sg_fd));
        ret = sg_convert_errno(-sg_fd);
        if (ret < 0)
            ret = SG_LIB_FILE_ERROR;
        goto err_out;
    }
#endif
    memset(rsp_buff, 0, rsp_buff_sz);
    ptvp = construct_scsi_pt_obj_with_fd(sg_fd, vb);
    if (NULL == ptvp) {
        pr2serr("memory problem from construct_scsi_pt_obj_with_fd()\n");
        ret = sg_convert_errno(ENOMEM);
        goto err_out;
    }

#if (HAVE_NVME && (! IGNORE_NVME))
    if (pt_device_is_nvme(ptvp)) {   /* NVMe char or NVMe block */
        op->possible_nvme = true;
        if (! op->page_given) {
            ret = do_nvme_identify_ctrl(ptvp, op, jop);
            goto fini2;
        }
    }
#endif

#if defined(SG_LIB_LINUX) && defined(SG_SCSI_STRINGS) && \
    defined(HDIO_GET_IDENTITY)
    if (op->do_ata) {
        res = try_ata_identify(sg_fd, op->do_hex, op->do_raw, vb);
        if (0 != res) {
            pr2serr("fetching %s failed on %s\n", ai_vpdp, op->device_name);
            ret = SG_LIB_CAT_OTHER;
        } else
            ret = 0;
        goto fini3;
    }
#endif

    if ((! op->do_cmddt) && (! op->do_vpd)) {
        /* So it's a standard INQUIRY, try ATA IDENTIFY if that fails */
        ret = std_inq_process(ptvp, op, jop, 0);
        if (ret)
            goto err_out;
    } else if (op->do_cmddt) {
        if (op->vpd_pn < 0)
            op->vpd_pn = 0;
        ret = cmddt_process(sg_fd, op);
        if (ret)
            goto err_out;
    } else if (op->do_vpd) {
pr2serr("do_decode=%d\n", !! op->do_decode);
        if (op->do_decode) {
            ret = vpd_decode(ptvp, op, jop, 0);
            if (ret)
                goto err_out;
        } else {
            ret = vpd_mainly_hex(ptvp, op, jop, 0);
            if (ret)
                goto err_out;
        }
    }
fini2:

#if defined(SG_LIB_LINUX) && defined(SG_SCSI_STRINGS) && \
    defined(HDIO_GET_IDENTITY)
fini3:
#endif

err_out:
    if (free_rsp_buff)
        free(free_rsp_buff);
    if ((0 == vb) && (! op->do_export)) {
        if (! sg_if_can2stderr("sg_inq failed: ", ret))
            pr2serr("Some error occurred, try again with '-v' or '-vv' for "
                    "more information\n");
    }
    if (ptvp)
        destruct_scsi_pt_obj(ptvp);
    res = (sg_fd >= 0) ? sg_cmds_close_device(sg_fd) : 0;
    if (res < 0) {
        pr2serr("close error: %s\n", safe_strerror(-res));
        if (0 == ret)
            ret = sg_convert_errno(-res);
    }
    ret = (ret >= 0) ? ret : SG_LIB_CAT_OTHER;
    if (as_json && jop) {
        FILE * fp = stdout;

        if (op->js_file) {
            if ((1 != strlen(op->js_file)) || ('-' != op->js_file[0])) {
                fp = fopen(op->js_file, "w");   /* truncate if exists */
                if (NULL == fp) {
                    int e = errno;

                    pr2serr("unable to open file: %s [%s]\n", op->js_file,
                            safe_strerror(e));
                    ret = sg_convert_errno(e);
                }
            }
            /* '--js-file=-' will send JSON output to stdout */
        }
        if (fp)
            sgj_js2file(jsp, NULL, ret, fp);
        if (op->js_file && fp && (stdout != fp))
            fclose(fp);
        sgj_finish(jsp);
    }
    return ret;
}


#if defined(SG_LIB_LINUX) && defined(SG_SCSI_STRINGS) && \
    defined(HDIO_GET_IDENTITY)
/* Following code permits ATA IDENTIFY commands to be performed on
   ATA non "Packet Interface" devices (e.g. ATA disks).
   GPL-ed code borrowed from smartmontools (smartmontools.sf.net).
   Copyright (C) 2002-4 Bruce Allen
                <smartmontools-support@lists.sourceforge.net>
 */
#ifndef ATA_IDENTIFY_DEVICE
#define ATA_IDENTIFY_DEVICE 0xec
#define ATA_IDENTIFY_PACKET_DEVICE 0xa1
#endif
#ifndef HDIO_DRIVE_CMD
#define HDIO_DRIVE_CMD    0x031f
#endif

/* Needed parts of the ATA DRIVE IDENTIFY Structure. Those labeled
 * word* are NOT used.
 */
struct ata_identify_device {
  unsigned short words000_009[10];
  uint8_t  serial_no[20];
  unsigned short words020_022[3];
  uint8_t  fw_rev[8];
  uint8_t  model[40];
  unsigned short words047_079[33];
  unsigned short major_rev_num;
  unsigned short minor_rev_num;
  unsigned short command_set_1;
  unsigned short command_set_2;
  unsigned short command_set_extension;
  unsigned short cfs_enable_1;
  unsigned short word086;
  unsigned short csf_default;
  unsigned short words088_255[168];
};

#define ATA_IDENTIFY_BUFF_SZ  sizeof(struct ata_identify_device)
#define HDIO_DRIVE_CMD_OFFSET 4

static int
ata_command_interface(int device, char *data, bool * atapi_flag, int verbose)
{
    int err;
    uint8_t buff[ATA_IDENTIFY_BUFF_SZ + HDIO_DRIVE_CMD_OFFSET];
    unsigned short get_ident[256];

    if (atapi_flag)
        *atapi_flag = false;
    memset(buff, 0, sizeof(buff));
    if (ioctl(device, HDIO_GET_IDENTITY, &get_ident) < 0) {
        err = errno;
        if (ENOTTY == err) {
            if (verbose > 1)
                pr2serr("HDIO_GET_IDENTITY failed with ENOTTY, "
                        "try HDIO_DRIVE_CMD ioctl ...\n");
            buff[0] = ATA_IDENTIFY_DEVICE;
            buff[3] = 1;
            if (ioctl(device, HDIO_DRIVE_CMD, buff) < 0) {
                if (verbose)
                    pr2serr("HDIO_DRIVE_CMD(ATA_IDENTIFY_DEVICE) "
                            "ioctl failed:\n\t%s [%d]\n",
                            safe_strerror(err), err);
                return sg_convert_errno(err);
            }
            memcpy(data, buff + HDIO_DRIVE_CMD_OFFSET, ATA_IDENTIFY_BUFF_SZ);
            return 0;
        } else {
            if (verbose)
                pr2serr("HDIO_GET_IDENTITY ioctl failed:\n"
                        "\t%s [%d]\n", safe_strerror(err), err);
            return sg_convert_errno(err);
        }
    } else if (verbose > 1)
        pr2serr("HDIO_GET_IDENTITY succeeded\n");
    if (0x2 == ((get_ident[0] >> 14) &0x3)) {   /* ATAPI device */
        if (verbose > 1)
            pr2serr("assume ATAPI device from HDIO_GET_IDENTITY response\n");
        memset(buff, 0, sizeof(buff));
        buff[0] = ATA_IDENTIFY_PACKET_DEVICE;
        buff[3] = 1;
        if (ioctl(device, HDIO_DRIVE_CMD, buff) < 0) {
            err = errno;
            if (verbose)
                pr2serr("HDIO_DRIVE_CMD(ATA_IDENTIFY_PACKET_DEVICE) ioctl "
                        "failed:\n\t%s [%d]\n", safe_strerror(err), err);
            buff[0] = ATA_IDENTIFY_DEVICE;
            buff[3] = 1;
            if (ioctl(device, HDIO_DRIVE_CMD, buff) < 0) {
                err = errno;
                if (verbose)
                    pr2serr("HDIO_DRIVE_CMD(ATA_IDENTIFY_DEVICE) ioctl "
                            "failed:\n\t%s [%d]\n", safe_strerror(err), err);
                return sg_convert_errno(err);
            }
        } else if (atapi_flag) {
            *atapi_flag = true;
            if (verbose > 1)
                pr2serr("HDIO_DRIVE_CMD(ATA_IDENTIFY_DEVICE) succeeded\n");
        }
    } else {    /* assume non-packet device */
        buff[0] = ATA_IDENTIFY_DEVICE;
        buff[3] = 1;
        if (ioctl(device, HDIO_DRIVE_CMD, buff) < 0) {
            err = errno;
            if (verbose)
                pr2serr("HDIO_DRIVE_CMD(ATA_IDENTIFY_DEVICE) ioctl failed:"
                        "\n\t%s [%d]\n", safe_strerror(err), err);
            return sg_convert_errno(err);
        } else if (verbose > 1)
            pr2serr("HDIO_DRIVE_CMD(ATA_IDENTIFY_DEVICE) succeeded\n");
    }
    /* if the command returns data, copy it back */
    memcpy(data, buff + HDIO_DRIVE_CMD_OFFSET, ATA_IDENTIFY_BUFF_SZ);
    return 0;
}

static void
show_ata_identify(const struct ata_identify_device * aidp, bool atapi,
                  int vb)
{
    int res;
    char model[64];
    char serial[64];
    char firm[64];

    printf("%s device: model, serial number and firmware revision:\n",
           (atapi ? "ATAPI" : "ATA"));
    res = sg_ata_get_chars((const unsigned short *)aidp->model,
                           0, 20, sg_is_big_endian(), model);
    model[res] = '\0';
    res = sg_ata_get_chars((const unsigned short *)aidp->serial_no,
                           0, 10, sg_is_big_endian(), serial);
    serial[res] = '\0';
    res = sg_ata_get_chars((const unsigned short *)aidp->fw_rev,
                           0, 4, sg_is_big_endian(), firm);
    firm[res] = '\0';
    printf("  %s %s %s\n", model, serial, firm);
    if (vb) {
        if (atapi)
            printf("ATA IDENTIFY PACKET DEVICE response "
                   "(256 words):\n");
        else
            printf("ATA IDENTIFY DEVICE response (256 words):\n");
        dWordHex((const unsigned short *)aidp, 256, 0,
                 sg_is_big_endian());
    }
}

static void
prepare_ata_identify(const struct opts_t * op, int inhex_len)
{
    int n = inhex_len;
    struct ata_identify_device ata_ident;

    if (n < 16) {
        pr2serr("%s: got only %d bytes, give up\n", __func__, n);
        return;
    } else if (n < 512)
        pr2serr("%s: expect 512 bytes or more, got %d, continue\n", __func__,
                n);
    else if (n > 512)
        n = 512;
    memset(&ata_ident, 0, sizeof(ata_ident));
    memcpy(&ata_ident, rsp_buff, n);
    show_ata_identify(&ata_ident, false, op->verbose);
}

/* Returns 0 if successful, else errno of error */
static int
try_ata_identify(int ata_fd, int do_hex, int do_raw, int verbose)
{
    bool atapi;
    int res;
    struct ata_identify_device ata_ident;

    memset(&ata_ident, 0, sizeof(ata_ident));
    res = ata_command_interface(ata_fd, (char *)&ata_ident, &atapi, verbose);
    if (res)
        return res;
    if ((2 == do_raw) || (3 == do_hex))
        dWordHex((const unsigned short *)&ata_ident, 256, -2,
                 sg_is_big_endian());
    else if (do_raw)
        dStrRaw((const char *)&ata_ident, 512);
    else {
        if (do_hex) {
            if (atapi)
                printf("ATA IDENTIFY PACKET DEVICE response ");
            else
                printf("ATA IDENTIFY DEVICE response ");
            if (do_hex > 1) {
                printf("(512 bytes):\n");
                hex2stdout((const uint8_t *)&ata_ident, 512, 0);
            } else {
                printf("(256 words):\n");
                dWordHex((const unsigned short *)&ata_ident, 256, 0,
                         sg_is_big_endian());
            }
        } else
            show_ata_identify(&ata_ident, atapi, verbose);
    }
    return 0;
}
#endif
