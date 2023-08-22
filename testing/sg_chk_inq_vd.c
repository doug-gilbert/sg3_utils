/*
 * Copyright (c) 2023 Douglas Gilbert.
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
#include <string.h>
#include <getopt.h>
#include <ctype.h>
#include <errno.h>

#include "sg_lib.h"
#include "sg_lib_data.h"
#include "sg_pr2serr.h"

/* A utility program for the Linux OS SCSI subsystem.
 *
 * This program takes a stds-num.txt file from www.t10.org and
 * checks it against the version descriptors held in a table
 * within the ../src/sg_inq_data.c file
 * The online version of the version descriptor codes listed in
 * numerical order and in plain text is at:
 *   https://www.t10.org/lists/stds-num.txt
 */

static const char * version_str = "1.01 20230821";


#define MAX_LINE_LEN 1024

extern struct sg_lib_simple_value_name_t sg_version_descriptor_arr[];

static struct option long_options[] = {
        {"ascii", 0, 0, 'a'},
        {"bypass", 0, 0, 'b'},
        {"help", 0, 0, 'h'},
        {"verbose", 0, 0, 'v'},
        {"version", 0, 0, 'V'},
        {0, 0, 0, 0},
};

static void usage()
{
    fprintf(stderr, "Usage: "
            "sg_chk_inq_vd [--ascii] [--bypass] [--help] [--verbose] "
            "[--version]\n"
            "                     <version_descriptor_file>\n"
            "  where:\n"
            "    --ascii|-a         check ASCII (def: only check number "
            "equality)\n"
            "    --bypass|-b        bypass table entries below file entry\n"
            "    --help|-h          print out usage message\n"
            "    --verbose|-v       increase verbosity\n"
            "    --version|-V       print version string and exit\n\n"
            "Checks version descriptor codes in <version_descriptor_file> "
            "against\na table in the sg3_utils src/sg_inq_data.c file. "
            "Example:\n    sg_chk_inq_vd -a -b -v stds-num.txt\n"
           );

}

int main(int argc, char * argv[])
{
    bool do_ascii = false;
    bool do_bypass = false;
    int k, j, res, c, num, len;
    unsigned int vd_num;
    FILE * fp;
    int verbose = 0;
    char file_name[256];
    char line[MAX_LINE_LEN];
    char b[MAX_LINE_LEN];
    char bb[MAX_LINE_LEN];
    char * cp;
    const char * ccp;
    const struct sg_lib_simple_value_name_t * svnp =
                                        sg_version_descriptor_arr;
    int ret = 1;

    memset(file_name, 0, sizeof file_name);
    memset(line, 0, sizeof file_name);
    while (1) {
        int option_index = 0;

        c = getopt_long(argc, argv, "abhvV", long_options,
                        &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'a':
            do_ascii = true;
            break;
        case 'b':
            do_bypass = true;
            break;
        case 'h':
        case '?':
            usage();
            return 0;
        case 'v':
            ++verbose;
            break;
        case 'V':
            fprintf(stderr, "version: %s\n", version_str);
            return 0;
        default:
            fprintf(stderr, "unrecognised switch code 0x%x ??\n", c);
            usage();
            return 1;
        }
    }
    if (optind < argc) {
        if ('\0' == file_name[0]) {
            strncpy(file_name, argv[optind], sizeof(file_name) - 1);
            file_name[sizeof(file_name) - 1] = '\0';
            ++optind;
        }
        if (optind < argc) {
            for (; optind < argc; ++optind)
                fprintf(stderr, "Unexpected extra argument: %s\n",
                        argv[optind]);
            usage();
            return 1;
        }
    }

    if (0 == file_name[0]) {
        fprintf(stderr, "missing file name!\n");
        usage();
        return 1;
    }
    fp = fopen(file_name, "r");
    if (NULL == fp) {
        fprintf(stderr, "open error: %s: %s\n", file_name,
                safe_strerror(errno));
        return 1;
    }
    for (k = 0; (cp = fgets(line, sizeof(line) - 1, fp)) &&
                (svnp->value < 0xffff); ++k) {
        len = strlen(line);
        if (len < 1)
            continue;
        if (! isxdigit(line[0]))
            continue;
        if (verbose > 4)
            pr2serr("line %d: %s", k + 1, line);
        num = sscanf(line, "%xh %20s", &vd_num, b);
        if (num < 1) {
            if (verbose)
                fprintf(stderr, "Badly formed line number %d (num=%d)\n",
                        k + 1, num);
            continue;
        }
        if ((num > 1) && (0 == memcmp("to", b, 2))) {
            continue;           // skip lines like: '0961h to 097Fh  ....'
        }
        if (svnp->value != (int)vd_num) {
            if (do_bypass && (svnp->value < (int)vd_num)) {
                if (verbose > 0)
                    pr2serr("bypassing table entry: 0x%x\n", svnp->value);
                j = 0;
                do {
                    ++svnp;
                    ++j;
                } while (svnp->value < (int)vd_num);
                if ((j > 1) && (verbose > 0))
                    pr2serr("  stepped over %d following table entries\n\n",
                            j);
                if (svnp->value == (int)vd_num) {
                    ++svnp;
                    continue;
                }
            }
            pr2serr("mismatch at file line %d: 0x%x in file versus 0x%x "
                    "[b: %s]\n", k + 1, vd_num, svnp->value, b);
            break;
        }
        if (NULL == svnp->name)
            break;
        if (! do_ascii) {
            ++svnp;
            continue;
        }
        ccp = strchr(line, '\t');
        if (NULL == ccp)
            continue;

        strncpy(b , ccp + 1, sizeof(b) - 1);
        b[sizeof(b) - 1] = '\0';
        num = strlen(b);
        if (0xd == b[num - 2])
            b[num - 2] = '\0';
        b[num - 1] = '\0';
        num = strlen(b);
        for (j = 0; j < num; ++j)
            b[j] = toupper(b[j]);
        if (verbose > 5)
            pr2serr("%s\n", b);

        strncpy(bb , svnp->name, sizeof(bb) - 1);
        num = strlen(bb);
        if (num < (int)sizeof(bb))
            bb[num] = '\0';
        for (j = 0; j < num; ++j)
            bb[j] = toupper(bb[j]);
        if (verbose > 5)
            pr2serr("  %s\n", bb);

        if ((0 != memcmp(b, bb, strlen(b))) ||
            (0 != memcmp(b, bb, strlen(bb)))) {
            pr2serr("line %d differs: %s", k + 1, line);
            pr2serr("t>>  %s\n\n", svnp->name ? svnp->name : "<null>");
        }

        ++svnp;
    }
    if (NULL == cp) {
        if (feof(fp)) {
            if (svnp->value < 0xffff) {
                for (j = 0; (svnp->value < 0xffff) && (j < 1024);
                     ++j, ++svnp) { }
                if (j >= 1024)
                    pr2serr("short stds-num.txt file, run-away on internal "
                            "table\n");
                else
                    pr2serr("%d extra entries on end of internal table\n", j);
            } else if (verbose > 1)
                pr2serr("EOF detected on given file\n");
        } else
            pr2serr("fgets: %s\n", safe_strerror(errno));
    } else if (verbose > 5)
        pr2serr("last line of given line processed: %s\n", line);
    if (verbose)
        pr2serr("Finished on line %d of input file\n", k + 1);

    res = fclose(fp);
    if (EOF == res) {
        fprintf(stderr, "close error: %s\n", safe_strerror(errno));
        return 1;
    }
    return ret;
}
