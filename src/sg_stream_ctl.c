/*
 * Copyright (c) 2018-2023 Douglas Gilbert.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the BSD_LICENSE file.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <errno.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sg_lib.h"
#include "sg_lib_data.h"
#include "sg_pt.h"
#include "sg_cmds_basic.h"
#include "sg_unaligned.h"
#include "sg_pr2serr.h"
#include "sg_json_sg_lib.h"

/*
 * This program issues the SCSI GET STREAM STATUS or STREAM CONTROL command
 * to the given SCSI device. Based on sbc4r15.pdf .
 */

static const char * version_str = "1.16 20231020";
#define MY_NAME "sg_stream_ctl"

#define STREAM_CONTROL_SA 0x14
#define GET_STREAM_STATUS_SA 0x16

#define STREAM_CONTROL_OPEN 0x1
#define STREAM_CONTROL_CLOSE 0x2

#define SENSE_BUFF_LEN 64       /* Arbitrary, could be larger */
#define DEF_PT_TIMEOUT  60      /* 60 seconds */

#define DEF_MAXLEN  252


struct opts_t {
    bool ctl_given;
    bool do_brief;
    bool do_close;
    bool do_get;
    bool do_json;
    bool do_open;
    bool maxlen_given;
    bool o_readonly;
    bool read_only;
    bool verbose_given;
    bool version_given;

    uint16_t stream_id;
    uint32_t ctl;
    int do_hex;
    int maxlen;
    int do_raw;
    int verbose;
    const char * in_fn;
    const char * json_arg;
    const char * js_file;
    sgj_state json_st;
};


static const struct option long_options[] = {
    {"brief", no_argument, 0, 'b'},
    {"close", no_argument, 0, 'c'},
    {"ctl", required_argument, 0, 'C'},
    {"get", no_argument, 0, 'g'},
    {"help", no_argument, 0, 'h'},
    {"hex", no_argument, 0, 'H'},
    {"id", required_argument, 0, 'i'},
    {"inhex", required_argument, 0, 'I'},
    {"json", optional_argument, 0, '^'},    /* short option is '-j' */
    {"js-file", required_argument, 0, 'J'},
    {"js_file", required_argument, 0, 'J'},
    {"maxlen", required_argument, 0, 'm'},
    {"open", no_argument, 0, 'o'},
    /* if inhex_given { raw=0 --> inhex:hex, output:non-binary;
                        raw=1 --> inhex:binary, output:non-binary;
                        raw=2 --> inhex:hex, output:binary;
                        raw=3 --> inhex:binary, output:binary; } */
    {"raw", no_argument, 0, 'R'},       /* give multiple times for raw > 1 */
    {"readonly", no_argument, 0, 'r'},
    {"verbose", no_argument, 0, 'v'},
    {"version", no_argument, 0, 'V'},
    {0, 0, 0, 0},
};

static const char * gss_s = "Get stream status";
static int gss_s_sz;
static const char * sc_s = "Stream control";
static int sc_s_sz;
static const char * gss_pd_sn = "get_stream_status_parameter_data";
static const char * sc_pd_sn = "stream_control_parameter_data";
static const char * asid_sn = "assigned_stream_id";


static void
usage()
{
    pr2serr("Usage: "
            "sg_stream_ctl  [-brief] [--close] [--ctl=CTL] [-get] [--help]\n"
            "                      [--hex] [--id=SID] [--inhex=FN] "
            "[--json[=JO]]\n"
            "                      [--js-file=JFN] [--maxlen=LEN] [--open] "
            "[--raw]\n"
            "                      [--readonly] [--verbose] [--version] "
            "DEVICE\n");
    pr2serr("  where:\n"
            "    --brief|-b          for open, output assigned stream id to "
            "stdout, or\n"
            "                        -1 if error; for close, output 0, or "
            "-1; for get\n"
            "                        output list of stream id, 1 per line\n"
            "    --close|-c          close stream given by --id=SID\n"
            "    --ctl=CTL|-C CTL    CTL is stream control value, "
            "(STR_CTL field)\n"
            "                        1 -> open; 2 -> close\n"
            "    --get|-g            do GET STREAM STATUS command (default "
            "if no other)\n"
            "    --help|-h           print out usage message\n"
            "    --hex|-H            print 'get' output in hex\n"
            "    --id=SID|-i SID     for close, SID is stream_id to close; "
            "for get,\n"
            "                        list from and including this stream "
            "id\n"
            "    --inhex=FN|-I FN    input taken from file FN rather than "
            "DEVICE,\n"
            "                        assume it is ASCII hex or, if --raw, "
            "in binary\n"
            "    --json[=JO]|-j[=JO]     output in JSON instead of plain "
            "text\n"
            "                            use --json=? for JSON help\n"
            "    --js-file=JFN|-J JFN    JFN is a filename to which JSON "
            "output is\n"
            "                            written (def: stdout); truncates "
            "then writes\n"
            "    --maxlen=LEN|-m LEN    length in bytes of buffer to "
            "receive data-in\n"
            "                           (def: 8 (for open and close); 252 "
            "(for get,\n"
            "                           but increase if needed)\n"
            "    --open|-o           open a new stream, return assigned "
            "stream id\n"
            "    --raw|-R            --hex output or --inhex= input: in "
            "binary\n"
            "                        instead of hex\n"
            "    --readonly|-r       open DEVICE read-only (if supported)\n"
            "    --verbose|-v        increase verbosity\n"
            "    --version|-V        print version string and exit\n\n"
            "Performs a SCSI GET STREAM STATUS or STREAM CONTROL command. "
            "If --open,\n--close or --ctl=CTL given (only one) then "
            "performs STREAM CONTROL\ncommand. If --get or no other "
            "selecting option given then performs a\nGET STREAM STATUS "
            "command. A successful --open will output the assigned\nstream "
            "id to stdout (and ignore --id=SID , if given).\n"
           );
}

/* Invokes a SCSI GET STREAM STATUS command (SBC-4).  Return of 0 -> success,
 * various SG_LIB_CAT_* positive values or -1 -> other errors */
static int
sg_ll_get_stream_status(int sg_fd, uint16_t s_str_id, uint8_t * resp,
                        uint32_t alloc_len, int * residp, bool noisy, int vb)
{
    int k, ret, res, sense_cat;
    uint8_t gssCdb[16] = {SG_SERVICE_ACTION_IN_16,
           GET_STREAM_STATUS_SA, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t sense_b[SENSE_BUFF_LEN] SG_C_CPP_ZERO_INIT;
    struct sg_pt_base * ptvp;

    if (s_str_id)         /* starting stream id, fetch from and including */
        sg_put_unaligned_be16(s_str_id, gssCdb + 4);
    sg_put_unaligned_be32(alloc_len, gssCdb + 10);
    if (vb) {
        char b[128];

        pr2serr("    %s cdb: %s\n", gss_s,
                sg_get_command_str(gssCdb, (int)sizeof(gssCdb), false,
                                   sizeof(b), b));
    }

    ptvp = construct_scsi_pt_obj_with_fd(sg_fd, vb);
    if (NULL == ptvp) {
        pr2serr("%s: out of memory\n", gss_s);
        return -1;
    }
    set_scsi_pt_cdb(ptvp, gssCdb, sizeof(gssCdb));
    set_scsi_pt_data_in(ptvp, resp, alloc_len);
    set_scsi_pt_sense(ptvp, sense_b, sizeof(sense_b));
    res = do_scsi_pt(ptvp, -1, DEF_PT_TIMEOUT, vb);
    ret = sg_cmds_process_resp(ptvp, gss_s, res, noisy, vb, &sense_cat);
    if (-1 == ret) {
        if (get_scsi_pt_transport_err(ptvp))
            ret = SG_LIB_TRANSPORT_ERROR;
        else
            ret = sg_convert_errno(get_scsi_pt_os_err(ptvp));
    } else if (-2 == ret) {
        switch (sense_cat) {
        case SG_LIB_CAT_RECOVERED:
        case SG_LIB_CAT_NO_SENSE:
            ret = 0;
            break;
        default:
            ret = sense_cat;
            break;
        }
    } else
        ret = 0;
    k = ret ? (int)alloc_len : get_scsi_pt_resid(ptvp);
    if (residp)
        *residp = k;
    if ((vb > 2) && ((alloc_len - k) > 0)) {
        pr2serr("%s: parameter data returned:\n", gss_s);
        hex2stderr((const uint8_t *)resp, alloc_len - k, ((vb > 3) ? -1 : 1));
    }
    destruct_scsi_pt_obj(ptvp);
    return ret;
}

/* Invokes a SCSI STREAM CONTROL command (SBC-4).  Return of 0 -> success,
 * various SG_LIB_CAT_* positive values or -1 -> other errors.
 * N.B. The is a device modifying command that is SERVICE ACTION IN(16)
 * command since it has data-in buffer that for open returns the
 * ASSIGNED_STR_ID field . */
static int
sg_ll_stream_control(int sg_fd, uint32_t str_ctl, uint16_t str_id,
                     uint8_t * resp, uint32_t alloc_len, int * residp,
                     bool noisy, int vb)
{
    int k, ret, res, sense_cat;
    uint8_t scCdb[16] = {SG_SERVICE_ACTION_IN_16,
           STREAM_CONTROL_SA, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t sense_b[SENSE_BUFF_LEN] SG_C_CPP_ZERO_INIT;
    struct sg_pt_base * ptvp;

    if (str_ctl)
        scCdb[1] |= (str_ctl & 0x3) << 5;
    if (str_id)         /* Only used for close, stream id to close */
        sg_put_unaligned_be16(str_id, scCdb + 4);
    sg_put_unaligned_be32(alloc_len, scCdb + 10);
    if (vb) {
        char b[128];

        pr2serr("    %s cdb: %s\n", sc_s,
                sg_get_command_str(scCdb, (int)sizeof(scCdb), false,
                                   sizeof(b), b));
    }

    ptvp = construct_scsi_pt_obj_with_fd(sg_fd, vb);
    if (NULL == ptvp) {
        pr2serr("%s: out of memory\n", sc_s);
        return -1;
    }
    set_scsi_pt_cdb(ptvp, scCdb, sizeof(scCdb));
    set_scsi_pt_data_in(ptvp, resp, alloc_len);
    set_scsi_pt_sense(ptvp, sense_b, sizeof(sense_b));
    res = do_scsi_pt(ptvp, -1, DEF_PT_TIMEOUT, vb);
    ret = sg_cmds_process_resp(ptvp, sc_s, res, noisy, vb, &sense_cat);
    if (-1 == ret) {
        if (get_scsi_pt_transport_err(ptvp))
            ret = SG_LIB_TRANSPORT_ERROR;
        else
            ret = sg_convert_errno(get_scsi_pt_os_err(ptvp));
    } else if (-2 == ret) {
        switch (sense_cat) {
        case SG_LIB_CAT_RECOVERED:
        case SG_LIB_CAT_NO_SENSE:
            ret = 0;
            break;
        default:
            ret = sense_cat;
            break;
        }
    } else
        ret = 0;
    k = ret ? (int)alloc_len : get_scsi_pt_resid(ptvp);
    if (residp)
        *residp = k;
    if ((vb > 2) && ((alloc_len - k) > 0)) {
        pr2serr("%s: parameter data returned:\n", sc_s);
        hex2stderr((const uint8_t *)resp, alloc_len - k, ((vb > 3) ? -1 : 1));
    }
    destruct_scsi_pt_obj(ptvp);
    return ret;
}

static void
dStrRaw(const uint8_t * str, int len)
{
    int k;

    for (k = 0; k < len; ++k)
        printf("%c", (int)(uint32_t)str[k]);
}

/* Handles short options after '-j' including a sequence of short options
 * that include one 'j' (for JSON). Want optional argument to '-j' to be
 * prefixed by '='. Return 0 for good, SG_LIB_SYNTAX_ERROR for syntax error
 * and SG_LIB_OK_FALSE for exit with no error. */
static int
chk_short_opts(const char sopt_ch, struct opts_t * op)
{
    /* only need to process short, non-argument options */
    switch (sopt_ch) {
    case 'b':
        op->do_brief = true;
        break;
    case 'c':
        op->do_close = true;
        break;
    case 'g':
        op->do_get = true;
        break;
    case 'h':
    case '?':
        usage();
        return SG_LIB_OK_FALSE;
    case 'H':
        ++op->do_hex;
        break;
    case 'j':
        break;  /* simply ignore second 'j' (e.g. '-jxj') */
    case 'o':
        op->do_open = true;
        break;
    case 'r':
        op->o_readonly = true;
        break;
    case 'R':
        ++op->do_raw;
        break;
    case 'v':
        op->verbose_given = true;
        ++op->verbose;
        break;
    case 'V':
        op->version_given = true;
        break;
    default:
        pr2serr("unrecognised option code %c [0x%x] ??\n", sopt_ch, sopt_ch);
        usage();
        return SG_LIB_SYNTAX_ERROR;
    }
    return 0;
}


int
main(int argc, char * argv[])
{
    bool no_final_msg = false;
    int c, k, n, res, in_len;
    int sg_fd = -1;
    int resid = 0;
    int ret = 0;
    uint16_t num_streams = 0;
    uint32_t pg_sz = sg_get_page_size();
    uint32_t param_dl;
    const char * device_name = NULL;
    uint8_t * arr = NULL;
    uint8_t * free_arr = NULL;
    struct opts_t * op;
    sgj_opaque_p jop = NULL;
    sgj_opaque_p jo2p = NULL;
    sgj_opaque_p jo3p = NULL;
    sgj_opaque_p jap = NULL;
    sgj_state * jsp;
    struct opts_t opts SG_C_CPP_ZERO_INIT;
    char b[128];
    static const int blen = sizeof(b);

    op = &opts;
    gss_s_sz = strlen(gss_s);
    sc_s_sz = strlen(sc_s);
    if (getenv("SG3_UTILS_INVOCATION"))
        sg_rep_invocation(MY_NAME, version_str, argc, argv, stderr);
    op->maxlen = DEF_MAXLEN;

    while (1) {
        int option_index = 0;

        c = getopt_long(argc, argv, "^bcC:ghHi:I:j::J:m:orRvV", long_options,
                        &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'b':
            op->do_brief = true;
            break;
        case 'c':
            op->do_close = true;
            break;
        case 'C':
            if ((1 != sscanf(optarg, "%4u", &op->ctl)) || (op->ctl > 3)) {
                pr2serr("--ctl= expects a number from 0 to 3\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            op->ctl_given = true;
            break;
        case 'g':
            op->do_get = true;
            break;
        case 'h':
        case '?':
            usage();
            return 0;
        case 'H':
            ++op->do_hex;
            break;
        case 'i':
            k = sg_get_num(optarg);
            if ((k < 0) || (k > (int)UINT16_MAX)) {
                pr2serr("--id= expects a number from 0 to 65535\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            op->stream_id = (uint16_t)k;
            break;
        case 'I':
            op->in_fn = optarg;
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
                    int q = chk_short_opts(*(optarg + k), op);

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
        case 'm':
            k = sg_get_num(optarg);
            if (k < 0) {
                pr2serr("--maxlen= unable to decode argument\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            op->maxlen_given = true;
            if (k > 0)
                op->maxlen = k;
            break;
        case 'o':
            op->do_open = true;
            break;
        case 'r':
            op->read_only = true;
            break;
        case 'R':
            ++op->do_raw;
            break;
        case 'v':
            op->verbose_given = true;
            ++op->verbose;
            break;
        case 'V':
            op->version_given = true;
            break;
        default:
            pr2serr("unrecognised option code 0x%x ??\n", c);
            usage();
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    if (optind < argc) {
        if (NULL == device_name) {
            device_name = argv[optind];
            ++optind;
        }
        if (optind < argc) {
            for (; optind < argc; ++optind)
                pr2serr("Unexpected extra argument: %s\n",
                        argv[optind]);
            usage();
            return SG_LIB_SYNTAX_ERROR;
        }
    }
#ifdef DEBUG
    if (! op->do_json)
        pr2serr("In DEBUG mode, ");
    if (op->verbose_given && op->version_given) {
        if (! op->do_json)
            pr2serr("but override: '-vV' given, zero verbose and continue\n");
        /* op->verbose_given = false; */
        op->version_given = false;
        op->verbose = 0;
    } else if (! op->verbose_given) {
        if (! op->do_json)
            pr2serr("set '-vv'\n");
        op->verbose = 2;
    } else if (! op->do_json)
        pr2serr("keep verbose=%d\n", op->verbose);
#else
    if (op->verbose_given && op->version_given && (! op->do_json))
        pr2serr("Not in DEBUG mode, so '-vV' has no special action\n");
#endif
    if (op->version_given) {
        pr2serr("version: %s\n", version_str);
        return 0;
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
            goto fini;
        }
        jop = sgj_start_r(MY_NAME, version_str, argc, argv, jsp);
    }
    if (op->maxlen > (int)pg_sz)
        arr = sg_memalign(op->maxlen, pg_sz, &free_arr, op->verbose > 3);
    else
        arr = sg_memalign(pg_sz, pg_sz, &free_arr, op->verbose > 3);
    if (NULL == arr) {
        pr2serr("Unable to allocate space for response\n");
        ret = sg_convert_errno(ENOMEM);
        goto fini;
    }

    k = (int)op->do_close + (int)op->do_get + (int)op->do_open +
        (int)op->ctl_given;
    if (k > 1) {
        pr2serr("Can only have one of: --close, --ctl==, --get, or --open\n");
        ret = SG_LIB_CONTRADICT;
        goto fini;
    } else if (0 == k)
        op->do_get = true;
    if (op->do_close)
        op->ctl = STREAM_CONTROL_CLOSE;
    else if (op->do_open)
        op->ctl = STREAM_CONTROL_OPEN;

    if (device_name && op->in_fn) {
        pr2serr("ignoring DEVICE, best to give DEVICE or --inhex=FN, but "
                "not both\n");
        device_name = NULL;
    }
    if (NULL == device_name) {
        if (op->in_fn) {
            if ((ret = sg_f2hex_arr(op->in_fn, 0x1 & op->do_raw,
                                    false, arr, &in_len, op->maxlen))) {
                if (SG_LIB_LBA_OUT_OF_RANGE == ret) {
                    pr2serr("--maxlen=%d needs to be increased\n",
                            op->maxlen);
                    pr2serr("... decode what we have\n");
                    no_final_msg = true;
                } else
                    goto fini;
            }
            if (op->verbose > 2)
                pr2serr("Read %d [0x%x] bytes of user supplied data\n",
                        in_len, in_len);
            if (op->do_raw > 0) /* if raw=2 or 3, output is in binary */
                op->do_raw >>= 1;    /* can interfere on decode */
            if (in_len < 4) {
                pr2serr("--in=%s only decoded %d bytes (needs 4 at least)\n",
                        op->in_fn, in_len);
                ret = SG_LIB_SYNTAX_ERROR;
                goto fini;
            }
            op->maxlen = in_len;
            if (op->do_get)
                goto start_get_response;
            else
                goto start_non_get_response;
        } else {
            pr2serr("missing device name!\n\n");
            if (! jsp->pr_as_json)
                usage();
            ret = SG_LIB_FILE_ERROR;
            no_final_msg = true;
            goto fini;
        }
    }
    if (0x1 & op->do_raw) {
        if (sg_set_binary_mode(STDOUT_FILENO) < 0) {
            perror("sg_set_binary_mode");
            ret = SG_LIB_FILE_ERROR;
            goto fini;
        }
    }
#if 0
    if (NULL == device_name) {
        pr2serr("missing device name!\n\n");
        usage();
        return SG_LIB_SYNTAX_ERROR;
    }
#endif

    if (op->maxlen_given) {
        if (0 == op->maxlen)
            op->maxlen = op->do_get ? 248 : 8;
    } else
        op->maxlen = op->do_get ? 248 : 8;

    if (op->verbose) {
        if (op->read_only && (! op->do_get))
            pr2serr("Probably need to open %s read-write\n", device_name);
        if (op->do_open && (op->stream_id > 0))
            pr2serr("With --open the --id-SID option is ignored\n");
    }

    sg_fd = sg_cmds_open_device(device_name, op->read_only, op->verbose);
    if (sg_fd < 0) {
        if (op->verbose)
            pr2serr("open error: %s: %s\n", device_name,
                    safe_strerror(-sg_fd));
        ret = sg_convert_errno(-sg_fd);
        goto fini;
    }

    resid = 0;
    if (op->do_get) {       /* Get stream status */
        ret = sg_ll_get_stream_status(sg_fd, op->stream_id, arr, op->maxlen,
                                      &resid, false, op->verbose);
        if (ret) {
            if (SG_LIB_CAT_INVALID_OP == ret)
                pr2serr("%s command not supported\n", gss_s);
            else {
                sg_get_category_sense_str(ret, sizeof(b), b, op->verbose);
                pr2serr("%s command: %s\n", gss_s, b);
            }
            goto fini;
        }
start_get_response:
        k = op->maxlen - resid;

        if (0x1 & op->do_raw) {  /* when --raw or --hex, forget about JSON */
            dStrRaw(arr, k);
            goto fini;
        } else if (op->do_hex) {
            if (op->do_hex > 2) {
                if (op->do_hex > 3) {
                    if (4 == op->do_hex)
                        printf("\n# %s:\n", gss_s);
                    else
                        printf("\n# %s [0x%x,0x%x]:\n", gss_s,
                               SG_SERVICE_ACTION_IN_16, GET_STREAM_STATUS_SA);
                }
                hex2stdout(arr, k, -1);
            } else
                hex2stdout(arr, k, (2 == op->do_hex) ? 0 : 1);
            goto fini;
        }
        jo2p = sgj_named_subobject_r(jsp, jop, gss_pd_sn);
        if (k < 8) {
            pr2serr("Response too short (%d bytes) assigned stream id\n",
                    k);
            ret = SG_LIB_CAT_MALFORMED;
            goto fini;
        } else
            op->maxlen = k;
        param_dl = sg_get_unaligned_be32(arr + 0);
        sgj_js_nv_ihex(jsp, jo2p, "parameter_data_length", param_dl);
        param_dl += 8;
        if (param_dl > (uint32_t)op->maxlen) {
            pr2serr("Response truncated, need to set --maxlen=%u\n",
                    param_dl);
            if (op->maxlen < (8 /* header */ + 4 /* enough of first */)) {
                pr2serr("Response too short to continue\n");
                goto fini;
            }
        }
#if 0
sgj_haj_vi(jsp, jo2p, 0, "Parameter data length",
           SGJ_SEP_COLON_1_SPACE, param_dl, false);
sgj_pr_hr(jsp, "No complete physical element status descriptors "
                  "available\n");
sgj_js_nv_ihex(jsp, jo2p, "element_identifier",
                           (int64_t)a_ped.elem_id);
#endif
        num_streams = sg_get_unaligned_be16(arr + 6);
        if (! op->do_brief) {
            if (op->stream_id > 0)
                sgj_pr_hr(jsp, "Starting at stream id: %u\n", op->stream_id);
            sgj_pr_hr(jsp, "Number of open streams: %u\n", num_streams);
        }
        sgj_js_nv_ihex(jsp, jo2p, "number_of_open_streams", num_streams);
        op->maxlen = ((uint32_t)op->maxlen < param_dl) ? op->maxlen :
                                                         (int)param_dl;
        if (jsp->pr_as_json)
            jap = sgj_named_subarray_r(jsp, jo2p, "stream_status_descriptor");

        for (k = 8; k < op->maxlen; k += 8) {
            uint8_t perm = (0x1 & arr[k]);
            uint8_t rel_lt = (0x7f & arr[k + 4]);
            uint16_t strm_id = sg_get_unaligned_be16(arr + k + 2);

            if (jsp->pr_as_json) {
                jo3p = sgj_new_unattached_object_r(jsp);
                sgj_js_nv_ihex_nex(jsp, jo3p, "perm", perm, false,
                                   "permanent stream");
                sgj_js_nv_ihex(jsp, jo3p, "element_identifier", strm_id);
                sgj_js_nv_ihex(jsp, jo3p, "relative_lifetime", rel_lt);
            }
            if (op->do_brief)
                sgj_pr_hr(jsp, "  %u\n", strm_id);
            else {
                sgj_pr_hr(jsp, "  PERM: %u\n", perm);
                sgj_pr_hr(jsp, "    Open stream id: %u\n", strm_id);
                sgj_pr_hr(jsp, "    Relative lifetime: %u\n", rel_lt);
            }
            if (jsp->pr_as_json)
                sgj_js_nv_o(jsp, jap, NULL /* name */, jo3p);
        }
    } else {            /* Stream control */
        uint16_t strm_id;
        int ln;

        ret = sg_ll_stream_control(sg_fd, op->ctl, op->stream_id, arr,
                                   op->maxlen, &resid, false, op->verbose);
        if (ret) {
            if (SG_LIB_CAT_INVALID_OP == ret)
                pr2serr("%s command not supported\n", sc_s);
            else {
                sg_get_category_sense_str(ret, sizeof(b), b, op->verbose);
                pr2serr("%s command: %s\n", sc_s, b);
            }
            goto fini;
        }
start_non_get_response:
        k = op->maxlen - resid;
        if ((k < 5) && op->do_open)
            goto bad_sc;
        else if (k < 5) {
            if (op->verbose)
                pr2serr("Response too short (%d bytes) on non-open Stream "
                        "control\n", k);
            goto fini;
        }
        if (0x1 & op->do_raw) {  /* when --raw or --hex, forget about JSON */
            dStrRaw(arr, k);
            goto fini;
        } else if (op->do_hex) {
            if (op->do_hex > 2) {
                if (op->do_hex > 3) {
                    if (4 == op->do_hex)
                        printf("\n# %s:\n", gss_s);
                    else
                        printf("\n# %s [0x%x,0x%x]:\n", gss_s,
                               SG_SERVICE_ACTION_IN_16, GET_STREAM_STATUS_SA);
                }
                hex2stdout(arr, k, -1);
            } else
                hex2stdout(arr, k, (2 == op->do_hex) ? 0 : 1);
            goto fini;
        }
        ln = arr[0] + 1;
        if (ln < k)
            k = ln;
        if (k < 5)
            goto bad_sc;
        strm_id = sg_get_unaligned_be16(arr + 4);
        if (op->do_brief)
            sgj_pr_hr(jsp, "%u\n", strm_id);
        else
            sgj_pr_hr(jsp, "Assigned stream id: %u\n", strm_id);
        if (jsp->pr_as_json) {
            jo2p = sgj_named_subobject_r(jsp, jop, sc_pd_sn);
            sgj_js_nv_ihex(jsp, jo2p, asid_sn, strm_id);
        }
    }
    goto fini;
bad_sc:
    pr2serr("Response too short (%d bytes) assigned stream id\n", k);
    sgj_pr_hr(jsp, "-1\n");
    ret = SG_LIB_CAT_MALFORMED;

fini:
    if (free_arr)
        free(free_arr);
    if (sg_fd >= 0) {
        res = sg_cmds_close_device(sg_fd);
        if (res < 0) {
            pr2serr("close error: %s\n", safe_strerror(-res));
            if (0 == ret)
                ret = sg_convert_errno(-res);
        }
    }
    if ((0 == op->verbose) && (! no_final_msg)) {
        if (! sg_if_can2stderr("sg_stream_ctl failed: ", ret))
            pr2serr("Some error occurred, try again with '-v' or '-vv' for "
                    "more information\n");
    }
    ret = (ret >= 0) ? ret : SG_LIB_CAT_OTHER;
    if (jsp->pr_as_json) {
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
        if (fp) {
            const char * estr = NULL;

            if (sg_exit2str(ret, jsp->verbose, blen, b)) {
                if (strlen(b) > 0)
                    estr = b;
            }
            sgj_js2file_estr(jsp, NULL, ret, estr, fp);
        }
        if (op->js_file && fp && (stdout != fp))
            fclose(fp);
        sgj_finish(jsp);
    }
    return ret;
}
