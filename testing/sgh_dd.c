/* A utility program for copying files. Specialised for "files" that
 * represent devices that understand the SCSI command set.
 *
 * Copyright (C) 2018-2019 D. Gilbert
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is a specialisation of the Unix "dd" command in which
 * one or both of the given files is a scsi generic device.
 * A logical block size ('bs') is assumed to be 512 if not given. This
 * program complains if 'ibs' or 'obs' are given with some other value
 * than 'bs'. If 'if' is not given or 'if=-' then stdin is assumed. If
 * 'of' is not given or 'of=-' then stdout assumed.
 *
 * A non-standard argument "bpt" (blocks per transfer) is added to control
 * the maximum number of blocks in each transfer. The default value is 128.
 * For example if "bs=512" and "bpt=32" then a maximum of 32 blocks (16 KiB
 * in this case) are transferred to or from the sg device in a single SCSI
 * command.
 *
 * This version is designed for the linux kernel 2.4, 2.6, 3 and 4 series.
 *
 * sgp_dd is a Posix threads specialization of the sg_dd utility. Both
 * sgp_dd and sg_dd only perform special tasks when one or both of the given
 * devices belong to the Linux sg driver.
 *
 * sgh_dd further extends sgp_dd to use the experimental kernel buffer
 * sharing feature added in 3.9.02 .
 * N.B. This utility was previously called sgs_dd but there was already an
 * archived version of a dd variant called sgs_dd so this utility name was
 * renamed [20181221]
 */

#define _XOPEN_SOURCE 600
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#ifndef major
#include <sys/types.h>
#endif
#include <sys/time.h>
#include <linux/major.h>        /* for MEM_MAJOR, SCSI_GENERIC_MAJOR, etc */
#include <linux/fs.h>           /* for BLKSSZGET and friends */
#include <sys/mman.h>           /* for mmap() system call */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef HAVE_LINUX_SG_V4_HDR
/* Kernel uapi header contain __user decorations on user space pointers
 * to indicate they are unsafe in the kernel space. However glibc takes
 * all those __user decorations out from headers in /usr/include/linux .
 * So to stop compile errors when directly importing include/uapi/scsi/sg.h
 * undef __user before doing that include. */
#define __user

/* Want to block the original sg.h header from also being included. That
 * causes lots of multiple definition errors. This will only work if this
 * header is included _before_ the original sg.h header.  */
#define _SCSI_GENERIC_H         /* original kernel header guard */
#define _SCSI_SG_H              /* glibc header guard */

#include "uapi_sg.h"    /* local copy of include/uapi/scsi/sg.h */

#else
#define __user
#endif  /* end of: ifndef HAVE_LINUX_SG_V4_HDR */

#include "sg_lib.h"
#include "sg_cmds_basic.h"
#include "sg_io_linux.h"
#include "sg_unaligned.h"
#include "sg_pr2serr.h"


static const char * version_str = "1.20 20190212";

#ifdef __GNUC__
#ifndef  __clang__
#pragma GCC diagnostic ignored "-Wclobbered"
#endif
#endif

/* <<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>   xxxxxxxxxx   beware next line */
#define SGH_DD_READ_COMPLET_AFTER 1

#define DEF_BLOCK_SIZE 512
#define DEF_BLOCKS_PER_TRANSFER 128
#define DEF_BLOCKS_PER_2048TRANSFER 32
#define DEF_SCSI_CDBSZ 10
#define MAX_SCSI_CDBSZ 16


#define SENSE_BUFF_LEN 64       /* Arbitrary, could be larger */
#define READ_CAP_REPLY_LEN 8
#define RCAP16_REPLY_LEN 32

#define DEF_TIMEOUT 60000       /* 60,000 millisecs == 60 seconds */

#define SGP_READ10 0x28
#define SGP_WRITE10 0x2a
#define DEF_NUM_THREADS 4
#define MAX_NUM_THREADS SG_MAX_QUEUE

#ifndef RAW_MAJOR
#define RAW_MAJOR 255   /*unlikely value */
#endif

#define FT_OTHER 1              /* filetype other than one of the following */
#define FT_SG 2                 /* filetype is sg char device */
#define FT_RAW 4                /* filetype is raw char device */
#define FT_DEV_NULL 8           /* either "/dev/null" or "." as filename */
#define FT_ST 16                /* filetype is st char device (tape) */
#define FT_BLOCK 32             /* filetype is a block device */
#define FT_ERROR 64             /* couldn't "stat" file */

#define DEV_NULL_MINOR_NUM 3

#define EBUFF_SZ 768

struct flags_t {
    bool append;
    bool coe;
    bool defres;        /* without this res_sz==bs*bpt */
    bool dio;
    bool direct;
    bool dpo;
    bool dsync;
    bool excl;
    bool fua;
    bool mmap;
    bool noshare;
    bool noxfer;
    bool same_fds;
    bool swait;
    bool v3;
    bool v4;
};

typedef struct global_collection
{       /* one instance visible to all threads */
    int infd;
    int64_t skip;
    int in_type;
    int cdbsz_in;
    int help;
    int elem_sz;
    struct flags_t in_flags;
    // int64_t in_blk;                /* -\ next block address to read */
    int64_t in_count;                 /*  | blocks remaining for next read */
    int64_t in_rem_count;             /*  | count of remaining in blocks */
    int in_partial;                   /*  | */
    bool in_stop;                     /*  | */
    pthread_mutex_t in_mutex;         /* -/ */
    int outfd;
    int64_t seek;
    int out_type;
    int out2fd;
    int out2_type;
    int cdbsz_out;
    int aen;                          /* abort every nth command */
    struct flags_t out_flags;
    int64_t out_blk;                  /* -\ next block address to write */
    int64_t out_count;                /*  | blocks remaining for next write */
    int64_t out_rem_count;            /*  | count of remaining out blocks */
    int out_partial;                  /*  | */
    bool out_stop;                    /*  | */
    pthread_mutex_t out_mutex;        /*  | */
    pthread_cond_t out_sync_cv;       /*  | hold writes until "in order" */
    pthread_mutex_t out2_mutex;
    int bs;
    int bpt;
    int outregfd;
    int outreg_type;
    int dio_incomplete_count;   /* -\ */
    int sum_of_resids;          /* -/ */
    int debug;          /* both -v and deb=VERB bump this field */
    int dry_run;
    bool ofile_given;
    bool ofile2_given;
    const char * infp;
    const char * outfp;
    const char * out2fp;
} Gbl_coll;

typedef struct request_element
{       /* one instance per worker thread */
    bool wr;
    bool has_share;
    bool swait; /* interleave READ WRITE async copy segment: READ submit,
                 * WRITE submit, READ receive, WRITE receive */
    int id;
    int infd;
    int outfd;
    int out2fd;
    int outregfd;
    int64_t iblk;
    int64_t oblk;
    int num_blks;
    uint8_t * buffp;
    uint8_t * alloc_bp;
    struct sg_io_hdr io_hdr;
    struct sg_io_v4 io_hdr4;
    uint8_t cmd[MAX_SCSI_CDBSZ];
    uint8_t sb[SENSE_BUFF_LEN];
    int bs;
    int dio_incomplete_count;
    int resid;
    int cdbsz_in;
    int cdbsz_out;
    int aen;
    int rep_count;
    int rq_id;
    int mmap_len;
    struct flags_t in_flags;
    struct flags_t out_flags;
    int debug;
} Rq_elem;

typedef struct thread_info
{
    int id;
    Gbl_coll * gcp;
    pthread_t a_pthr;
} Thread_info;

static atomic_int mono_pack_id = 0;
static atomic_long pos_index = 0;

static sigset_t signal_set;
static pthread_t sig_listen_thread_id;

static const char * proc_allow_dio = "/proc/scsi/sg/allow_dio";

static void sg_in_rd_cmd(Gbl_coll * clp, Rq_elem * rep);
static void sg_out_wr_cmd(Gbl_coll * clp, Rq_elem * rep, bool is_wr2);
static bool normal_in_rd(Gbl_coll * clp, Rq_elem * rep, int blocks);
static void normal_out_wr(Gbl_coll * clp, Rq_elem * rep, int blocks);
static int sg_start_io(Rq_elem * rep, bool is_wr2);
static int sg_finish_io(bool wr, Rq_elem * rep, bool is_wr2);
static int sg_in_open(Gbl_coll *clp, const char *inf, uint8_t **mmpp,
                      int *mmap_len);
static int sg_out_open(Gbl_coll *clp, const char *outf, uint8_t **mmpp,
                       int *mmap_len);
static void sg_in_out_interleave(Gbl_coll *clp, Rq_elem * rep);

#define STRERR_BUFF_LEN 128

static pthread_mutex_t strerr_mut = PTHREAD_MUTEX_INITIALIZER;

static bool shutting_down = false;
static bool do_sync = false;
static bool do_time = true;
static Gbl_coll gcoll;
static struct timeval start_tm;
static int64_t dd_count = -1;
static int num_threads = DEF_NUM_THREADS;
static int exit_status = 0;
static volatile bool swait_reported = false;

static const char * my_name = "sgh_dd: ";


#ifdef __GNUC__
static int pr2serr_lk(const char * fmt, ...)
        __attribute__ ((format (printf, 1, 2)));
#if 0
static void pr_errno_lk(int e_no, const char * fmt, ...)
        __attribute__ ((format (printf, 2, 3)));
#endif
#else
static int pr2serr_lk(const char * fmt, ...);
#if 0
static void pr_errno_lk(int e_no, const char * fmt, ...);
#endif
#endif


static int
pr2serr_lk(const char * fmt, ...)
{
    int n;
    va_list args;

    pthread_mutex_lock(&strerr_mut);
    va_start(args, fmt);
    n = vfprintf(stderr, fmt, args);
    va_end(args);
    pthread_mutex_unlock(&strerr_mut);
    return n;
}

#if 0   // not used yet
static void
pr_errno_lk(int e_no, const char * fmt, ...)
{
    char b[180];
    va_list args;

    pthread_mutex_lock(&strerr_mut);
    va_start(args, fmt);
    vsnprintf(b, sizeof(b), fmt, args);
    fprintf(stderr, "%s: %s\n", b, strerror(e_no));
    va_end(args);
    pthread_mutex_unlock(&strerr_mut);
}
#endif

static void
lk_print_command(uint8_t * cmdp)
{
    pthread_mutex_lock(&strerr_mut);
    sg_print_command(cmdp);
    pthread_mutex_unlock(&strerr_mut);
}

static void
lk_chk_n_print3(const char * leadin, struct sg_io_hdr * hp, bool raw_sinfo)
{
    pthread_mutex_lock(&strerr_mut);
    sg_chk_n_print3(leadin, hp, raw_sinfo);
    pthread_mutex_unlock(&strerr_mut);
}


static void
lk_chk_n_print4(const char * leadin, struct sg_io_v4 * h4p, bool raw_sinfo)
{
    pthread_mutex_lock(&strerr_mut);
    sg_linux_sense_print(leadin, h4p->device_status, h4p->transport_status,
                         h4p->driver_status, (const uint8_t *)h4p->response,
                         h4p->response_len, raw_sinfo);
    pthread_mutex_unlock(&strerr_mut);
}

static void
calc_duration_throughput(int contin)
{
    struct timeval end_tm, res_tm;
    double a, b;

    gettimeofday(&end_tm, NULL);
    res_tm.tv_sec = end_tm.tv_sec - start_tm.tv_sec;
    res_tm.tv_usec = end_tm.tv_usec - start_tm.tv_usec;
    if (res_tm.tv_usec < 0) {
        --res_tm.tv_sec;
        res_tm.tv_usec += 1000000;
    }
    a = res_tm.tv_sec;
    a += (0.000001 * res_tm.tv_usec);
    b = (double)gcoll.bs * (dd_count - gcoll.out_rem_count);
    pr2serr("time to transfer data %s %d.%06d secs",
            (contin ? "so far" : "was"), (int)res_tm.tv_sec,
            (int)res_tm.tv_usec);
    if ((a > 0.00001) && (b > 511))
        pr2serr(", %.2f MB/sec\n", b / (a * 1000000.0));
    else
        pr2serr("\n");
}

static void
print_stats(const char * str)
{
    int64_t infull, outfull;

    if (0 != gcoll.out_rem_count)
        pr2serr("  remaining block count=%" PRId64 "\n",
                gcoll.out_rem_count);
    infull = dd_count - gcoll.in_rem_count;
    pr2serr("%s%" PRId64 "+%d records in\n", str,
            infull - gcoll.in_partial, gcoll.in_partial);

    outfull = dd_count - gcoll.out_rem_count;
    pr2serr("%s%" PRId64 "+%d records out\n", str,
            outfull - gcoll.out_partial, gcoll.out_partial);
}

static void
interrupt_handler(int sig)
{
    struct sigaction sigact;

    sigact.sa_handler = SIG_DFL;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(sig, &sigact, NULL);
    pr2serr("Interrupted by signal,");
    if (do_time)
        calc_duration_throughput(0);
    print_stats("");
    kill(getpid (), sig);
}

static void
siginfo_handler(int sig)
{
    if (sig) { ; }      /* unused, dummy to suppress warning */
    pr2serr("Progress report, continuing ...\n");
    if (do_time)
        calc_duration_throughput(1);
    print_stats("  ");
}

static void
siginfo2_handler(int sig)
{
    Gbl_coll * clp = &gcoll;

    if (sig) { ; }      /* unused, dummy to suppress warning */
    pr2serr("Progress report, continuing ...\n");
    if (do_time)
        calc_duration_throughput(1);
    print_stats("  ");
    pr2serr("Send broadcast on out_sync_cv condition variable\n");
    pthread_cond_broadcast(&clp->out_sync_cv);
}

static void
install_handler(int sig_num, void (*sig_handler) (int sig))
{
    struct sigaction sigact;
    sigaction (sig_num, NULL, &sigact);
    if (sigact.sa_handler != SIG_IGN)
    {
        sigact.sa_handler = sig_handler;
        sigemptyset (&sigact.sa_mask);
        sigact.sa_flags = 0;
        sigaction (sig_num, &sigact, NULL);
    }
}

#ifdef SG_LIB_ANDROID
static void
thread_exit_handler(int sig)
{
    pthread_exit(0);
}
#endif

/* Make safe_strerror() thread safe */
static char *
tsafe_strerror(int code, char * ebp)
{
    char * cp;

    pthread_mutex_lock(&strerr_mut);
    cp = safe_strerror(code);
    strncpy(ebp, cp, STRERR_BUFF_LEN);
    pthread_mutex_unlock(&strerr_mut);

    ebp[STRERR_BUFF_LEN - 1] = '\0';
    return ebp;
}


/* Following macro from D.R. Butenhof's POSIX threads book:
 * ISBN 0-201-63392-2 . [Highly recommended book.] Changed __FILE__
 * to __func__ */
#define err_exit(code,text) do { \
    char strerr_buff[STRERR_BUFF_LEN]; \
    pr2serr("%s at \"%s\":%d: %s\n", \
        text, __func__, __LINE__, tsafe_strerror(code, strerr_buff)); \
    exit(1); \
    } while (0)


static int
dd_filetype(const char * filename)
{
    struct stat st;
    size_t len = strlen(filename);

    if ((1 == len) && ('.' == filename[0]))
        return FT_DEV_NULL;
    if (stat(filename, &st) < 0)
        return FT_ERROR;
    if (S_ISCHR(st.st_mode)) {
        if ((MEM_MAJOR == major(st.st_rdev)) &&
            (DEV_NULL_MINOR_NUM == minor(st.st_rdev)))
            return FT_DEV_NULL;
        if (RAW_MAJOR == major(st.st_rdev))
            return FT_RAW;
        if (SCSI_GENERIC_MAJOR == major(st.st_rdev))
            return FT_SG;
        if (SCSI_TAPE_MAJOR == major(st.st_rdev))
            return FT_ST;
    } else if (S_ISBLK(st.st_mode))
        return FT_BLOCK;
    return FT_OTHER;
}

static void
usage(int pg_num)
{
    if (pg_num > 2)
        goto page3;
    else if (pg_num > 1)
        goto page2;

    pr2serr("Usage: sgh_dd  [bs=BS] [count=COUNT] [ibs=BS] [if=IFILE]"
            " [iflag=FLAGS]\n"
            "               [obs=BS] [of=OFILE] [oflag=FLAGS] "
            "[seek=SEEK] [skip=SKIP]\n"
            "               [--help] [--version]\n\n");
    pr2serr("               [ae=AEN] [bpt=BPT] [cdbsz=6|10|12|16] [coe=0|1] "
            "[deb=VERB]\n"
            "               [dio=0|1] [elemsz_kb=ESK] [fua=0|1|2|3] "
            "[of2=OFILE2]\n"
            "               [ofreg=OFREG] [sync=0|1] [thr=THR] [time=0|1] "
            "[verbose=VERB]\n"
            "               [--dry-run] [--verbose]\n\n"
            "  where the main options (shown in first group above) are:\n"
            "    bs          must be device logical block size (default "
            "512)\n"
            "    count       number of blocks to copy (def: device size)\n"
            "    if          file or device to read from (def: stdin)\n"
            "    iflag       comma separated list from: [coe,defres,dio,"
            "direct,dpo,\n"
            "                dsync,excl,fua,mmap,noshare,noxfer,null,"
            "same_fds,v3,v4]\n"
            "    of          file or device to write to (def: /dev/null "
            "N.B. different\n"
            "                from dd it defaults to stdout). If 'of=.' "
            "uses /dev/null\n"
            "    of2         second file or device to write to (def: "
            "/dev/null)\n"
            "    oflag       comma separated list from: [append,coe,dio,"
            "direct,dpo,\n"
            "                dsync,excl,fua,mmap,noshare,noxfer,null,"
            "same_fds,swait,v3,v4]\n"
            "    seek        block position to start writing to OFILE\n"
            "    skip        block position to start reading from IFILE\n"
            "    --help|-h      output this usage message then exit\n"
            "    --version|-V   output version string then exit\n\n"
            "Copy IFILE to OFILE, similar to dd command. This utility is "
            "specialized for\nSCSI devices and uses multiple POSIX threads. "
            "It expects one or both IFILE\nand OFILE to be sg devices. It "
            "is Linux specific and uses the v4 sg driver\n'share' capability "
            "if available. Use '-hh' or '-hhh' for more information.\n"
#ifdef SGH_DD_READ_COMPLET_AFTER
	    "\nIn this version oflag=swait does read completion _after_ "
	    "write completion\n"
#endif
           );
    return;
page2:
    pr2serr("Syntax:  sgh_dd [operands] [options]\n\n"
            "  where: operands have the form name=value and are pecular to "
            "'dd'\n         style commands, and options start with one or "
            "two hyphens\n\n"
            "  where the less used options (not shown on first help page) "
            "are:\n"
            "    ae          abort every n commands (def: 0 --> don't abort "
            "any)\n"
            "    bpt         is blocks_per_transfer (default is 128)\n"
            "    cdbsz       size of SCSI READ or WRITE cdb (default is 10)\n"
            "    coe         continue on error, 0->exit (def), "
            "1->zero + continue\n"
            "    deb         for debug, 0->none (def), > 0->varying degrees "
            "of debug\n"
            "    dio         is direct IO, 1->attempt, 0->indirect IO (def)\n"
            "    elemsz_kb    scatter gather list element size in kilobytes "
            "(def: 32[KB])\n"
            "    fua         force unit access: 0->don't(def), 1->OFILE, "
            "2->IFILE,\n"
            "                3->OFILE+IFILE\n"
            "    ofreg       OFREG is regular file or pipe to send what is "
            "read from\n"
            "                IFILE in the first half of each shared element\n"
            "    sync        0->no sync(def), 1->SYNCHRONIZE CACHE on OFILE "
            "after copy\n"
            "    thr         is number of threads, must be > 0, default 4, "
            "max 16\n"
            "    time        0->no timing, 1->time plus calculate "
            "throughput (def)\n"
            "    verbose     same as 'deb=VERB': increase verbosity\n"
            "    --dry-run|-d    prepare but bypass copy/read\n"
            "    --verbose|-v   increase verbosity of utility\n\n"
            "Use '-hhh' for more information about flags.\n"
           );
    return;
page3:
    pr2serr("Syntax:  sgh_dd [operands] [options]\n\n"
            "  where: iflag=' and 'oflag=' arguments are listed below:\n"
            "    append      append output to OFILE (assumes OFILE is "
            "regular file)\n"
            "    coe         continue of error (reading, fills with zeros)\n"
            "    defres      keep default reserve buffer size (else its "
            "bs*bpt)\n"
            "    dio         sets the SG_FLAG_DIRECT_IO in sg requests\n"
            "    direct      sets the O_DIRECT flag on open()\n"
            "    dpo         sets the DPO (disable page out) in SCSI READs "
            "and WRITEs\n"
            "    dsync       sets the O_SYNC flag on open()\n"
            "    excl        sets the O_EXCL flag on open()\n"
            "    fua         sets the FUA (force unit access) in SCSI READs "
            "and WRITEs\n"
            "    mmap        setup mmap IO on IFILE or OFILE; OFILE only "
            "with noshare\n"
            "    noshare     if IFILE and OFILE are sg devices, don't set "
            "up sharing\n"
            "                (def: do)\n"
            "    same_fds    each thread use the same IFILE and OFILE(2) "
            "file\n"
            "                descriptors (def: each threads has own file "
            "desciptors)\n"
            "    swait       slave wait: issue WRITE on OFILE before READ "
            "is finished;\n"
            "                [oflag only] and IFILE and OFILE must be sg "
            "devices\n"
            "    v3          use v3 sg interface which is the default (also "
            "see v4)\n"
            "    v4          use v4 sg interface (def: v3 unless other side "
            "is v4)\n"
            "\n"
            "Copies IFILE to OFILE (and to OFILE2 if given). If IFILE and "
            "OFILE are sg\ndevices 'shared' mode is selected unless "
            "'noshare' is given to 'iflag=' or\n'oflag='. of2=OFILE2 uses "
            "'oflag=FLAGS'. When sharing, the data stays in a\nsingle "
            "in-kernel buffer which is copied (or mmap-ed) to the user "
            "space\nif the 'ofreg=OFREG' is given.\n"
           );
}

static void
guarded_stop_in(Gbl_coll * clp)
{
    pthread_mutex_lock(&clp->in_mutex);
    clp->in_stop = true;
    pthread_mutex_unlock(&clp->in_mutex);
}

static void
guarded_stop_out(Gbl_coll * clp)
{
    pthread_mutex_lock(&clp->out_mutex);
    clp->out_stop = true;
    pthread_mutex_unlock(&clp->out_mutex);
}

static void
guarded_stop_both(Gbl_coll * clp)
{
    guarded_stop_in(clp);
    guarded_stop_out(clp);
}

/* Return of 0 -> success, see sg_ll_read_capacity*() otherwise */
static int
scsi_read_capacity(int sg_fd, int64_t * num_sect, int * sect_sz)
{
    int res;
    uint8_t rcBuff[RCAP16_REPLY_LEN];

    res = sg_ll_readcap_10(sg_fd, 0, 0, rcBuff, READ_CAP_REPLY_LEN, false, 0);
    if (0 != res)
        return res;

    if ((0xff == rcBuff[0]) && (0xff == rcBuff[1]) && (0xff == rcBuff[2]) &&
        (0xff == rcBuff[3])) {

        res = sg_ll_readcap_16(sg_fd, 0, 0, rcBuff, RCAP16_REPLY_LEN, false,
                               0);
        if (0 != res)
            return res;
        *num_sect = sg_get_unaligned_be64(rcBuff + 0) + 1;
        *sect_sz = sg_get_unaligned_be32(rcBuff + 8);
    } else {
        /* take care not to sign extend values > 0x7fffffff */
        *num_sect = (int64_t)sg_get_unaligned_be32(rcBuff + 0) + 1;
        *sect_sz = sg_get_unaligned_be32(rcBuff + 4);
    }
    return 0;
}

/* Return of 0 -> success, -1 -> failure. BLKGETSIZE64, BLKGETSIZE and */
/* BLKSSZGET macros problematic (from <linux/fs.h> or <sys/mount.h>). */
static int
read_blkdev_capacity(int sg_fd, int64_t * num_sect, int * sect_sz)
{
#ifdef BLKSSZGET
    if ((ioctl(sg_fd, BLKSSZGET, sect_sz) < 0) && (*sect_sz > 0)) {
        perror("BLKSSZGET ioctl error");
        return -1;
    } else {
 #ifdef BLKGETSIZE64
        uint64_t ull;

        if (ioctl(sg_fd, BLKGETSIZE64, &ull) < 0) {

            perror("BLKGETSIZE64 ioctl error");
            return -1;
        }
        *num_sect = ((int64_t)ull / (int64_t)*sect_sz);
 #else
        unsigned long ul;

        if (ioctl(sg_fd, BLKGETSIZE, &ul) < 0) {
            perror("BLKGETSIZE ioctl error");
            return -1;
        }
        *num_sect = (int64_t)ul;
 #endif
    }
    return 0;
#else
    *num_sect = 0;
    *sect_sz = 0;
    return -1;
#endif
}

static void *
sig_listen_thread(void * v_clp)
{
    Gbl_coll * clp = (Gbl_coll *)v_clp;
    int sig_number;

    while (1) {
        sigwait(&signal_set, &sig_number);
        if (shutting_down)
            break;
        if (SIGINT == sig_number) {
            pr2serr_lk("%sinterrupted by SIGINT\n", my_name);
            guarded_stop_both(clp);
            pthread_cond_broadcast(&clp->out_sync_cv);
        }
    }
    return NULL;
}

static bool
sg_share_prepare(int slave_wr_fd, int master_rd_fd, int id, bool vb_b)
{
    struct sg_extended_info sei;
    struct sg_extended_info * seip;

    seip = &sei;
    memset(seip, 0, sizeof(*seip));
    seip->sei_wr_mask |= SG_SEIM_SHARE_FD;
    seip->sei_rd_mask |= SG_SEIM_SHARE_FD;
    seip->share_fd = master_rd_fd;
    if (ioctl(slave_wr_fd, SG_SET_GET_EXTENDED, seip) < 0) {
        pr2serr_lk("tid=%d: ioctl(EXTENDED(shared_fd=%d), failed "
                   "errno=%d %s\n", id, master_rd_fd, errno,
                   strerror(errno));
        return false;
    }
    if (vb_b)
        pr2serr_lk("%s: tid=%d: ioctl(EXTENDED(shared_fd)) ok, master_fd=%d, "
                   "slave_fd=%d\n", __func__, id, master_rd_fd, slave_wr_fd);
    return true;
}

static void
cleanup_in(void * v_clp)
{
    Gbl_coll * clp = (Gbl_coll *)v_clp;

    pr2serr("thread cancelled while in mutex held\n");
    clp->in_stop = true;
    pthread_mutex_unlock(&clp->in_mutex);
    guarded_stop_out(clp);
    pthread_cond_broadcast(&clp->out_sync_cv);
}

static void
cleanup_out(void * v_clp)
{
    Gbl_coll * clp = (Gbl_coll *)v_clp;

    pr2serr("thread cancelled while out_mutex held\n");
    clp->out_stop = true;
    pthread_mutex_unlock(&clp->out_mutex);
    guarded_stop_in(clp);
    pthread_cond_broadcast(&clp->out_sync_cv);
}

static void *
read_write_thread(void * v_tip)
{
    Thread_info * tip;
    Gbl_coll * clp;
    Rq_elem rel;
    Rq_elem * rep = &rel;
    int sz, blocks, status, vb, err, res;
    int num_sg = 0;
    int64_t my_index;
    volatile bool stop_after_write = false;
    bool own_infd = false;
    bool own_outfd = false;
    bool own_out2fd = false;
    bool share_and_ofreg;

    tip = (Thread_info *)v_tip;
    clp = tip->gcp;
    vb = clp->debug;
    sz = clp->bpt * clp->bs;
    memset(rep, 0, sizeof(Rq_elem));
    /* Following clp members are constant during lifetime of thread */
    rep->id = tip->id;
    if (vb > 0)
        pr2serr_lk("Starting worker thread %d\n", rep->id);
    if (! clp->in_flags.mmap) {
        rep->buffp = sg_memalign(sz, 0 /* page align */, &rep->alloc_bp,
                                 false);
        if (NULL == rep->buffp)
            err_exit(ENOMEM, "out of memory creating user buffers\n");
    }

    rep->bs = clp->bs;
    rep->infd = clp->infd;
    rep->outfd = clp->outfd;
    rep->out2fd = clp->out2fd;
    rep->outregfd = clp->outregfd;
    rep->debug = clp->debug;
    rep->cdbsz_in = clp->cdbsz_in;
    rep->cdbsz_out = clp->cdbsz_out;
    rep->in_flags = clp->in_flags;
    rep->out_flags = clp->out_flags;
    rep->aen = clp->aen;
    rep->rep_count = 0;

    if (rep->in_flags.same_fds || rep->out_flags.same_fds) {
        /* we are sharing a single pair of fd_s across all threads */
        if (rep->out_flags.swait && (! swait_reported)) {
            swait_reported = true;
            pr2serr_lk("oflag=swait ignored because same_fds flag given\n");
        }
    } else {
        int fd;

        if ((FT_SG == clp->in_type) && clp->infp) {
            fd = sg_in_open(clp, clp->infp,
                            (rep->in_flags.mmap ? &rep->buffp : NULL),
                            (rep->in_flags.mmap ? &rep->mmap_len : NULL));
            if (fd < 0)
                goto fini;
            rep->infd = fd;
            own_infd = true;
            ++num_sg;
            if (vb > 2)
                pr2serr_lk("thread=%d: opened local sg IFILE\n", rep->id);
        }
        if ((FT_SG == clp->out_type) && clp->outfp) {
            fd = sg_out_open(clp, clp->outfp,
                            (rep->out_flags.mmap ? &rep->buffp : NULL),
                            (rep->out_flags.mmap ? &rep->mmap_len : NULL));
            if (fd < 0)
                goto fini;
            rep->outfd = fd;
            own_outfd = true;
            ++num_sg;
            if (vb > 2)
                pr2serr_lk("thread=%d: opened local sg OFILE\n", rep->id);
        }
        if ((FT_SG == clp->out2_type) && clp->out2fp) {
            fd = sg_out_open(clp, clp->out2fp,
                            (rep->out_flags.mmap ? &rep->buffp : NULL),
                            (rep->out_flags.mmap ? &rep->mmap_len : NULL));
            if (fd < 0)
                goto fini;
            rep->out2fd = fd;
            own_out2fd = true;
            if (vb > 2)
                pr2serr_lk("thread=%d: opened local sg OFILE2\n", rep->id);
        }
        if (rep->out_flags.swait) {
            if (num_sg < 2)
                pr2serr_lk("oflag=swait ignored since need both IFILE and "
                           "OFILE to be sg devices\n");
            else
                rep->swait = true;
        }
    }
    if (vb > 2) {
        if ((FT_SG == clp->in_type) && (! own_infd))
            pr2serr_lk("thread=%d: using global sg IFILE, fd=%d\n", rep->id,
                       rep->infd);
        if ((FT_SG == clp-> out_type) && (! own_outfd))
            pr2serr_lk("thread=%d: using global sg OFILE, fd=%d\n", rep->id,
                       rep->outfd);
        if ((FT_SG == clp->out2_type) && (! own_out2fd))
            pr2serr_lk("thread=%d: using global sg OFILE2, fd=%d\n", rep->id,
                       rep->out2fd);
    }
    if (rep->in_flags.noshare || rep->out_flags.noshare) {
        if (vb)
            pr2serr_lk("thread=%d: Skipping share on both IFILE and OFILE\n",
                       rep->id);
    } else if ((FT_SG == clp->in_type) && (FT_SG == clp->out_type))
        rep->has_share = sg_share_prepare(rep->outfd, rep->infd, rep->id,
                                          rep->debug > 9);
    if (vb > 9)
        pr2serr_lk("tid=%d, has_share=%s\n", rep->id,
                   (rep->has_share ? "true" : "false"));
    share_and_ofreg = (rep->has_share && (rep->outregfd >= 0));

    /* vvvvvvvvvvvvvv  Main segment copy loop  vvvvvvvvvvvvvvvvvvvvvvv */
    while (1) {
        rep->wr = false;
        my_index = atomic_fetch_add(&pos_index, clp->bpt);
        /* Start of READ half of a segment */
        status = pthread_mutex_lock(&clp->in_mutex);
        if (0 != status) err_exit(status, "lock in_mutex");
        if (clp->in_stop || (clp->in_count <= 0)) {
            /* no more to do, exit loop then thread */
            status = pthread_mutex_unlock(&clp->in_mutex);
            if (0 != status) err_exit(status, "unlock in_mutex");
            break;
        }
        if (dd_count >= 0) {
            if (my_index >= dd_count) {
                status = pthread_mutex_unlock(&clp->in_mutex);
                if (0 != status) err_exit(status, "unlock in_mutex");
                break;
            } else if ((my_index + clp->bpt) > dd_count)
                blocks = dd_count - my_index;
            else
                blocks = clp->bpt;
        } else
            blocks = clp->bpt;

        rep->iblk = clp->skip + my_index;
        rep->oblk = clp->seek + my_index;
        rep->num_blks = blocks;

        // clp->in_blk += blocks;
        clp->in_count -= blocks;

        pthread_cleanup_push(cleanup_in, (void *)clp);
        if (FT_SG == clp->in_type) {
            if (rep->swait)
                sg_in_out_interleave(clp, rep);
            else
                sg_in_rd_cmd(clp, rep); /* unlocks in_mutex mid operation */
        } else {
            stop_after_write = normal_in_rd(clp, rep, blocks);
            status = pthread_mutex_unlock(&clp->in_mutex);
            if (0 != status) err_exit(status, "unlock in_mutex");
        }
        pthread_cleanup_pop(0);
        ++rep->rep_count;

        /* Start of WRITE part of a segment */
        rep->wr = true;
        status = pthread_mutex_lock(&clp->out_mutex);
        if (0 != status) err_exit(status, "lock out_mutex");

        /* Make sure the OFILE (+ OFREG) are in same sequence as IFILE */
        if ((rep->outregfd < 0) && (FT_SG == clp->in_type) &&
            (FT_SG == clp->out_type))
            goto skip_force_out_sequence;
        if (share_and_ofreg || (FT_DEV_NULL != clp->out_type)) {
            while ((! clp->out_stop) &&
                   (rep->oblk != clp->out_blk)) {
                /* if write would be out of sequence then wait */
                pthread_cleanup_push(cleanup_out, (void *)clp);
                status = pthread_cond_wait(&clp->out_sync_cv, &clp->out_mutex);
                if (0 != status) err_exit(status, "cond out_sync_cv");
                pthread_cleanup_pop(0);
            }
        }

skip_force_out_sequence:
        if (clp->out_stop || (clp->out_count <= 0)) {
            if (! clp->out_stop)
                clp->out_stop = true;
            status = pthread_mutex_unlock(&clp->out_mutex);
            if (0 != status) err_exit(status, "unlock out_mutex");
            break;
        }
        if (stop_after_write)
            clp->out_stop = true;

        clp->out_blk += blocks;
        clp->out_count -= blocks;

        if (0 == rep->num_blks) {
            clp->out_stop = true;
            stop_after_write = true;
            status = pthread_mutex_unlock(&clp->out_mutex);
            if (0 != status) err_exit(status, "unlock out_mutex");
            break;      /* read nothing so leave loop */
        }

        pthread_cleanup_push(cleanup_out, (void *)clp);
        if (rep->outregfd >= 0) {
            res = write(rep->outregfd, rep->buffp, rep->bs * rep->num_blks);
            err = errno;
            if (res < 0)
                pr2serr_lk("%s: tid=%d: write(outregfd) failed: %s\n",
                           __func__, rep->id, strerror(err));
            else if (rep->debug > 9)
                pr2serr_lk("%s: tid=%d: write(outregfd), fd=%d, num_blks=%d"
                           "\n", __func__, rep->id, rep->outregfd,
                           rep->num_blks);
        }
        /* Output to OFILE */
        if (FT_SG == clp->out_type) {
            if (rep->swait) {   /* done already in sg_in_out_interleave() */
                status = pthread_mutex_unlock(&clp->out_mutex);
                if (0 != status) err_exit(status, "unlock out_mutex");
            } else
                sg_out_wr_cmd(clp, rep, false); /* releases out_mutex */
        } else if (FT_DEV_NULL == clp->out_type) {
            /* skip actual write operation */
            clp->out_rem_count -= blocks;
            status = pthread_mutex_unlock(&clp->out_mutex);
            if (0 != status) err_exit(status, "unlock out_mutex");
            --rep->rep_count;
        } else {
            normal_out_wr(clp, rep, blocks);
            status = pthread_mutex_unlock(&clp->out_mutex);
            if (0 != status) err_exit(status, "unlock out_mutex");
        }
        ++rep->rep_count;
        pthread_cleanup_pop(0);

        /* Output to OFILE2 if sg device */
        if ((clp->out2fd >= 0) && (FT_SG == clp->out2_type)) {
            pthread_cleanup_push(cleanup_out, (void *)clp);
            status = pthread_mutex_lock(&clp->out2_mutex);
            if (0 != status) err_exit(status, "lock out2_mutex");
            sg_out_wr_cmd(clp, rep, true); /* releases out2_mutex mid oper */

            pthread_cleanup_pop(0);
        }
        // if ((! rep->has_share) && (FT_DEV_NULL != clp->out_type))
        pthread_cond_broadcast(&clp->out_sync_cv);
        if (stop_after_write)
            break;
    } /* end of while loop which copies segments */

    status = pthread_mutex_lock(&clp->in_mutex);
    if (0 != status) err_exit(status, "lock in_mutex");
    if (! clp->in_stop)
        clp->in_stop = true;  /* flag other workers to stop */
    status = pthread_mutex_unlock(&clp->in_mutex);
    if (0 != status) err_exit(status, "unlock in_mutex");

fini:
    if (rep->mmap_len > 0) {
        if (munmap(rep->buffp, rep->mmap_len) < 0) {
            int err = errno;
            char bb[64];

            pr2serr_lk("thread=%d: munmap() failed: %s\n", rep->id,
                       tsafe_strerror(err, bb));
        }

    } else if (rep->alloc_bp)
        free(rep->alloc_bp);
    if (own_infd && (rep->infd >= 0))
        close(rep->infd);
    if (own_outfd && (rep->outfd >= 0))
        close(rep->outfd);
    if (own_out2fd && (rep->out2fd >= 0))
        close(rep->out2fd);
    pthread_cond_broadcast(&clp->out_sync_cv);
    return stop_after_write ? NULL : clp;
}

static bool
normal_in_rd(Gbl_coll * clp, Rq_elem * rep, int blocks)
{
    bool stop_after_write = false;
    bool same_fds = rep->in_flags.same_fds || rep->out_flags.same_fds;
    int res;
    char strerr_buff[STRERR_BUFF_LEN];

    if (! same_fds) {   /* each has own file pointer, so we need to move it */
        int64_t pos = rep->iblk * clp->bs;

        if (lseek64(rep->infd, pos, SEEK_SET) < 0) {    /* problem if pipe! */
            pr2serr_lk("%s: tid=%d: >> lseek64(%" PRId64 "): %s\n", __func__,
                       rep->id, pos, safe_strerror(errno));
            clp->in_stop = true;
            guarded_stop_out(clp);
            return true;
        }
    }
    /* enters holding in_mutex */
    while (((res = read(clp->infd, rep->buffp, blocks * clp->bs)) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno)))
        sched_yield();  /* another thread may be able to progress */
    if (res < 0) {
        if (clp->in_flags.coe) {
            memset(rep->buffp, 0, rep->num_blks * rep->bs);
            pr2serr_lk("tid=%d: >> substituted zeros for in blk=%" PRId64
                       " for %d bytes, %s\n", rep->id, rep->iblk,
                       rep->num_blks * rep->bs,
                       tsafe_strerror(errno, strerr_buff));
            res = rep->num_blks * clp->bs;
        }
        else {
            pr2serr_lk("tid=%d: error in normal read, %s\n", rep->id,
                       tsafe_strerror(errno, strerr_buff));
            clp->in_stop = true;
            guarded_stop_out(clp);
            return true;
        }
    }
    if (res < blocks * clp->bs) {
        int o_blocks = blocks;
        stop_after_write = true;
        blocks = res / clp->bs;
        if ((res % clp->bs) > 0) {
            blocks++;
            clp->in_partial++;
        }
        /* Reverse out + re-apply blocks on clp */
        // clp->in_blk -= o_blocks;
        clp->in_count += o_blocks;
        rep->num_blks = blocks;
        // clp->in_blk += blocks;
        clp->in_count -= blocks;
    }
    clp->in_rem_count -= blocks;
    return stop_after_write;
}

static void
normal_out_wr(Gbl_coll * clp, Rq_elem * rep, int blocks)
{
    int res;
    char strerr_buff[STRERR_BUFF_LEN];

    /* enters holding out_mutex */
    while (((res = write(clp->outfd, rep->buffp, rep->num_blks * clp->bs))
            < 0) && ((EINTR == errno) || (EAGAIN == errno)))
        sched_yield();  /* another thread may be able to progress */
    if (res < 0) {
        if (clp->out_flags.coe) {
            pr2serr_lk("tid=%d: >> ignored error for out blk=%" PRId64
                       " for %d bytes, %s\n", rep->id, rep->oblk,
                       rep->num_blks * rep->bs,
                       tsafe_strerror(errno, strerr_buff));
            res = rep->num_blks * clp->bs;
        }
        else {
            pr2serr_lk("tid=%d: error normal write, %s\n", rep->id,
                       tsafe_strerror(errno, strerr_buff));
            guarded_stop_in(clp);
            clp->out_stop = true;
            return;
        }
    }
    if (res < blocks * clp->bs) {
        blocks = res / clp->bs;
        if ((res % clp->bs) > 0) {
            blocks++;
            clp->out_partial++;
        }
        rep->num_blks = blocks;
    }
    clp->out_rem_count -= blocks;
}

static int
sg_build_scsi_cdb(uint8_t * cdbp, int cdb_sz, unsigned int blocks,
                  int64_t start_block, bool write_true, bool fua, bool dpo)
{
    int rd_opcode[] = {0x8, 0x28, 0xa8, 0x88};
    int wr_opcode[] = {0xa, 0x2a, 0xaa, 0x8a};
    int sz_ind;

    memset(cdbp, 0, cdb_sz);
    if (dpo)
        cdbp[1] |= 0x10;
    if (fua)
        cdbp[1] |= 0x8;
    switch (cdb_sz) {
    case 6:
        sz_ind = 0;
        cdbp[0] = (uint8_t)(write_true ? wr_opcode[sz_ind] :
                                               rd_opcode[sz_ind]);
        sg_put_unaligned_be24(0x1fffff & start_block, cdbp + 1);
        cdbp[4] = (256 == blocks) ? 0 : (uint8_t)blocks;
        if (blocks > 256) {
            pr2serr_lk("%sfor 6 byte commands, maximum number of blocks is "
                       "256\n", my_name);
            return 1;
        }
        if ((start_block + blocks - 1) & (~0x1fffff)) {
            pr2serr_lk("%sfor 6 byte commands, can't address blocks beyond "
                       "%d\n", my_name, 0x1fffff);
            return 1;
        }
        if (dpo || fua) {
            pr2serr_lk("%sfor 6 byte commands, neither dpo nor fua bits "
                       "supported\n", my_name);
            return 1;
        }
        break;
    case 10:
        sz_ind = 1;
        cdbp[0] = (uint8_t)(write_true ? wr_opcode[sz_ind] :
                                               rd_opcode[sz_ind]);
        sg_put_unaligned_be32((uint32_t)start_block, cdbp + 2);
        sg_put_unaligned_be16((uint16_t)blocks, cdbp + 7);
        if (blocks & (~0xffff)) {
            pr2serr_lk("%sfor 10 byte commands, maximum number of blocks is "
                       "%d\n", my_name, 0xffff);
            return 1;
        }
        break;
    case 12:
        sz_ind = 2;
        cdbp[0] = (uint8_t)(write_true ? wr_opcode[sz_ind] :
                                               rd_opcode[sz_ind]);
        sg_put_unaligned_be32((uint32_t)start_block, cdbp + 2);
        sg_put_unaligned_be32((uint32_t)blocks, cdbp + 6);
        break;
    case 16:
        sz_ind = 3;
        cdbp[0] = (uint8_t)(write_true ? wr_opcode[sz_ind] :
                                               rd_opcode[sz_ind]);
        sg_put_unaligned_be64((uint64_t)start_block, cdbp + 2);
        sg_put_unaligned_be32((uint32_t)blocks, cdbp + 10);
        break;
    default:
        pr2serr_lk("%sexpected cdb size of 6, 10, 12, or 16 but got %d\n",
                   my_name, cdb_sz);
        return 1;
    }
    return 0;
}

/* Enters this function holding in_mutex */
static void
sg_in_rd_cmd(Gbl_coll * clp, Rq_elem * rep)
{
    int res;
    int status;

    while (1) {
        res = sg_start_io(rep, false);
        if (1 == res)
            err_exit(ENOMEM, "sg starting in command");
        else if (res < 0) {
            pr2serr_lk("tid=%d: inputting to sg failed, blk=%" PRId64 "\n",
                       rep->id, rep->iblk);
            status = pthread_mutex_unlock(&clp->in_mutex);
            if (0 != status) err_exit(status, "unlock in_mutex");
            guarded_stop_both(clp);
            return;
        }
        /* Now release in mutex to let other reads run in parallel */
        status = pthread_mutex_unlock(&clp->in_mutex);
        if (0 != status) err_exit(status, "unlock in_mutex");

        res = sg_finish_io(rep->wr, rep, false);
        switch (res) {
        case SG_LIB_CAT_ABORTED_COMMAND:
        case SG_LIB_CAT_UNIT_ATTENTION:
            /* try again with same addr, count info */
            /* now re-acquire in mutex for balance */
            /* N.B. This re-read could now be out of read sequence */
            status = pthread_mutex_lock(&clp->in_mutex);
            if (0 != status) err_exit(status, "lock in_mutex");
            break;      /* will loop again */
        case SG_LIB_CAT_MEDIUM_HARD:
            if (0 == clp->in_flags.coe) {
                pr2serr_lk("error finishing sg in command (medium)\n");
                if (exit_status <= 0)
                    exit_status = res;
                guarded_stop_both(clp);
                return;
            } else {
                memset(rep->buffp, 0, rep->num_blks * rep->bs);
                pr2serr_lk("tid=%d: >> substituted zeros for in blk=%" PRId64
                           " for %d bytes\n", rep->id, rep->iblk,
                           rep->num_blks * rep->bs);
            }
#if defined(__GNUC__)
#if (__GNUC__ >= 7)
            __attribute__((fallthrough));
            /* FALL THROUGH */
#endif
#endif
        case 0:
            status = pthread_mutex_lock(&clp->in_mutex);
            if (0 != status) err_exit(status, "lock in_mutex");
            if (rep->dio_incomplete_count || rep->resid) {
                clp->dio_incomplete_count += rep->dio_incomplete_count;
                clp->sum_of_resids += rep->resid;
            }
            clp->in_rem_count -= rep->num_blks;
            status = pthread_mutex_unlock(&clp->in_mutex);
            if (0 != status) err_exit(status, "unlock in_mutex");
            return;
        default:
            pr2serr_lk("tid=%d: error finishing sg in command (%d)\n",
                       rep->id, res);
            if (exit_status <= 0)
                exit_status = res;
            guarded_stop_both(clp);
            return;
        }
    }           /* end of while (1) loop */
}

static bool
sg_wr_swap_share(Rq_elem * rep, int to_fd, bool before)
{
    bool not_first = false;
    int err = 0;
    int master_fd = rep->infd;  /* in (READ) side is master */
    struct sg_extended_info sei;
    struct sg_extended_info * seip;

    seip = &sei;
    memset(seip, 0, sizeof(*seip));
    seip->sei_wr_mask |= SG_SEIM_CHG_SHARE_FD;
    seip->sei_rd_mask |= SG_SEIM_CHG_SHARE_FD;
    seip->share_fd = to_fd;
    if (before) {
        /* clear MASTER_FINI bit to put master in SG_RQ_SHR_SWAP state */
        seip->sei_wr_mask |= SG_SEIM_CTL_FLAGS;
        seip->sei_rd_mask |= SG_SEIM_CTL_FLAGS;
        seip->ctl_flags_wr_mask |= SG_CTL_FLAGM_MASTER_FINI;
        seip->ctl_flags &= SG_CTL_FLAGM_MASTER_FINI;/* would be 0 anyway */
    }
    while ((ioctl(master_fd, SG_SET_GET_EXTENDED, seip) < 0) &&
           (EBUSY == errno)) {
        err = errno;
        if (! not_first) {
            if (rep->debug > 9)
                pr2serr_lk("tid=%d: ioctl(EXTENDED(change_shared_fd=%d), "
                           "failed errno=%d %s\n", rep->id, master_fd, err,
                           strerror(err));
            not_first = true;
        }
        err = 0;
        sched_yield();  /* another thread may be able to progress */
    }
    if (err) {
        pr2serr_lk("tid=%d: ioctl(EXTENDED(change_shared_fd=%d), failed "
                   "errno=%d %s\n", rep->id, master_fd, err, strerror(err));
        return false;
    }
    if (rep->debug > 15)
        pr2serr_lk("%s: tid=%d: ioctl(EXTENDED(change_shared_fd)) ok, "
                   "master_fd=%d, to_slave_fd=%d\n", __func__, rep->id,
                   master_fd, to_fd);
    return true;
}

/* Enters this function holding out_mutex */
static void
sg_out_wr_cmd(Gbl_coll * clp, Rq_elem * rep, bool is_wr2)
{
    int res;
    int status;
    pthread_mutex_t * mutexp = is_wr2 ? &clp->out2_mutex : &clp->out_mutex;

    if (rep->has_share && is_wr2)
        sg_wr_swap_share(rep, rep->out2fd, true);

    while (1) {
        res = sg_start_io(rep, is_wr2);
        if (1 == res)
            err_exit(ENOMEM, "sg starting out command");
        else if (res < 0) {
            pr2serr_lk("%soutputting from sg failed, blk=%" PRId64 "\n",
                       my_name, rep->oblk);
            status = pthread_mutex_unlock(mutexp);
            if (0 != status) err_exit(status, "unlock out_mutex");
            guarded_stop_both(clp);
            goto fini;
        }
        /* Now release in mutex to let other reads run in parallel */
        status = pthread_mutex_unlock(mutexp);
        if (0 != status) err_exit(status, "unlock out_mutex");

        res = sg_finish_io(rep->wr, rep, is_wr2);
        switch (res) {
        case SG_LIB_CAT_ABORTED_COMMAND:
        case SG_LIB_CAT_UNIT_ATTENTION:
            /* try again with same addr, count info */
            /* now re-acquire out mutex for balance */
            /* N.B. This re-write could now be out of write sequence */
            status = pthread_mutex_lock(mutexp);
            if (0 != status) err_exit(status, "lock out_mutex");
            break;      /* loops around */
        case SG_LIB_CAT_MEDIUM_HARD:
            if (0 == clp->out_flags.coe) {
                pr2serr_lk("error finishing sg out command (medium)\n");
                if (exit_status <= 0)
                    exit_status = res;
                guarded_stop_both(clp);
                goto fini;
            } else
                pr2serr_lk(">> ignored error for out blk=%" PRId64 " for %d "
                           "bytes\n", rep->oblk, rep->num_blks * rep->bs);
#if defined(__GNUC__)
#if (__GNUC__ >= 7)
            __attribute__((fallthrough));
            /* FALL THROUGH */
#endif
#endif
        case 0:
            if (! is_wr2) {
                status = pthread_mutex_lock(mutexp);
                if (0 != status) err_exit(status, "lock out_mutex");
                if (rep->dio_incomplete_count || rep->resid) {
                    clp->dio_incomplete_count += rep->dio_incomplete_count;
                    clp->sum_of_resids += rep->resid;
                }
                clp->out_rem_count -= rep->num_blks;
                status = pthread_mutex_unlock(mutexp);
                if (0 != status) err_exit(status, "unlock out_mutex");
            }
            goto fini;
        default:
            pr2serr_lk("error finishing sg out command (%d)\n", res);
            if (exit_status <= 0)
                exit_status = res;
            guarded_stop_both(clp);
            goto fini;
        }
    }           /* end of while (1) loop */
fini:
    if (rep->has_share && is_wr2)
        sg_wr_swap_share(rep, rep->outfd, false);
}

/* Returns 0 on success, 1 if ENOMEM error else -1 for other errors. */
static int
sg_start_io(Rq_elem * rep, bool is_wr2)
{
    bool wr = rep->wr;
    bool fua = wr ? rep->out_flags.fua : rep->in_flags.fua;
    bool dpo = wr ? rep->out_flags.dpo : rep->in_flags.dpo;
    bool dio = wr ? rep->out_flags.dio : rep->in_flags.dio;
    bool mmap = wr ? rep->out_flags.mmap : rep->in_flags.mmap;
    bool noxfer = wr ? rep->out_flags.noxfer : rep->in_flags.noxfer;
    bool v4 = wr ? rep->out_flags.v4 : rep->in_flags.v4;
    int cdbsz = wr ? rep->cdbsz_out : rep->cdbsz_in;
    int flags = 0;
    int res, err, fd;
    int64_t blk = wr ? rep->oblk : rep->iblk;
    struct sg_io_hdr * hp = &rep->io_hdr;
    struct sg_io_v4 * h4p = &rep->io_hdr4;
    const char * cp = "";
    const char * c2p = "";
    const char * c3p = "";
    const char * crwp;

    if (wr) {
        fd = is_wr2 ? rep->out2fd : rep->outfd;
        crwp = is_wr2 ? "writing2" : "writing";
    } else {
        fd = rep->infd;
        crwp = "reading";
    }
    if (sg_build_scsi_cdb(rep->cmd, cdbsz, rep->num_blks, blk, wr, fua,
                          dpo)) {
        pr2serr_lk("%sbad cdb build, start_blk=%" PRId64 ", blocks=%d\n",
                   my_name, blk, rep->num_blks);
        return -1;
    }
    if (mmap && (rep->outregfd >= 0)) {
        flags |= SG_FLAG_MMAP_IO;
        c3p = " mmap";
    }
    if (noxfer)
        flags |= SG_FLAG_NO_DXFER;
    if (dio)
        flags |= SG_FLAG_DIRECT_IO;
    if (rep->has_share) {
        flags |= SGV4_FLAG_SHARE;
        if (wr)
            flags |= SGV4_FLAG_NO_DXFER;
        else if (rep->outregfd < 0)
            flags |= SGV4_FLAG_NO_DXFER;
        if (flags & SGV4_FLAG_NO_DXFER)
            c2p = " and FLAG_NO_DXFER";

        cp = (wr ? " slave active" : " master active");
    } else
        cp = (wr ? " slave not sharing" : " master not sharing");
    rep->rq_id = atomic_fetch_add(&mono_pack_id, 1);    /* fetch before */
    if (rep->debug > 3) {
        pr2serr_lk("%s tid,rq_id=%d,%d: SCSI %s%s%s%s, blk=%" PRId64
                   " num_blks=%d\n", __func__, rep->id, rep->rq_id, crwp, cp,
                   c2p, c3p, blk, rep->num_blks);
        lk_print_command(rep->cmd);
    }
    if (v4)
        goto do_v4;

    memset(hp, 0, sizeof(struct sg_io_hdr));
    hp->interface_id = 'S';
    hp->cmd_len = cdbsz;
    hp->cmdp = rep->cmd;
    hp->dxfer_direction = wr ? SG_DXFER_TO_DEV : SG_DXFER_FROM_DEV;
    hp->dxfer_len = rep->bs * rep->num_blks;
    hp->dxferp = rep->buffp;
    hp->mx_sb_len = sizeof(rep->sb);
    hp->sbp = rep->sb;
    hp->timeout = DEF_TIMEOUT;
    hp->usr_ptr = rep;
    hp->pack_id = rep->rq_id;
    hp->flags = flags;

    while (((res = write(fd, hp, sizeof(struct sg_io_hdr))) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno)))
        sched_yield();  /* another thread may be able to progress */
    err = errno;
    if (res < 0) {
        if (ENOMEM == err)
            return 1;
        pr2serr_lk("%s tid=%d: %s%s%s write(2) failed: %s\n", __func__,
                   rep->id, cp, c2p, c3p, strerror(err));
        return -1;
    }
    return 0;
do_v4:
    memset(h4p, 0, sizeof(struct sg_io_v4));
    h4p->guard = 'Q';
    h4p->request_len = cdbsz;
    h4p->request = (uint64_t)rep->cmd;
    if (wr) {
        h4p->dout_xfer_len = rep->bs * rep->num_blks;
        h4p->dout_xferp = (uint64_t)rep->buffp;
    } else if (rep->num_blks > 0) {
        h4p->din_xfer_len = rep->bs * rep->num_blks;
        h4p->din_xferp = (uint64_t)rep->buffp;
    }
    h4p->max_response_len = sizeof(rep->sb);
    h4p->response = (uint64_t)rep->sb;
    h4p->timeout = DEF_TIMEOUT;
    h4p->usr_ptr = (uint64_t)rep;
    h4p->request_extra = rep->rq_id;    /* this is the pack_id */
    h4p->flags = flags;
    while (((res = ioctl(fd, SG_IOSUBMIT, h4p)) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno)))
        sched_yield();  /* another thread may be able to progress */
    err = errno;
    if (res < 0) {
        if (ENOMEM == err)
            return 1;
        pr2serr_lk("%s tid=%d: %s%s%s ioctl(2) failed: %s\n", __func__,
                   rep->id, cp, c2p, c3p, strerror(err));
        return -1;
    }
    if ((rep->aen > 0) && (rep->rep_count > 0)) {
        if (0 == (rep->rq_id % rep->aen)) {
            struct pollfd a_poll;

            a_poll.fd = fd;
            a_poll.events = POLL_IN;
            a_poll.revents = 0;
            res = poll(&a_poll, 1 /* element */, 1 /* millisecond */);
            if (res < 0)
                pr2serr_lk("%s: poll() failed: %s [%d]\n",
                           __func__, safe_strerror(errno), errno);
            else if (0 == res) { /* timeout, cmd still inflight, so abort */
                res = ioctl(fd, SG_IOABORT, h4p);
                if (res < 0)
                    pr2serr_lk("%s: ioctl(SG_IOABORT) failed: %s [%d]\n",
                               __func__, safe_strerror(errno), errno);
                else if (rep->debug > 3)
                    pr2serr_lk("%s: sending ioctl(SG_IOABORT) on rq_id=%d\n",
                               __func__, rep->rq_id);
            }   /* else got response, too late for timeout, so skip */
        }
    }
    return 0;
}

/* 0 -> successful, SG_LIB_CAT_UNIT_ATTENTION or SG_LIB_CAT_ABORTED_COMMAND
   -> try again, SG_LIB_CAT_NOT_READY, SG_LIB_CAT_MEDIUM_HARD,
   -1 other errors */
static int
sg_finish_io(bool wr, Rq_elem * rep, bool is_wr2)
{
    bool v4 = wr ? rep->out_flags.v4 : rep->in_flags.v4;
    int res, fd;
    int64_t blk = wr ? rep->oblk : rep->iblk;
    struct sg_io_hdr io_hdr;
    struct sg_io_hdr * hp;
    struct sg_io_v4 * h4p;
    const char *cp;
#if 0
    static int testing = 0;     /* thread dubious! */
#endif

    if (wr) {
        fd = is_wr2 ? rep->out2fd : rep->outfd;
        cp = is_wr2 ? "writing2" : "writing";
    } else {
        fd = rep->infd;
        cp = "reading";
    }
    if (v4)
        goto do_v4;
    memset(&io_hdr, 0 , sizeof(struct sg_io_hdr));
    /* FORCE_PACK_ID active set only read packet with matching pack_id */
    io_hdr.interface_id = 'S';
    io_hdr.dxfer_direction = wr ? SG_DXFER_TO_DEV : SG_DXFER_FROM_DEV;
    io_hdr.pack_id = rep->rq_id;

    while (((res = read(fd, &io_hdr, sizeof(struct sg_io_hdr))) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno)))
        sched_yield();  /* another thread may be able to progress */
    if (res < 0) {
        perror("finishing io [read(2)] on sg device, error");
        return -1;
    }
    if (rep != (Rq_elem *)io_hdr.usr_ptr)
        err_exit(0, "sg_finish_io: bad usr_ptr, request-response mismatch\n");
    memcpy(&rep->io_hdr, &io_hdr, sizeof(struct sg_io_hdr));
    hp = &rep->io_hdr;

    res = sg_err_category3(hp);
    switch (res) {
    case SG_LIB_CAT_CLEAN:
        break;
    case SG_LIB_CAT_RECOVERED:
        lk_chk_n_print3(cp, hp, false);
        break;
    case SG_LIB_CAT_ABORTED_COMMAND:
    case SG_LIB_CAT_UNIT_ATTENTION:
        if (rep->debug > 3)
            lk_chk_n_print3(cp, hp, false);
        return res;
    case SG_LIB_CAT_NOT_READY:
    default:
        {
            char ebuff[EBUFF_SZ];

            snprintf(ebuff, EBUFF_SZ, "%s blk=%" PRId64, cp, blk);
            lk_chk_n_print3(ebuff, hp, false);
            return res;
        }
    }
#if 0
    if (0 == (++testing % 100)) return -1;
#endif
    if ((wr ? rep->out_flags.dio : rep->in_flags.dio) &&
        ((hp->info & SG_INFO_DIRECT_IO_MASK) != SG_INFO_DIRECT_IO))
        rep->dio_incomplete_count = 1; /* count dios done as indirect IO */
    else
        rep->dio_incomplete_count = 0;
    rep->resid = hp->resid;
    if (rep->debug > 3)
        pr2serr_lk("%s: tid=%d: completed %s\n", __func__, rep->id, cp);
    return 0;

do_v4:
    h4p = &rep->io_hdr4;
    while (((res = ioctl(fd, SG_IORECEIVE, h4p)) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno)))
        sched_yield();  /* another thread may be able to progress */
    if (res < 0) {
        perror("finishing io [SG_IORECEIVE] on sg device, error");
        return -1;
    }
    if (rep != (Rq_elem *)h4p->usr_ptr)
        err_exit(0, "sg_finish_io: bad usr_ptr, request-response mismatch\n");
    res = sg_err_category_new(h4p->device_status, h4p->transport_status,
                              h4p->driver_status,
                              (const uint8_t *)h4p->response,
                              h4p->response_len);
    switch (res) {
    case SG_LIB_CAT_CLEAN:
        break;
    case SG_LIB_CAT_RECOVERED:
        lk_chk_n_print4(cp, h4p, false);
        break;
    case SG_LIB_CAT_ABORTED_COMMAND:
    case SG_LIB_CAT_UNIT_ATTENTION:
        if (rep->debug > 3)
            lk_chk_n_print4(cp, h4p, false);
        return res;
    case SG_LIB_CAT_NOT_READY:
    default:
        {
            char ebuff[EBUFF_SZ];

            snprintf(ebuff, EBUFF_SZ, "%s rq_id=%d, blk=%" PRId64, cp,
                     rep->rq_id, blk);
            lk_chk_n_print4(ebuff, h4p, false);
            if ((rep->debug > 4) && h4p->info)
                pr2serr_lk(" info=0x%x sg_info_check=%d another_waiting=%d "
                           "direct=%d detaching=%d aborted=%d\n", h4p->info,
                           !!(h4p->info & SG_INFO_CHECK),
                           !!(h4p->info & SG_INFO_ANOTHER_WAITING),
                           !!(h4p->info & SG_INFO_DIRECT_IO),
                           !!(h4p->info & SG_INFO_DEVICE_DETACHING),
                           !!(h4p->info & SG_INFO_ABORTED));
            return res;
        }
    }
#if 0
    if (0 == (++testing % 100)) return -1;
#endif
    if ((wr ? rep->out_flags.dio : rep->in_flags.dio) &&
        (h4p->info & SG_INFO_DIRECT_IO))
        rep->dio_incomplete_count = 1; /* count dios done as indirect IO */
    else
        rep->dio_incomplete_count = 0;
    rep->resid = h4p->din_resid;
    if (rep->debug > 3) {
        pr2serr_lk("%s: tid,rq_id=%d,%d: completed %s\n", __func__, rep->id,
                   rep->rq_id, cp);
        if ((rep->debug > 4) && h4p->info)
            pr2serr_lk(" info=0x%x sg_info_check=%d another_waiting=%d "
                       "direct=%d detaching=%d aborted=%d\n", h4p->info,
                       !!(h4p->info & SG_INFO_CHECK),
                       !!(h4p->info & SG_INFO_ANOTHER_WAITING),
                       !!(h4p->info & SG_INFO_DIRECT_IO),
                       !!(h4p->info & SG_INFO_DEVICE_DETACHING),
                       !!(h4p->info & SG_INFO_ABORTED));
    }
    return 0;
}

/* Enter holding in_mutex, exits holding nothing */
static void
sg_in_out_interleave(Gbl_coll *clp, Rq_elem * rep)
{
    int res, pid_read, pid_write;
    int status;

    while (1) {
        /* start READ */
        res = sg_start_io(rep, false);
        pid_read = rep->rq_id;
        if (1 == res)
            err_exit(ENOMEM, "sg interleave starting in command");
        else if (res < 0) {
            pr2serr_lk("tid=%d: inputting to sg failed, blk=%" PRId64 "\n",
                       rep->id, rep->iblk);
            status = pthread_mutex_unlock(&clp->in_mutex);
            if (0 != status) err_exit(status, "unlock in_mutex");
            guarded_stop_both(clp);
            return;
        }

        /* start WRITE */
        rep->wr = true;
        res = sg_start_io(rep, false);
        pid_write = rep->rq_id;
        if (1 == res)
            err_exit(ENOMEM, "sg interleave starting out command");
        else if (res < 0) {
            pr2serr_lk("tid=%d: outputting to sg failed, blk=%" PRId64 "\n",
                       rep->id, rep->oblk);
            status = pthread_mutex_unlock(&clp->in_mutex);
            if (0 != status) err_exit(status, "unlock in_mutex");
            guarded_stop_both(clp);
            return;
        }
        /* Now release in mutex to let other reads run in parallel */
        status = pthread_mutex_unlock(&clp->in_mutex);
        if (0 != status) err_exit(status, "unlock in_mutex");

#ifdef SGH_DD_READ_COMPLET_AFTER
#warning "SGH_DD_READ_COMPLET_AFTER is set (testing)"
	goto write_complet;
read_complet:
#endif

        /* finish READ */
        rep->rq_id = pid_read;
        rep->wr = false;
        res = sg_finish_io(rep->wr, rep, false);
        switch (res) {
        case SG_LIB_CAT_ABORTED_COMMAND:
        case SG_LIB_CAT_UNIT_ATTENTION:
            /* try again with same addr, count info */
            /* now re-acquire in mutex for balance */
            /* N.B. This re-read could now be out of read sequence */
            status = pthread_mutex_lock(&clp->in_mutex);
            if (0 != status) err_exit(status, "lock in_mutex");
            break;      /* will loop again */
        case SG_LIB_CAT_MEDIUM_HARD:
            if (0 == clp->in_flags.coe) {
                pr2serr_lk("%s: finishing in (medium)\n", __func__);
                if (exit_status <= 0)
                    exit_status = res;
                guarded_stop_both(clp);
                // return;
                break;
            } else {
                memset(rep->buffp, 0, rep->num_blks * rep->bs);
                pr2serr_lk("tid=%d: >> substituted zeros for in blk=%" PRId64
                           " for %d bytes\n", rep->id, rep->iblk,
                           rep->num_blks * rep->bs);
            }
#if defined(__GNUC__)
#if (__GNUC__ >= 7)
            __attribute__((fallthrough));
            /* FALL THROUGH */
#endif
#endif
        case 0:
            status = pthread_mutex_lock(&clp->in_mutex);
            if (0 != status) err_exit(status, "lock in_mutex");
            if (rep->dio_incomplete_count || rep->resid) {
                clp->dio_incomplete_count += rep->dio_incomplete_count;
                clp->sum_of_resids += rep->resid;
            }
            clp->in_rem_count -= rep->num_blks;
            status = pthread_mutex_unlock(&clp->in_mutex);
            if (0 != status) err_exit(status, "unlock in_mutex");
            // return;
            break;
        default:
            pr2serr_lk("%s: tid=%d: error finishing in (%d)\n", __func__,
                       rep->id, res);
            if (exit_status <= 0)
                exit_status = res;
            guarded_stop_both(clp);
            // return;
            break;
        }


#ifdef SGH_DD_READ_COMPLET_AFTER
	return;

write_complet:
#endif
        /* finish WRITE, no lock held */
        rep->rq_id = pid_write;
        rep->wr = true;
        res = sg_finish_io(rep->wr, rep, false);
        switch (res) {
        case SG_LIB_CAT_ABORTED_COMMAND:
        case SG_LIB_CAT_UNIT_ATTENTION:
            /* try again with same addr, count info */
            /* now re-acquire in mutex for balance */
            /* N.B. This re-write could now be out of write sequence */
            status = pthread_mutex_lock(&clp->in_mutex);
            if (0 != status) err_exit(status, "lock in_mutex");
            break;      /* loops around */
        case SG_LIB_CAT_MEDIUM_HARD:
            if (0 == clp->out_flags.coe) {
                pr2serr_lk("error finishing sg out command (medium)\n");
                if (exit_status <= 0)
                    exit_status = res;
                guarded_stop_both(clp);
                return;
            } else
                pr2serr_lk(">> ignored error for out blk=%" PRId64 " for %d "
                           "bytes\n", rep->oblk, rep->num_blks * rep->bs);
#if defined(__GNUC__)
#if (__GNUC__ >= 7)
            __attribute__((fallthrough));
            /* FALL THROUGH */
#endif
#endif
        case 0:
            status = pthread_mutex_lock(&clp->in_mutex);
            if (0 != status) err_exit(status, "lock in_mutex");
            if (rep->dio_incomplete_count || rep->resid) {
                clp->dio_incomplete_count += rep->dio_incomplete_count;
                clp->sum_of_resids += rep->resid;
            }
            clp->out_rem_count -= rep->num_blks;
            status = pthread_mutex_unlock(&clp->in_mutex);
            if (0 != status) err_exit(status, "unlock out_mutex");

#ifdef SGH_DD_READ_COMPLET_AFTER
	goto read_complet;
#endif
            return;
        default:
            pr2serr_lk("error finishing sg out command (%d)\n", res);
            if (exit_status <= 0)
                exit_status = res;
            guarded_stop_both(clp);
            return;
        }
    }           /* end of while (1) loop */
}

/* Returns reserved_buffer_size/mmap_size if success, else 0 for failure */
static int
sg_prepare_resbuf(int fd, int bs, int bpt, bool def_res, int elem_sz,
                  uint8_t **mmpp)
{
    int res, t, num;
    uint8_t *mmp;

    res = ioctl(fd, SG_GET_VERSION_NUM, &t);
    if ((res < 0) || (t < 40000)) {
        pr2serr_lk("%ssg driver prior to 4.0.00\n", my_name);
        return 0;
    }
    if (elem_sz >= 4096) {
        struct sg_extended_info sei;
        struct sg_extended_info * seip;

        seip = &sei;
        memset(seip, 0, sizeof(*seip));
        seip->sei_rd_mask |= SG_SEIM_SGAT_ELEM_SZ;
        res = ioctl(fd, SG_SET_GET_EXTENDED, seip);
        if (res < 0)
            pr2serr_lk("sgh_dd: %s: SG_SET_GET_EXTENDED(SGAT_ELEM_SZ) rd "
                       "error: %s\n", __func__, strerror(errno));
        if (elem_sz != (int)seip->sgat_elem_sz) {
            memset(seip, 0, sizeof(*seip));
            seip->sei_wr_mask |= SG_SEIM_SGAT_ELEM_SZ;
            seip->sgat_elem_sz = elem_sz;
            res = ioctl(fd, SG_SET_GET_EXTENDED, seip);
            if (res < 0)
                pr2serr_lk("sgh_dd: %s: SG_SET_GET_EXTENDED(SGAT_ELEM_SZ) "
                           "wr error: %s\n", __func__, strerror(errno));
        }
    }
    if (! def_res) {
        num = bs * bpt;
        res = ioctl(fd, SG_SET_RESERVED_SIZE, &num);
        if (res < 0)
            perror("sgh_dd: SG_SET_RESERVED_SIZE error");
        else if (mmpp) {
            mmp = (uint8_t *)mmap(NULL, num, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
            if (MAP_FAILED == mmp) {
                perror("error using mmap()");
                return 0;
            }
            *mmpp = mmp;
        }
    }
    t = 1;
    res = ioctl(fd, SG_SET_FORCE_PACK_ID, &t);
    if (res < 0)
        perror("sgh_dd: SG_SET_FORCE_PACK_ID error");
    return (res < 0) ? 0 : num;
}

static bool
process_flags(const char * arg, struct flags_t * fp)
{
    char buff[256];
    char * cp;
    char * np;

    strncpy(buff, arg, sizeof(buff));
    buff[sizeof(buff) - 1] = '\0';
    if ('\0' == buff[0]) {
        pr2serr("no flag found\n");
        return false;
    }
    cp = buff;
    do {
        np = strchr(cp, ',');
        if (np)
            *np++ = '\0';
        if (0 == strcmp(cp, "append"))
            fp->append = true;
        else if (0 == strcmp(cp, "coe"))
            fp->coe = true;
        else if (0 == strcmp(cp, "defres"))
            fp->defres = true;
        else if (0 == strcmp(cp, "dio"))
            fp->dio = true;
        else if (0 == strcmp(cp, "direct"))
            fp->direct = true;
        else if (0 == strcmp(cp, "dpo"))
            fp->dpo = true;
        else if (0 == strcmp(cp, "dsync"))
            fp->dsync = true;
        else if (0 == strcmp(cp, "excl"))
            fp->excl = true;
        else if (0 == strcmp(cp, "fua"))
            fp->fua = true;
        else if (0 == strcmp(cp, "mmap"))
            fp->mmap = true;
        else if (0 == strcmp(cp, "noshare"))
            fp->noshare = true;
        else if (0 == strcmp(cp, "noxfer"))
            fp->noxfer = true;
        else if (0 == strcmp(cp, "null"))
            ;
        else if (0 == strcmp(cp, "same_fds"))
            fp->same_fds = true;
        else if (0 == strcmp(cp, "swait"))
            fp->swait = true;
        else if (0 == strcmp(cp, "v3"))
            fp->v3 = true;
        else if (0 == strcmp(cp, "v4"))
            fp->v4 = true;
        else {
            pr2serr("unrecognised flag: %s\n", cp);
            return false;
        }
        cp = np;
    } while (cp);
    return true;
}

/* Returns the number of times 'ch' is found in string 's' given the
 * string's length. */
static int
num_chs_in_str(const char * s, int slen, int ch)
{
    int res = 0;

    while (--slen >= 0) {
        if (ch == s[slen])
            ++res;
    }
    return res;
}

static int
sg_in_open(Gbl_coll *clp, const char *inf, uint8_t **mmpp, int * mmap_lenp)
{
    int fd, err, n;
    int flags = O_RDWR;
    char ebuff[EBUFF_SZ];

    if (clp->in_flags.direct)
        flags |= O_DIRECT;
    if (clp->in_flags.excl)
        flags |= O_EXCL;
    if (clp->in_flags.dsync)
        flags |= O_SYNC;

    if ((fd = open(inf, flags)) < 0) {
        err = errno;
        snprintf(ebuff, EBUFF_SZ, "%s: could not open %s for sg reading",
                 __func__, inf);
        perror(ebuff);
        return -sg_convert_errno(err);
    }
    n = sg_prepare_resbuf(fd, clp->bs, clp->bpt, clp->in_flags.defres,
                         clp->elem_sz,  mmpp);
    if (n <= 0)
        return -SG_LIB_FILE_ERROR;
    if (mmap_lenp)
        *mmap_lenp = n;
    return fd;
}

static int
sg_out_open(Gbl_coll *clp, const char *outf, uint8_t **mmpp, int * mmap_lenp)
{
    int fd, err, n;
    int flags = O_RDWR;
    char ebuff[EBUFF_SZ];

    if (clp->out_flags.direct)
        flags |= O_DIRECT;
    if (clp->out_flags.excl)
        flags |= O_EXCL;
    if (clp->out_flags.dsync)
        flags |= O_SYNC;

    if ((fd = open(outf, flags)) < 0) {
        err = errno;
        snprintf(ebuff,  EBUFF_SZ, "%s: could not open %s for sg writing",
                 __func__, outf);
        perror(ebuff);
        return -sg_convert_errno(err);
    }
    n = sg_prepare_resbuf(fd, clp->bs, clp->bpt, clp->out_flags.defres,
                          clp->elem_sz, mmpp);
    if (n <= 0)
        return -SG_LIB_FILE_ERROR;
    if (mmap_lenp)
        *mmap_lenp = n;
    return fd;
}

#define STR_SZ 1024
#define INOUTF_SZ 512

int
main(int argc, char * argv[])
{
    bool verbose_given = false;
    bool version_given = false;
    bool bpt_given = false;
    bool cdbsz_given = false;
    int64_t skip = 0;
    int64_t seek = 0;
    int ibs = 0;
    int obs = 0;
    char str[STR_SZ];
    char * key;
    char * buf;
    char inf[INOUTF_SZ];
    char outf[INOUTF_SZ];
    char out2f[INOUTF_SZ];
    char outregf[INOUTF_SZ];
    int res, k, err, keylen;
    int64_t in_num_sect = 0;
    int64_t out_num_sect = 0;
    int in_sect_sz, out_sect_sz, status, n, flags;
    void * vp;
    Gbl_coll * clp = &gcoll;
    Thread_info thread_arr[MAX_NUM_THREADS];
    char ebuff[EBUFF_SZ];
#if SG_LIB_ANDROID
    struct sigaction actions;

    memset(&actions, 0, sizeof(actions));
    sigemptyset(&actions.sa_mask);
    actions.sa_flags = 0;
    actions.sa_handler = thread_exit_handler;
    sigaction(SIGUSR1, &actions, NULL);
    sigaction(SIGUSR2, &actions, NULL);
#endif
    memset(clp, 0, sizeof(*clp));
    memset(thread_arr, 0, sizeof(thread_arr));
    clp->bpt = DEF_BLOCKS_PER_TRANSFER;
    clp->in_type = FT_OTHER;
    /* change dd's default: if of=OFILE not given, assume /dev/null */
    clp->out_type = FT_DEV_NULL;
    clp->out2_type = FT_DEV_NULL;
    clp->cdbsz_in = DEF_SCSI_CDBSZ;
    clp->cdbsz_out = DEF_SCSI_CDBSZ;
    inf[0] = '\0';
    outf[0] = '\0';
    out2f[0] = '\0';
    outregf[0] = '\0';

    for (k = 1; k < argc; k++) {
        if (argv[k]) {
            strncpy(str, argv[k], STR_SZ);
            str[STR_SZ - 1] = '\0';
        }
        else
            continue;
        for (key = str, buf = key; *buf && *buf != '=';)
            buf++;
        if (*buf)
            *buf++ = '\0';
        keylen = strlen(key);
        if (0 == strcmp(key, "ae")) {
            clp->aen = sg_get_num(buf);
            if (clp->aen < 0) {
                pr2serr("%sbad argument to 'ae=', want 0 or higher\n",
                        my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "bpt")) {
            clp->bpt = sg_get_num(buf);
            if (-1 == clp->bpt) {
                pr2serr("%sbad argument to 'bpt='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
            bpt_given = true;
        } else if (0 == strcmp(key, "bs")) {
            clp->bs = sg_get_num(buf);
            if (-1 == clp->bs) {
                pr2serr("%sbad argument to 'bs='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "cdbsz")) {
            clp->cdbsz_in = sg_get_num(buf);
            clp->cdbsz_out = clp->cdbsz_in;
            cdbsz_given = true;
        } else if (0 == strcmp(key, "coe")) {
            clp->in_flags.coe = !! sg_get_num(buf);
            clp->out_flags.coe = clp->in_flags.coe;
        } else if (0 == strcmp(key, "count")) {
            if (0 != strcmp("-1", buf)) {
                dd_count = sg_get_llnum(buf);
                if (-1LL == dd_count) {
                    pr2serr("%sbad argument to 'count='\n", my_name);
                    return SG_LIB_SYNTAX_ERROR;
                }
            }   /* treat 'count=-1' as calculate count (same as not given) */
        } else if ((0 == strncmp(key, "deb", 3)) ||
                   (0 == strncmp(key, "verb", 4)))
            clp->debug = sg_get_num(buf);
        else if (0 == strcmp(key, "dio")) {
            clp->in_flags.dio = !! sg_get_num(buf);
            clp->out_flags.dio = clp->in_flags.dio;
        } else if (0 == strcmp(key, "elemsz_kb")) {
            clp->elem_sz = sg_get_num(buf) * 1024;
            if ((clp->elem_sz > 0) && (clp->elem_sz < 4096)) {
                pr2serr("elemsz_kb cannot be less than 4 (4 KB = 4096 "
                        "bytes)\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "fua")) {
            n = sg_get_num(buf);
            if (n & 1)
                clp->out_flags.fua = true;
            if (n & 2)
                clp->in_flags.fua = true;
        } else if (0 == strcmp(key, "ibs")) {
            ibs = sg_get_num(buf);
            if (-1 == ibs) {
                pr2serr("%sbad argument to 'ibs='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "if")) {
            if ('\0' != inf[0]) {
                pr2serr("Second 'if=' argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            } else
                snprintf(inf, INOUTF_SZ, "%s", buf);
        } else if (0 == strcmp(key, "iflag")) {
            if (! process_flags(buf, &clp->in_flags)) {
                pr2serr("%sbad argument to 'iflag='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "obs")) {
            obs = sg_get_num(buf);
            if (-1 == obs) {
                pr2serr("%sbad argument to 'obs='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (strcmp(key, "of2") == 0) {
            if ('\0' != out2f[0]) {
                pr2serr("Second OFILE2 argument??\n");
                return SG_LIB_CONTRADICT;
            } else
                strncpy(out2f, buf, INOUTF_SZ - 1);
        } else if (strcmp(key, "ofreg") == 0) {
            if ('\0' != outregf[0]) {
                pr2serr("Second OFREG argument??\n");
                return SG_LIB_CONTRADICT;
            } else
                strncpy(outregf, buf, INOUTF_SZ - 1);
        } else if (strcmp(key, "of") == 0) {
            if ('\0' != outf[0]) {
                pr2serr("Second 'of=' argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            } else
                snprintf(outf, INOUTF_SZ, "%s", buf);
        } else if (0 == strcmp(key, "oflag")) {
            if (! process_flags(buf, &clp->out_flags)) {
                pr2serr("%sbad argument to 'oflag='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "seek")) {
            seek = sg_get_llnum(buf);
            if (-1LL == seek) {
                pr2serr("%sbad argument to 'seek='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "skip")) {
            skip = sg_get_llnum(buf);
            if (-1LL == skip) {
                pr2serr("%sbad argument to 'skip='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "sync"))
            do_sync = !! sg_get_num(buf);
        else if (0 == strcmp(key, "thr"))
            num_threads = sg_get_num(buf);
        else if (0 == strcmp(key, "time"))
            do_time = !! sg_get_num(buf);
        else if ((keylen > 1) && ('-' == key[0]) && ('-' != key[1])) {
            res = 0;
            n = num_chs_in_str(key + 1, keylen - 1, 'd');
            clp->dry_run += n;
            res += n;
            n = num_chs_in_str(key + 1, keylen - 1, 'h');
            clp->help += n;
            res += n;
            n = num_chs_in_str(key + 1, keylen - 1, 'v');
            if (n > 0)
                verbose_given = true;
            clp->debug += n;   /* -v  ---> --verbose */
            res += n;
            n = num_chs_in_str(key + 1, keylen - 1, 'V');
            if (n > 0)
                version_given = true;
            res += n;

            if (res < (keylen - 1)) {
                pr2serr("Unrecognised short option in '%s', try '--help'\n",
                        key);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if ((0 == strncmp(key, "--dry-run", 9)) ||
                   (0 == strncmp(key, "--dry_run", 9)))
            ++clp->dry_run;
        else if ((0 == strncmp(key, "--help", 6)) ||
                   (0 == strcmp(key, "-?")))
            ++clp->help;
        else if (0 == strncmp(key, "--verb", 6)) {
            verbose_given = true;
            ++clp->debug;      /* --verbose */
        } else if (0 == strncmp(key, "--vers", 6))
            version_given = true;
        else {
            pr2serr("Unrecognized option '%s'\n", key);
            pr2serr("For more information use '--help'\n");
            return SG_LIB_SYNTAX_ERROR;
        }
    }

#ifdef DEBUG
    pr2serr("In DEBUG mode, ");
    if (verbose_given && version_given) {
        pr2serr("but override: '-vV' given, zero verbose and continue\n");
        verbose_given = false;
        version_given = false;
        clp->debug = 0;
    } else if (! verbose_given) {
        pr2serr("set '-vv'\n");
        clp->debug = 2;
    } else
        pr2serr("keep verbose=%d\n", clp->debug);
#else
    if (verbose_given && version_given)
        pr2serr("Not in DEBUG mode, so '-vV' has no special action\n");
#endif
    if (version_given) {
        pr2serr("%s%s\n", my_name, version_str);
        return 0;
    }
    if (clp->help > 0) {
        usage(clp->help);
        return 0;
    }
    if (clp->bs <= 0) {
        clp->bs = DEF_BLOCK_SIZE;
        pr2serr("Assume default 'bs' ((logical) block size) of %d bytes\n",
                clp->bs);
    }
    if ((ibs && (ibs != clp->bs)) || (obs && (obs != clp->bs))) {
        pr2serr("If 'ibs' or 'obs' given must be same as 'bs'\n");
        usage(0);
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((skip < 0) || (seek < 0)) {
        pr2serr("skip and seek cannot be negative\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (clp->out_flags.append && (seek > 0)) {
        pr2serr("Can't use both append and seek switches\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (clp->bpt < 1) {
        pr2serr("bpt must be greater than 0\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (clp->in_flags.mmap && clp->out_flags.mmap) {
        pr2serr("mmap flag on both IFILE and OFILE doesn't work\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (clp->out_flags.mmap && !(clp->in_flags.noshare ||
                                  clp->out_flags.noshare)) {
        pr2serr("oflag=mmap needs either iflag=noshare or oflag=noshare\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((clp->in_flags.mmap || clp->out_flags.mmap) &&
        (clp->in_flags.same_fds || clp->in_flags.same_fds)) {
        pr2serr("can't have both 'mmap' and 'same_fds' flags\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (((! clp->in_flags.noshare) && clp->in_flags.dio) ||
        ((! clp->out_flags.noshare) && clp->out_flags.dio)) {
        pr2serr("dio flag can only be used with noshare flag\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    /* defaulting transfer size to 128*2048 for CD/DVDs is too large
       for the block layer in lk 2.6 and results in an EIO on the
       SG_IO ioctl. So reduce it in that case. */
    if ((clp->bs >= 2048) && (! bpt_given))
        clp->bpt = DEF_BLOCKS_PER_2048TRANSFER;
    if ((num_threads < 1) || (num_threads > MAX_NUM_THREADS)) {
        pr2serr("too few or too many threads requested\n");
        usage(1);
        return SG_LIB_SYNTAX_ERROR;
    }
    if (clp->in_flags.swait && (! clp->out_flags.swait))
        pr2serr("iflag=swait is ignored, it should be oflag=swait\n");
    if (clp->debug)
        pr2serr("%sif=%s skip=%" PRId64 " of=%s seek=%" PRId64 " count=%"
                PRId64 "\n", my_name, inf, skip, outf, seek, dd_count);

    install_handler(SIGINT, interrupt_handler);
    install_handler(SIGQUIT, interrupt_handler);
    install_handler(SIGPIPE, interrupt_handler);
    install_handler(SIGUSR1, siginfo_handler);
    install_handler(SIGUSR2, siginfo2_handler);

    clp->infd = STDIN_FILENO;
    clp->outfd = STDOUT_FILENO;
    if (inf[0] && ('-' != inf[0])) {
        clp->in_type = dd_filetype(inf);

        if (FT_ERROR == clp->in_type) {
            pr2serr("%sunable to access %s\n", my_name, inf);
            return SG_LIB_FILE_ERROR;
        } else if (FT_ST == clp->in_type) {
            pr2serr("%sunable to use scsi tape device %s\n", my_name, inf);
            return SG_LIB_FILE_ERROR;
        } else if (FT_SG == clp->in_type) {
            clp->infd = sg_in_open(clp, inf, NULL, NULL);
            if (clp->infd < 0)
                return -clp->infd;
        } else {
            flags = O_RDONLY;
            if (clp->in_flags.direct)
                flags |= O_DIRECT;
            if (clp->in_flags.excl)
                flags |= O_EXCL;
            if (clp->in_flags.dsync)
                flags |= O_SYNC;

            if ((clp->infd = open(inf, flags)) < 0) {
                err = errno;
                snprintf(ebuff, EBUFF_SZ, "%scould not open %s for reading",
                         my_name, inf);
                perror(ebuff);
                return sg_convert_errno(err);
            } else if (skip > 0) {
                off64_t offset = skip;

                offset *= clp->bs;       /* could exceed 32 here! */
                if (lseek64(clp->infd, offset, SEEK_SET) < 0) {
                    err = errno;
                    snprintf(ebuff, EBUFF_SZ, "%scouldn't skip to required "
                             "position on %s", my_name, inf);
                    perror(ebuff);
                    return sg_convert_errno(err);
                }
            }
        }
        clp->infp = inf;
        if ((clp->in_flags.v3 || clp->in_flags.v4) &&
            (FT_SG != clp->in_type)) {
            clp->in_flags.v3 = false;
            clp->in_flags.v4 = false;
            pr2serr("%siflag= v3 and v4 both ignored when IFILE is not sg "
                    "device\n", my_name);
        }
    }
    if (outf[0])
        clp->ofile_given = true;
    if (outf[0] && ('-' != outf[0])) {
        clp->out_type = dd_filetype(outf);

        if (FT_ST == clp->out_type) {
            pr2serr("%sunable to use scsi tape device %s\n", my_name, outf);
            return SG_LIB_FILE_ERROR;
        }
        else if (FT_SG == clp->out_type) {
            clp->outfd = sg_out_open(clp, outf, NULL, NULL);
            if (clp->outfd < 0)
                return -clp->outfd;
        }
        else if (FT_DEV_NULL == clp->out_type)
            clp->outfd = -1; /* don't bother opening */
        else {
            if (FT_RAW != clp->out_type) {
                flags = O_WRONLY | O_CREAT;
                if (clp->out_flags.direct)
                    flags |= O_DIRECT;
                if (clp->out_flags.excl)
                    flags |= O_EXCL;
                if (clp->out_flags.dsync)
                    flags |= O_SYNC;
                if (clp->out_flags.append)
                    flags |= O_APPEND;

                if ((clp->outfd = open(outf, flags, 0666)) < 0) {
                    err = errno;
                    snprintf(ebuff, EBUFF_SZ, "%scould not open %s for "
                             "writing", my_name, outf);
                    perror(ebuff);
                    return sg_convert_errno(err);
                }
            }
            else {      /* raw output file */
                if ((clp->outfd = open(outf, O_WRONLY)) < 0) {
                    err = errno;
                    snprintf(ebuff, EBUFF_SZ, "%scould not open %s for raw "
                             "writing", my_name, outf);
                    perror(ebuff);
                    return sg_convert_errno(err);
                }
            }
            if (seek > 0) {
                off64_t offset = seek;

                offset *= clp->bs;       /* could exceed 32 bits here! */
                if (lseek64(clp->outfd, offset, SEEK_SET) < 0) {
                    err = errno;
                    snprintf(ebuff, EBUFF_SZ, "%scouldn't seek to required "
                             "position on %s", my_name, outf);
                    perror(ebuff);
                    return sg_convert_errno(err);
                }
            }
        }
        clp->outfp = outf;
        if ((clp->out_flags.v3 || clp->out_flags.v4) &&
            (FT_SG != clp->out_type)) {
            clp->out_flags.v3 = false;
            clp->out_flags.v4 = false;
            pr2serr("%soflag= v3 and v4 both ignored when OFILE is not sg "
                    "device\n", my_name);
        }
    }

    if (out2f[0])
        clp->ofile2_given = true;
    if (out2f[0] && ('-' != out2f[0])) {
        clp->out2_type = dd_filetype(out2f);

        if (FT_ST == clp->out2_type) {
            pr2serr("%sunable to use scsi tape device %s\n", my_name, out2f);
            return SG_LIB_FILE_ERROR;
        }
        else if (FT_SG == clp->out2_type) {
            clp->out2fd = sg_out_open(clp, out2f, NULL, NULL);
            if (clp->out2fd < 0)
                return -clp->out2fd;
        }
        else if (FT_DEV_NULL == clp->out2_type)
            clp->out2fd = -1; /* don't bother opening */
        else {
            if (FT_RAW != clp->out2_type) {
                flags = O_WRONLY | O_CREAT;
                if (clp->out_flags.direct)
                    flags |= O_DIRECT;
                if (clp->out_flags.excl)
                    flags |= O_EXCL;
                if (clp->out_flags.dsync)
                    flags |= O_SYNC;
                if (clp->out_flags.append)
                    flags |= O_APPEND;

                if ((clp->out2fd = open(out2f, flags, 0666)) < 0) {
                    err = errno;
                    snprintf(ebuff, EBUFF_SZ, "%scould not open %s for "
                             "writing", my_name, out2f);
                    perror(ebuff);
                    return sg_convert_errno(err);
                }
            }
            else {      /* raw output file */
                if ((clp->out2fd = open(out2f, O_WRONLY)) < 0) {
                    err = errno;
                    snprintf(ebuff, EBUFF_SZ, "%scould not open %s for raw "
                             "writing", my_name, out2f);
                    perror(ebuff);
                    return sg_convert_errno(err);
                }
            }
            if (seek > 0) {
                off64_t offset = seek;

                offset *= clp->bs;       /* could exceed 32 bits here! */
                if (lseek64(clp->out2fd, offset, SEEK_SET) < 0) {
                    err = errno;
                    snprintf(ebuff, EBUFF_SZ, "%scouldn't seek to required "
                             "position on %s", my_name, out2f);
                    perror(ebuff);
                    return sg_convert_errno(err);
                }
            }
        }
        clp->out2fp = out2f;
    }
    if ((FT_SG == clp->in_type ) && (FT_SG == clp->out_type)) {
        if (clp->in_flags.v4 && (! clp->out_flags.v3)) {
            if (! clp->out_flags.v4) {
                clp->out_flags.v4 = true;
                if (clp->debug)
                    pr2serr("Changing OFILE from v3 to v4, use oflag=v3 to "
                            "force v3\n");
            }
        }
        if (clp->out_flags.v4 && (! clp->in_flags.v3)) {
            if (! clp->in_flags.v4) {
                clp->in_flags.v4 = true;
                if (clp->debug)
                    pr2serr("Changing IFILE from v3 to v4, use iflag=v3 to "
                            "force v3\n");
            }
        }
    }
    if (outregf[0]) {
        int ftyp = dd_filetype(outregf);

        clp->outreg_type = ftyp;
        if (! ((FT_OTHER == ftyp) || (FT_ERROR == ftyp) ||
               (FT_DEV_NULL == ftyp))) {
            pr2serr("File: %s can only be regular file or pipe (or "
                    "/dev/null)\n", outregf);
            return SG_LIB_SYNTAX_ERROR;
        }
        if ((clp->outregfd = open(outregf, O_WRONLY | O_CREAT, 0666)) < 0) {
            err = errno;
            snprintf(ebuff, EBUFF_SZ, "could not open %s for writing",
                     outregf);
            perror(ebuff);
            return sg_convert_errno(err);
        }
        if (clp->debug > 1)
            pr2serr("ofreg=%s opened okay, fd=%d\n", outregf, clp->outregfd);
        if (FT_ERROR == ftyp)
            clp->outreg_type = FT_OTHER;        /* regular file created */
    } else
        clp->outregfd = -1;

    if ((STDIN_FILENO == clp->infd) && (STDOUT_FILENO == clp->outfd)) {
        pr2serr("Won't default both IFILE to stdin _and_ OFILE to "
                "/dev/null\n");
        pr2serr("For more information use '--help'\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (dd_count < 0) {
        in_num_sect = -1;
        if (FT_SG == clp->in_type) {
            res = scsi_read_capacity(clp->infd, &in_num_sect, &in_sect_sz);
            if (2 == res) {
                pr2serr("Unit attention, media changed(in), continuing\n");
                res = scsi_read_capacity(clp->infd, &in_num_sect,
                                         &in_sect_sz);
            }
            if (0 != res) {
                if (res == SG_LIB_CAT_INVALID_OP)
                    pr2serr("read capacity not supported on %s\n", inf);
                else if (res == SG_LIB_CAT_NOT_READY)
                    pr2serr("read capacity failed, %s not ready\n", inf);
                else
                    pr2serr("Unable to read capacity on %s\n", inf);
                in_num_sect = -1;
            }
        } else if (FT_BLOCK == clp->in_type) {
            if (0 != read_blkdev_capacity(clp->infd, &in_num_sect,
                                          &in_sect_sz)) {
                pr2serr("Unable to read block capacity on %s\n", inf);
                in_num_sect = -1;
            }
            if (clp->bs != in_sect_sz) {
                pr2serr("logical block size on %s confusion; bs=%d, from "
                        "device=%d\n", inf, clp->bs, in_sect_sz);
                in_num_sect = -1;
            }
        }
        if (in_num_sect > skip)
            in_num_sect -= skip;

        out_num_sect = -1;
        if (FT_SG == clp->out_type) {
            res = scsi_read_capacity(clp->outfd, &out_num_sect, &out_sect_sz);
            if (2 == res) {
                pr2serr("Unit attention, media changed(out), continuing\n");
                res = scsi_read_capacity(clp->outfd, &out_num_sect,
                                         &out_sect_sz);
            }
            if (0 != res) {
                if (res == SG_LIB_CAT_INVALID_OP)
                    pr2serr("read capacity not supported on %s\n", outf);
                else if (res == SG_LIB_CAT_NOT_READY)
                    pr2serr("read capacity failed, %s not ready\n", outf);
                else
                    pr2serr("Unable to read capacity on %s\n", outf);
                out_num_sect = -1;
            }
        } else if (FT_BLOCK == clp->out_type) {
            if (0 != read_blkdev_capacity(clp->outfd, &out_num_sect,
                                          &out_sect_sz)) {
                pr2serr("Unable to read block capacity on %s\n", outf);
                out_num_sect = -1;
            }
            if (clp->bs != out_sect_sz) {
                pr2serr("logical block size on %s confusion: bs=%d, from "
                        "device=%d\n", outf, clp->bs, out_sect_sz);
                out_num_sect = -1;
            }
        }
        if (out_num_sect > seek)
            out_num_sect -= seek;

        if (in_num_sect > 0) {
            if (out_num_sect > 0)
                dd_count = (in_num_sect > out_num_sect) ? out_num_sect :
                                                          in_num_sect;
            else
                dd_count = in_num_sect;
        }
        else
            dd_count = out_num_sect;
    }
    if (clp->debug > 2)
        pr2serr("Start of loop, count=%" PRId64 ", in_num_sect=%" PRId64
                ", out_num_sect=%" PRId64 "\n", dd_count, in_num_sect,
                out_num_sect);
    if (dd_count < 0) {
        pr2serr("Couldn't calculate count, please give one\n");
        return SG_LIB_CAT_OTHER;
    }
    if (! cdbsz_given) {
        if ((FT_SG == clp->in_type) && (MAX_SCSI_CDBSZ != clp->cdbsz_in) &&
            (((dd_count + skip) > UINT_MAX) || (clp->bpt > USHRT_MAX))) {
            pr2serr("Note: SCSI command size increased to 16 bytes (for "
                    "'if')\n");
            clp->cdbsz_in = MAX_SCSI_CDBSZ;
        }
        if ((FT_SG == clp->out_type) && (MAX_SCSI_CDBSZ != clp->cdbsz_out) &&
            (((dd_count + seek) > UINT_MAX) || (clp->bpt > USHRT_MAX))) {
            pr2serr("Note: SCSI command size increased to 16 bytes (for "
                    "'of')\n");
            clp->cdbsz_out = MAX_SCSI_CDBSZ;
        }
    }

    clp->in_count = dd_count;
    clp->in_rem_count = dd_count;
    clp->skip = skip;
    // clp->in_blk = skip;
    clp->out_count = dd_count;
    clp->out_rem_count = dd_count;
    clp->seek = seek;
    clp->out_blk = seek;
    status = pthread_mutex_init(&clp->in_mutex, NULL);
    if (0 != status) err_exit(status, "init in_mutex");
    status = pthread_mutex_init(&clp->out_mutex, NULL);
    if (0 != status) err_exit(status, "init out_mutex");
    status = pthread_mutex_init(&clp->out2_mutex, NULL);
    if (0 != status) err_exit(status, "init out2_mutex");
    status = pthread_cond_init(&clp->out_sync_cv, NULL);
    if (0 != status) err_exit(status, "init out_sync_cv");

    if (clp->dry_run > 0) {
        pr2serr("Due to --dry-run option, bypass copy/read\n");
        goto fini;
    }
    if (! clp->ofile_given)
        pr2serr("of=OFILE not given so only read from IFILE, to output to "
                "stdout use 'of=-'\n");

    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    status = pthread_sigmask(SIG_BLOCK, &signal_set, NULL);
    if (0 != status) err_exit(status, "pthread_sigmask");
    status = pthread_create(&sig_listen_thread_id, NULL,
                            sig_listen_thread, (void *)clp);
    if (0 != status) err_exit(status, "pthread_create, sig...");

    if (do_time) {
        start_tm.tv_sec = 0;
        start_tm.tv_usec = 0;
        gettimeofday(&start_tm, NULL);
    }

/* vvvvvvvvvvv  Start worker threads  vvvvvvvvvvvvvvvvvvvvvvvv */
    if ((clp->out_rem_count > 0) && (num_threads > 0)) {
        Thread_info *tip = thread_arr + 0;

        tip->gcp = clp;
        tip->id = 0;
        /* Run 1 work thread to shake down infant retryable stuff */
        status = pthread_mutex_lock(&clp->out_mutex);
        if (0 != status) err_exit(status, "lock out_mutex");
        status = pthread_create(&tip->a_pthr, NULL, read_write_thread,
                                (void *)tip);
        if (0 != status) err_exit(status, "pthread_create");

        /* wait for any broadcast */
        pthread_cleanup_push(cleanup_out, (void *)clp);
        status = pthread_cond_wait(&clp->out_sync_cv, &clp->out_mutex);
        if (0 != status) err_exit(status, "cond out_sync_cv");
        pthread_cleanup_pop(0);
        status = pthread_mutex_unlock(&clp->out_mutex);
        if (0 != status) err_exit(status, "unlock out_mutex");

        /* now start the rest of the threads */
        for (k = 1; k < num_threads; ++k) {
            tip = thread_arr + k;
            tip->gcp = clp;
            tip->id = k;
            status = pthread_create(&tip->a_pthr, NULL, read_write_thread,
                                    (void *)tip);
            if (0 != status) err_exit(status, "pthread_create");
        }

        /* now wait for worker threads to finish */
        for (k = 0; k < num_threads; ++k) {
            tip = thread_arr + k;
            status = pthread_join(tip->a_pthr, &vp);
            if (0 != status) err_exit(status, "pthread_join");
            if (clp->debug > 0)
                pr2serr_lk("Worker thread k=%d terminated\n", k);
        }
    }   /* started worker threads and here after they have all exited */

    if (do_time && (start_tm.tv_sec || start_tm.tv_usec))
        calc_duration_throughput(0);

    if (do_sync) {
        if (FT_SG == clp->out_type) {
            pr2serr_lk(">> Synchronizing cache on %s\n", outf);
            res = sg_ll_sync_cache_10(clp->outfd, 0, 0, 0, 0, 0, false, 0);
            if (SG_LIB_CAT_UNIT_ATTENTION == res) {
                pr2serr_lk("Unit attention(out), continuing\n");
                res = sg_ll_sync_cache_10(clp->outfd, 0, 0, 0, 0, 0, false,
                                          0);
            }
            if (0 != res)
                pr2serr_lk("Unable to synchronize cache\n");
        }
        if (FT_SG == clp->out2_type) {
            pr2serr_lk(">> Synchronizing cache on %s\n", out2f);
            res = sg_ll_sync_cache_10(clp->out2fd, 0, 0, 0, 0, 0, false, 0);
            if (SG_LIB_CAT_UNIT_ATTENTION == res) {
                pr2serr_lk("Unit attention(out2), continuing\n");
                res = sg_ll_sync_cache_10(clp->out2fd, 0, 0, 0, 0, 0, false,
                                          0);
            }
            if (0 != res)
                pr2serr_lk("Unable to synchronize cache (of2)\n");
        }
    }

    shutting_down = true;
    status = pthread_kill(sig_listen_thread_id, SIGINT);
    if (0 != status) err_exit(status, "pthread_kill");
    /* valgrind says the above _kill() leaks; web says it needs a following
     * _join() to clear heap taken by associated _create() */

fini:

    if ((STDIN_FILENO != clp->infd) && (clp->infd >= 0))
        close(clp->infd);
    if ((STDOUT_FILENO != clp->outfd) && (FT_DEV_NULL != clp->out_type) &&
        (clp->outfd >= 0))
        close(clp->outfd);
    if ((clp->out2fd >= 0) && (STDOUT_FILENO != clp->out2fd) &&
        (FT_DEV_NULL != clp->out2_type))
        close(clp->out2fd);
    if ((clp->outregfd >= 0) && (STDOUT_FILENO != clp->outregfd) &&
        (FT_DEV_NULL != clp->outreg_type))
        close(clp->outregfd);
    res = exit_status;
    if ((0 != clp->out_count) && (0 == clp->dry_run)) {
        pr2serr(">>>> Some error occurred, remaining blocks=%" PRId64 "\n",
                clp->out_count);
        if (0 == res)
            res = SG_LIB_CAT_OTHER;
    }
    print_stats("");
    if (clp->dio_incomplete_count) {
        int fd;
        char c;

        pr2serr(">> Direct IO requested but incomplete %d times\n",
                clp->dio_incomplete_count);
        if ((fd = open(proc_allow_dio, O_RDONLY)) >= 0) {
            if (1 == read(fd, &c, 1)) {
                if ('0' == c)
                    pr2serr(">>> %s set to '0' but should be set to '1' for "
                            "direct IO\n", proc_allow_dio);
            }
            close(fd);
        }
    }
    if (clp->sum_of_resids)
        pr2serr(">> Non-zero sum of residual counts=%d\n",
               clp->sum_of_resids);
    return (res >= 0) ? res : SG_LIB_CAT_OTHER;
}
