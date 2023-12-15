#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/bsg.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "./smp_discover.h"

#define I_MPT   2
#define I_SGV4  4
#define I_AAC   6

#define DEF_TIMEOUT_MS 20000    /* 20 seconds */

/* SAS transport frame types associated with SMP */
#define SMP_FRAME_TYPE_REQ                  0x40
#define SMP_FRAME_TYPE_RESP                 0x41

/* SMP function codes */
#define SMP_FN_REPORT_GENERAL               0x0
#define SMP_FN_REPORT_MANUFACTURER          0x1
#define SMP_FN_READ_GPIO_REG                0x2
#define SMP_FN_REPORT_SELF_CONFIG           0x3
#define SMP_FN_REPORT_ZONE_PERMISSION_TBL   0x4
#define SMP_FN_REPORT_ZONE_MANAGER_PASS     0x5
#define SMP_FN_REPORT_BROADCAST             0x6
#define SMP_FN_READ_GPIO_REG_ENH            0x7
#define SMP_FN_DISCOVER                     0x10
#define SMP_FN_REPORT_PHY_ERR_LOG           0x11
#define SMP_FN_REPORT_PHY_SATA              0x12
#define SMP_FN_REPORT_ROUTE_INFO            0x13
#define SMP_FN_REPORT_PHY_EVENT             0x14
/* #define SMP_FN_REPORT_PHY_BROADCAST 0x15  removed in sas2r13 */
#define SMP_FN_DISCOVER_LIST                0x20  /* was 0x16 in sas2r10 */
#define SMP_FN_REPORT_PHY_EVENT_LIST        0x21
#define SMP_FN_REPORT_EXP_ROUTE_TBL_LIST    0x22  /* was 0x17 in sas2r10 */
#define SMP_FN_CONFIG_GENERAL               0x80
#define SMP_FN_ENABLE_DISABLE_ZONING        0x81
#define SMP_FN_WRITE_GPIO_REG               0x82
#define SMP_FN_WRITE_GPIO_REG_ENH           0x83
#define SMP_FN_ZONED_BROADCAST              0x85
#define SMP_FN_ZONE_LOCK                    0x86
#define SMP_FN_ZONE_ACTIVATE                0x87
#define SMP_FN_ZONE_UNLOCK                  0x88
#define SMP_FN_CONFIG_ZONE_MANAGER_PASS     0x89
#define SMP_FN_CONFIG_ZONE_PHY_INFO         0x8a
#define SMP_FN_CONFIG_ZONE_PERMISSION_TBL   0x8b
#define SMP_FN_CONFIG_ROUTE_INFO            0x90
#define SMP_FN_PHY_CONTROL                  0x91
#define SMP_FN_PHY_TEST_FUNCTION            0x92
#define SMP_FN_CONFIG_PHY_EVENT             0x93

/* SMP function result values */
#define SMP_FRES_FUNCTION_ACCEPTED          0x0
#define SMP_FRES_UNKNOWN_FUNCTION           0x1
#define SMP_FRES_FUNCTION_FAILED            0x2
#define SMP_FRES_INVALID_REQUEST_LEN        0x3
#define SMP_FRES_INVALID_EXP_CHANGE_COUNT   0x4
#define SMP_FRES_BUSY                       0x5
#define SMP_FRES_INCOMPLETE_DESCRIPTOR_LIST 0x6
#define SMP_FRES_NO_PHY                     0x10
#define SMP_FRES_NO_INDEX                   0x11
#define SMP_FRES_NO_SATA_SUPPORT            0x12
#define SMP_FRES_UNKNOWN_PHY_OP             0x13
#define SMP_FRES_UNKNOWN_PHY_TEST_FN        0x14
#define SMP_FRES_PHY_TEST_IN_PROGRESS       0x15
#define SMP_FRES_PHY_VACANT                 0x16
#define SMP_FRES_UNKNOWN_PHY_EVENT_SRC      0x17
#define SMP_FRES_UNKNOWN_DESCRIPTOR_TYPE    0x18
#define SMP_FRES_UNKNOWN_PHY_FILTER         0x19
#define SMP_FRES_AFFILIATION_VIOLATION      0x1a
#define SMP_FRES_SMP_ZONE_VIOLATION         0x20
#define SMP_FRES_NO_MANAGEMENT_ACCESS       0x21
#define SMP_FRES_UNKNOWN_EN_DIS_ZONING_VAL  0x22
#define SMP_FRES_ZONE_LOCK_VIOLATION        0x23
#define SMP_FRES_NOT_ACTIVATED              0x24
#define SMP_FRES_ZONE_GROUP_OUT_OF_RANGE    0x25
#define SMP_FRES_NO_PHYSICAL_PRESENCE       0x26
#define SMP_FRES_SAVING_NOT_SUPPORTED       0x27
#define SMP_FRES_SOURCE_ZONE_GROUP          0x28
#define SMP_FRES_DIS_PASSWORD_NOT_SUPPORTED 0x29
#define SMP_FRES_INVALID_FIELD_IN_REQUEST   0x2a

/* Utilities can use these process status values for syntax errors and
   file (device node) problems (e.g. not found or permissions). Numbers
   between 1 and 32 are reserved for SMP function result values */
#define SMP_LIB_SYNTAX_ERROR                91
#define SMP_LIB_FILE_ERROR                  92
#define SMP_LIB_RESOURCE_ERROR              93
#define SMP_LIB_CAT_MALFORMED               97

#define SMP_FN_DISCOVER_RESP_LEN            124
#define SMP_FN_REPORT_GENERAL_RESP_LEN      76

static const char * dev_bsg = "/dev/bsg";

struct smp_target_obj {
    QString device_name;
    int subvalue;               /* adapter number (opt) */
    unsigned char sas_addr[8];  /* target SMP (opt) */
    uint64_t sas_addr64;        /* target SMP (opt) */
    int interface_selector;
    int opened;
    int fd;
    void * vp;                  /* opaque for pass-through (e.g. CAM) */
};

/* SAS standards include a 4 byte CRC at the end of each SMP request
   and response frames. All current pass-throughs calculate and check
   the CRC in the driver, but some pass-throughs want the space allocated.
 */
struct smp_req_resp {
    int request_len;            /* [i] in bytes, includes space for 4 byte CRC */
    unsigned char * request;    /* [*i], includes space for CRC */
    int max_response_len;       /* [i] in bytes, includes space for CRC */
    unsigned char * response;   /* [*o] */
    int act_response_len;       /* [o] -1 implies don't know */
    int transport_err;          /* [o] 0 implies no error */
};

/* Assume original SAS implementations were based on SAS-1.1 . In SAS-2
 * and later, SMP responses should contain an accurate "response length"
 * field. However is SAS-1.1 (sas1r10.pdf) the "response length field
 * (byte 3) is always 0 irrespective of the response's length. There is
 * a similar problem with the "request length" field in the request.
 * So if zero is found in either the request/response fields this table
 * is consulted.
 * The units of 'def_req_len' and 'def_resp_len' are dwords (4 bytes)
 * calculated by: ((len_bytes - 8) / 4) where 'len_bytes' includes
 * the 4 byte CRC at the end of each frame. The 4 byte CRC field
 * does not need to be set (just space allocated (for some pass
 * throughs)). */
struct smp_func_def_rrlen {
    int func;           /* '-1' for last entry */
    int def_req_len;    /* if 0==<request_length> use this value, unless */
    /*  -2 -> no default; -3 -> different format */
    int def_resp_len;   /* if 0==<response_length> use this value, unless */
    /*  -2 -> no default; -3 -> different format */
    /* N.B. Some SAS-2 functions have 8 byte request or response lengths.
            This is noted by putting 0 in one of the two above fields. */
};

/* Positive request and response lengths match SAS-1.1 (sas1r10.pdf) */
static struct smp_func_def_rrlen smp_def_rrlen_arr[] = {
    /* in numerical order by 'func' */
    {SMP_FN_REPORT_GENERAL, 0, 6},
    {SMP_FN_REPORT_MANUFACTURER, 0, 14},
    {SMP_FN_READ_GPIO_REG, -3, -3}, /* obsolete, not applicable: SFF-8485 */
    {SMP_FN_REPORT_SELF_CONFIG, -2, -2},
    {SMP_FN_REPORT_ZONE_PERMISSION_TBL, -2, -2}, /* variable length response */
    {SMP_FN_REPORT_ZONE_MANAGER_PASS, -2, -2},
    {SMP_FN_REPORT_BROADCAST, -2, -2},
    {SMP_FN_READ_GPIO_REG_ENH, -2, -2}, /* SFF-8485 should explain */
    {SMP_FN_DISCOVER, 2, 0xc},
    {SMP_FN_REPORT_PHY_ERR_LOG, 2, 6},
    {SMP_FN_REPORT_PHY_SATA, 2, 13},
    {SMP_FN_REPORT_ROUTE_INFO, 2, 9},
    {SMP_FN_REPORT_PHY_EVENT, -2, -2}, /* variable length response */
    {SMP_FN_DISCOVER_LIST, -2, -2},
    {SMP_FN_REPORT_PHY_EVENT_LIST, -2, -2},
    {SMP_FN_REPORT_EXP_ROUTE_TBL_LIST, -2, -2},
    {SMP_FN_CONFIG_GENERAL, 3, 0},
    {SMP_FN_ENABLE_DISABLE_ZONING, -2, 0},
    {SMP_FN_WRITE_GPIO_REG, -3, -3}, /* obsolete, not applicable: SFF-8485 */
    {SMP_FN_WRITE_GPIO_REG_ENH, -2, -2}, /* SFF-8485 should explain */
    {SMP_FN_ZONED_BROADCAST, -2, 0}, /* variable length request */
    {SMP_FN_ZONE_LOCK, -2, -2},
    {SMP_FN_ZONE_ACTIVATE, -2, 0},
    {SMP_FN_ZONE_UNLOCK, -2, 0},
    {SMP_FN_CONFIG_ZONE_MANAGER_PASS, -2, 0},
    {SMP_FN_CONFIG_ZONE_PHY_INFO, -2, 0}, /* variable length request */
    {SMP_FN_CONFIG_ZONE_PERMISSION_TBL, -2, 0}, /* variable length request */
    {SMP_FN_CONFIG_ROUTE_INFO, 9, 0},
    {SMP_FN_PHY_CONTROL, 9, 0},
    {SMP_FN_PHY_TEST_FUNCTION, 9, 0},
    {SMP_FN_CONFIG_PHY_EVENT, -2, 0}, /* variable length request */
    {-1, -1, -1},
};

struct smp_val_name {
    int value;
    const char * name;
};

static struct smp_val_name smp_func_results[] =
{
    {SMP_FRES_FUNCTION_ACCEPTED, "SMP function accepted"},
    {SMP_FRES_UNKNOWN_FUNCTION, "Unknown SMP function"},
    {SMP_FRES_FUNCTION_FAILED, "SMP function failed"},
    {SMP_FRES_INVALID_REQUEST_LEN, "Invalid request frame length"},
    {SMP_FRES_INVALID_EXP_CHANGE_COUNT, "Invalid expander change count"},
    {SMP_FRES_BUSY, "Busy"},
    {SMP_FRES_INCOMPLETE_DESCRIPTOR_LIST, "Incomplete descriptor list"},
    {SMP_FRES_NO_PHY, "Phy does not exist"},
    {SMP_FRES_NO_INDEX, "Index does not exist"},
    {SMP_FRES_NO_SATA_SUPPORT, "Phy does not support SATA"},
    {SMP_FRES_UNKNOWN_PHY_OP, "Unknown phy operation"},
    {SMP_FRES_UNKNOWN_PHY_TEST_FN, "Unknown phy test function"},
    {SMP_FRES_PHY_TEST_IN_PROGRESS, "Phy test function in progress"},
    {SMP_FRES_PHY_VACANT, "Phy vacant"},
    {SMP_FRES_UNKNOWN_PHY_EVENT_SRC, "Unknown phy event source"},
    {SMP_FRES_UNKNOWN_DESCRIPTOR_TYPE, "Unknown descriptor type"},
    {SMP_FRES_UNKNOWN_PHY_FILTER, "Unknown phy filter"},
    {SMP_FRES_AFFILIATION_VIOLATION, "Affiliation violation"},
    {SMP_FRES_SMP_ZONE_VIOLATION, "SMP zone violation"},
    {SMP_FRES_NO_MANAGEMENT_ACCESS, "No management access rights"},
    {SMP_FRES_UNKNOWN_EN_DIS_ZONING_VAL, "Unknown enable disable zoning value"},
    {SMP_FRES_ZONE_LOCK_VIOLATION, "Zone lock violation"},
    {SMP_FRES_NOT_ACTIVATED, "Not activated"},
    {SMP_FRES_ZONE_GROUP_OUT_OF_RANGE, "Zone group out of range"},
    {SMP_FRES_NO_PHYSICAL_PRESENCE, "No physical presence"},
    {SMP_FRES_SAVING_NOT_SUPPORTED, "Saving not supported"},
    {SMP_FRES_SOURCE_ZONE_GROUP, "Source zone group does not exist"},
    {SMP_FRES_DIS_PASSWORD_NOT_SUPPORTED, "Disabled password not supported"},
    {SMP_FRES_INVALID_FIELD_IN_REQUEST, "Invalid field in SMP request"},
    {0x0, NULL},
};

static uint32_t
smp_get_page_size(void)
{
#if defined(HAVE_SYSCONF) && defined(_SC_PAGESIZE)
    return sysconf(_SC_PAGESIZE); /* POSIX.1 (was getpagesize()) */
#elif defined(SG_LIB_WIN32)
    if (! got_page_size) {
        SYSTEM_INFO si;

        GetSystemInfo(&si);
        win_page_size = si.dwPageSize;
        got_page_size = true;
    }
    return win_page_size;
#elif defined(SG_LIB_FREEBSD)
    return PAGE_SIZE;
#else
    return 4096;     /* give up, pick likely figure */
#endif
}

/* Returns pointer to heap (or NULL) that is aligned to a align_to byte
 * boundary. Sends back *buff_to_free pointer in third argument that may be
 * different from the return value. If it is different then the *buff_to_free
 * pointer should be freed (rather than the returned value) when the heap is
 * no longer needed. If align_to is 0 then aligns to OS's page size. Sets all
 * returned heap to zeros. If num_bytes is 0 then set to page size. */
static uint8_t *
smp_memalign(uint32_t num_bytes, uint32_t align_to, uint8_t ** buff_to_free, bool vb)
{
    size_t psz;
    uint8_t * res;

    if (buff_to_free)   /* make sure buff_to_free is NULL if alloc fails */
        *buff_to_free = NULL;
    psz = (align_to > 0) ? align_to : smp_get_page_size();
    if (0 == num_bytes)
        num_bytes = psz;        /* ugly to handle otherwise */

#if 1  ///#ifdef HAVE_POSIX_MEMALIGN
    {
        int err;
        void * wp = NULL;

        err = posix_memalign(&wp, psz, num_bytes);
        if (err || (NULL == wp)) {
            printf("%s: posix_memalign: error [%d], out of memory?\n", __func__, err);
            return NULL;
        }
        memset(wp, 0, num_bytes);
        if (buff_to_free)
            *buff_to_free = (uint8_t *)wp;
        res = (uint8_t *)wp;

        if (vb) {
            printf("%s: posix_ma, len=%d, ", __func__, num_bytes);
            if (buff_to_free)
                printf("wrkBuffp=%p, ", (void *)res);
            printf("psz=%u, rp=%p\n", (unsigned int)psz, (void *)res);
        }
        return res;
    }
#else
    {
        void * wrkBuff;
        uintptr_t align_1 = psz - 1;

        wrkBuff = (uint8_t *)calloc(num_bytes + psz, 1);
        if (NULL == wrkBuff) {
            if (buff_to_free)
                *buff_to_free = NULL;
            return NULL;
        } else if (buff_to_free)
            *buff_to_free = (uint8_t *)wrkBuff;
        res = (uint8_t *)(void *)
            (((uintptr_t)wrkBuff + align_1) & (~align_1));
        if (vb) {
            printf("%s: hack, len=%d, ", __func__, num_bytes);
            if (buff_to_free)
                printf("buff_to_free=%p, ", wrkBuff);
            printf("align_1=%lu, rp=%p\n", (unsigned long)align_1, (void *)res);
        }
        return res;
    }
#endif
}

/* Want safe, 'n += snprintf(b + n, blen - n, ...)' style sequence of
 * functions. Returns number of chars placed in cp excluding the
 * trailing null char. So for cp_max_len > 0 the return value is always
  * < cp_max_len; for cp_max_len <= 1 the return value is 0 and no chars are
 * written to cp. Note this means that when cp_max_len = 1, this function
 * assumes that cp[0] is the null character and does nothing (and returns
 * 0). Linux kernel has a similar function called  scnprintf().  */
static int
scnpr(char * cp, int cp_max_len, const char * fmt, ...)
{
    va_list args;
    int n;

    if (cp_max_len < 2)
        return 0;
    va_start(args, fmt);
    n = vsnprintf(cp, cp_max_len, fmt, args);
    va_end(args);
    return (n < cp_max_len) ? n : (cp_max_len - 1);
}

static void
trimTrailingSpaces(char * b)
{
    int k;

    for (k = ((int)strlen(b) - 1); k >= 0; --k) {
        if (' ' != b[k])
            break;
    }
    if ('\0' != b[k + 1])
        b[k + 1] = '\0';
}

/* Simple ASCII printable (does not use locale), includes space and excludes
 * DEL (0x7f). */
static inline int
my_isprint(int ch)
{
    return ((ch >= ' ') && (ch < 0x7f));
}

/* Note the ASCII-hex output goes to stream identified by 'fp'. This usually
 * be either stdout or stderr.
 * 'no_ascii' allows for 3 output types:
 *     > 0     each line has address then up to 16 ASCII-hex bytes
 *     = 0     in addition, the bytes are listed in ASCII to the right
 *     < 0     only the ASCII-hex bytes are listed (i.e. without address) */
static void
dStrHexFp(const char* str, int len, int no_ascii, FILE * fp)
{
    const char * p = str;
    const char * formatstr;
    unsigned char c;
    char buff[82];
    int a = 0;
    int bpstart = 5;
    const int cpstart = 60;
    int cpos = cpstart;
    int bpos = bpstart;
    int i, k, blen;

    if (len <= 0)
        return;
    blen = (int)sizeof(buff);
    if (0 == no_ascii)  /* address at left and ASCII at right */
        formatstr = "%.76s\n";
    else if (no_ascii > 0)
        formatstr = "%s\n";     /* was: "%.58s\n" */
    else /* negative: no address at left and no ASCII at right */
        formatstr = "%s\n";     /* was: "%.48s\n"; */
    memset(buff, ' ', 80);
    buff[80] = '\0';
    if (no_ascii < 0) {
        bpstart = 0;
        bpos = bpstart;
        for (k = 0; k < len; k++) {
            c = *p++;
            if (bpos == (bpstart + (8 * 3)))
                bpos++;
            scnpr(&buff[bpos], blen - bpos, "%.2x", (int)(unsigned char)c);
            buff[bpos + 2] = ' ';
            if ((k > 0) && (0 == ((k + 1) % 16))) {
                trimTrailingSpaces(buff);
                fprintf(fp, formatstr, buff);
                bpos = bpstart;
                memset(buff, ' ', 80);
            } else
                bpos += 3;
        }
        if (bpos > bpstart) {
            buff[bpos + 2] = '\0';
            trimTrailingSpaces(buff);
            fprintf(fp, "%s\n", buff);
        }
        return;
    }
    /* no_ascii>=0, start each line with address (offset) */
    k = scnpr(buff + 1, blen - 1, "%.2x", a);
    buff[k + 1] = ' ';

    for (i = 0; i < len; i++) {
        c = *p++;
        bpos += 3;
        if (bpos == (bpstart + (9 * 3)))
            bpos++;
        scnpr(&buff[bpos], blen - bpos, "%.2x", (int)(unsigned char)c);
        buff[bpos + 2] = ' ';
        if (no_ascii)
            buff[cpos++] = ' ';
        else {
            if (! my_isprint(c))
                c = '.';
            buff[cpos++] = c;
        }
        if (cpos > (cpstart + 15)) {
            if (no_ascii)
                trimTrailingSpaces(buff);
            fprintf(fp, formatstr, buff);
            bpos = bpstart;
            cpos = cpstart;
            a += 16;
            memset(buff, ' ', 80);
            k = scnpr(buff + 1, blen - 1, "%.2x", a);
            buff[k + 1] = ' ';
        }
    }
    if (cpos > cpstart) {
        buff[cpos] = '\0';
        if (no_ascii)
            trimTrailingSpaces(buff);
        fprintf(fp, "%s\n", buff);
    }
}

static void
dStrHex(const char* str, int len, int no_ascii)
{
    dStrHexFp(str, len, no_ascii, stdout);
}

static void
hex2stdout(const uint8_t * b_str, int len, int no_ascii)
{
    dStrHex((const char *)b_str, len, no_ascii);
}

/* Returns 0 on success else -1 . */
static int
send_req_lin_bsg(int fd, struct smp_req_resp * rresp, bool verbose)
{
    struct sg_io_v4 hdr;
    unsigned char cmd[16];      /* unused */
    int res;

    memset(&hdr, 0, sizeof(hdr));
    memset(cmd, 0, sizeof(cmd));

    hdr.guard = 'Q';
    hdr.protocol = BSG_PROTOCOL_SCSI;
    hdr.subprotocol = BSG_SUB_PROTOCOL_SCSI_TRANSPORT;

    hdr.request_len = sizeof(cmd);      /* unused */
    hdr.request = (uintptr_t) cmd;

    hdr.dout_xfer_len = rresp->request_len;
    hdr.dout_xferp = (uintptr_t) rresp->request;

    hdr.din_xfer_len = rresp->max_response_len;
    hdr.din_xferp = (uintptr_t) rresp->response;

    hdr.timeout = DEF_TIMEOUT_MS;

    if (verbose)
        printf("send_req_lin_bsg: dout_xfer_len=%u, din_xfer_len=%u, timeout=%u ms\n",
               hdr.dout_xfer_len, hdr.din_xfer_len, hdr.timeout);

    res = ioctl(fd, SG_IO, &hdr);
    if (res) {
        perror("send_req_lin_bsg: SG_IO ioctl");
        return -1;
    }
    res = hdr.din_xfer_len - hdr.din_resid;
    rresp->act_response_len = res;
    /* was: rresp->act_response_len = -1; */
    if (verbose) {
        printf("send_req_lin_bsg: driver_status=%u, transport_status=%u\n", hdr.driver_status, hdr.transport_status);
        printf("    device_status=%u, duration=%u, info=%u\n", hdr.device_status, hdr.duration, hdr.info);
        printf("    din_resid=%d, dout_resid=%d\n", hdr.din_resid, hdr.dout_resid);
        printf("  smp_req_resp::max_response_len=%d act_response_len=%d\n", rresp->max_response_len, res);
        printf("  response (din_resid might exclude CRC):\n");
        hex2stdout(rresp->response, (res > 0) ? res : (int)hdr.din_xfer_len, 1);
    }
    if (hdr.driver_status)
        rresp->transport_err = hdr.driver_status;
    else if (hdr.transport_status)
        rresp->transport_err = hdr.transport_status;
    else if (hdr.device_status)
        rresp->transport_err = hdr.device_status;
    return 0;
}

static int
smp_send_req(const struct smp_target_obj * tobj, struct smp_req_resp * rresp, bool verbose)
{
    if ((NULL == tobj) || (0 == tobj->opened)) {
        printf("smp_send_req: nothing open??\n");
        return -1;
    }
    if (I_SGV4 == tobj->interface_selector)
        return send_req_lin_bsg(tobj->fd, rresp, verbose);
#if 0
    else if (I_MPT == tobj->interface_selector)
        return send_req_mpt(tobj->fd, tobj->subvalue, tobj->sas_addr64, rresp, verbose);
    else if (I_AAC == tobj->interface_selector)
        return send_req_aac(tobj->fd, tobj->subvalue, tobj->sas_addr, rresp, verbose);
#endif
    else {
        printf("smp_send_req: no transport??\n");
        return -1;
    }
}

static int
smp_get_func_def_resp_len(int func_code)
{
    struct smp_func_def_rrlen * drlp;

    for (drlp = smp_def_rrlen_arr; drlp->func >= 0; ++drlp) {
        if (func_code == drlp->func)
            return drlp->def_resp_len;
    }
    return -1;
}

static char *
smp_get_func_res_str(int func_res, int buff_len, char * buff)
{
    struct smp_val_name * vnp;

    for (vnp = smp_func_results; vnp->name; ++vnp) {
        if (func_res == vnp->value) {
            snprintf(buff, buff_len, "%s", vnp->name);
            return buff;
        }
    }
    snprintf(buff, buff_len, "Unknown function result code=0x%x\n", func_res);
    return buff;
}

/* Returns the number of phys (from REPORT GENERAL response) and if
 * t2t_routingp is non-NULL places 'Table to Table Supported' bit where it
 * points. Returns -3 (or less) -> SMP_LIB errors negated (-4 - smp_err),
 * -1 for other errors. */
static int
get_num_phys(struct smp_target_obj * top, bool * t2t_routingp, bool vb)
{
    bool t2t;
    int len, res, k, act_resplen;
    int ret = 0;
    char * cp;
    uint8_t smp_req[] = {SMP_FRAME_TYPE_REQ, SMP_FN_REPORT_GENERAL, 0, 0, 0, 0, 0, 0};
    struct smp_req_resp smp_rr;
    uint8_t * rp = NULL;
    uint8_t * free_rp = NULL;
    char b[256];

    rp = smp_memalign(SMP_FN_REPORT_GENERAL_RESP_LEN, 0, &free_rp, false);
    if (NULL == rp) {
        printf("%s: heap allocation problem\n", __func__);
        return SMP_LIB_RESOURCE_ERROR;
    }
    if (vb) {
        printf("    Report general request: ");
        for (k = 0; k < (int)sizeof(smp_req); ++k)
            printf("%02x ", smp_req[k]);
        printf("\n");
    }
    memset(&smp_rr, 0, sizeof(smp_rr));
    smp_rr.request_len = sizeof(smp_req);
    smp_rr.request = smp_req;
    smp_rr.max_response_len = SMP_FN_REPORT_GENERAL_RESP_LEN;
    smp_rr.response = rp;
    res = smp_send_req(top, &smp_rr, vb);

    if (res) {
        printf("RG smp_send_req failed, res=%d\n", res);
        ret = -1;
        goto finish;
    }
    if (smp_rr.transport_err) {
        printf("RG smp_send_req transport_error=%d\n", smp_rr.transport_err);
        ret = -1;
        goto finish;
    }
    act_resplen = smp_rr.act_response_len;
    if ((act_resplen >= 0) && (act_resplen < 4)) {
        printf("RG response too short, len=%d\n", act_resplen);
        ret = -4 - SMP_LIB_CAT_MALFORMED;
        goto finish;
    }
    len = rp[3];
    if ((0 == len) && (0 == rp[2])) {
        len = smp_get_func_def_resp_len(rp[1]);
        if (len < 0) {
            len = 0;
            if (vb)
                printf("unable to determine RG response length\n");
        }
    }
    len = 4 + (len * 4);        /* length in bytes, excluding 4 byte CRC */
    if ((act_resplen >= 0) && (len > act_resplen)) {
        if (vb)
            printf("actual RG response length [%d] less than deduced length [%d]\n", act_resplen, len);
        len = act_resplen;
    }
    /* ignore --hex and --raw */
    if (SMP_FRAME_TYPE_RESP != rp[0]) {
        printf("RG expected SMP frame response type, got=0x%x\n", rp[0]);
        ret = -4 - SMP_LIB_CAT_MALFORMED;
        goto finish;
    }
    if (rp[1] != smp_req[1]) {
        printf("RG Expected function code=0x%x, got=0x%x\n", smp_req[1], rp[1]);
        ret = -4 - SMP_LIB_CAT_MALFORMED;
        goto finish;
    }
    if (rp[2]) {
        if (vb) {
            cp = smp_get_func_res_str(rp[2], sizeof(b), b);
            printf("Report General result: %s\n", cp);
        }
        ret = -4 - rp[2];
        goto finish;
    }
    t2t = (len > 10) ? !!(0x80 & rp[10]) : false;
    if (t2t_routingp)
        *t2t_routingp = t2t;
    if (vb)
        printf("%s: len=%d, number of phys: %u, t2t=%d\n", __func__, len, rp[9], (int)t2t);
    ret = (len > 9) ? rp[9] : 0;

finish:
    if (free_rp)
        free(free_rp);
    return ret;
}

/* Calls do_discover() multiple times. Summarizes info into one
 * line per phy. Returns 0 if ok, else function result. */
static int
do_multiple(struct smp_target_obj * top)
{
    int ret = 0;
    int num;
    bool has_t2t = false;
    uint8_t * rp = NULL;
    uint8_t * free_rp = NULL;

    rp = smp_memalign(SMP_FN_DISCOVER_RESP_LEN, 0, &free_rp, false);
    if (NULL == rp) {
        printf("%s: heap allocation problem\n", __func__);
        return SMP_LIB_RESOURCE_ERROR;
    }

    num = get_num_phys(top, &has_t2t, true);

finish:
    if (free_rp)
        free(free_rp);
    return ret;
}

/* Returns open file descriptor to dev_name bsg device or -1 */
static int
open_lin_bsg_device(QString dev_name)
{
    int ret = -1;

    ret = open(dev_name.toStdString().c_str(), O_RDWR);
    if (ret < 0) {
        perror(QString("%1: open() device node failed").arg(__func__).toStdString().c_str());
        printf("\t\ttried to open %s\n", dev_name.toStdString().c_str());
    }
    return ret;
}

static int
smp_initiator_open(QString device_name, struct smp_target_obj * tobj)
{
    int res;

    res = open_lin_bsg_device(device_name);
    if (res < 0) {
        return res;
    }
    tobj->device_name = device_name;
    tobj->interface_selector = I_SGV4;
    tobj->fd = res;
    tobj->opened = 1;
    return 0;
}

static int
smp_initiator_close(struct smp_target_obj * tobj)
{
    int res;

    if ((NULL == tobj) || (0 == tobj->opened)) {
        printf("%s: nothing open??\n", __func__);
        return -1;
    }

    res = close(tobj->fd);
    if (res < 0) {
        printf("failed to close %s\n", tobj->device_name.toStdString().c_str());
    }

    tobj->opened = 0;
    return res;
}

static int
bsgdev_scan_select(const struct dirent * s)
{
    if (strstr(s->d_name, "expander-") && strchr(s->d_name, ':')) {
        return 1;
    }
    /* Still need to filter out "." and ".." */
    return 0;
}

void
smpDiscover(Widget* pw)
{
    int num, k, res;
    struct dirent ** namelist;
    struct smp_target_obj tobj;
    QString device_name;

    printf("discovering...\n");

    num = scandir(dev_bsg, &namelist, bsgdev_scan_select, alphasort);
    if (num < 0) {  /* HBA mid level may not be loaded */
        perror("scandir");
        pw->appendMessage("HBA mid level module may not be loaded.");
        return;
    }

    for (k = 0; k < num; ++k) {
        device_name = QString("%1/%2").arg(dev_bsg, namelist[k]->d_name);
        pw->appendMessage(device_name);

        res = smp_initiator_open(device_name, &tobj);
        if (res < 0) {
            pw->appendMessage(QString("failed to open ") + device_name);
            continue;
        }

        res = do_multiple(&tobj);
        if (res)
            printf("Exit status %d indicates error detected\n", res);

        smp_initiator_close(&tobj);
    }

    for (k = 0; k < num; ++k) {
        free(namelist[k]);
    }
    free(namelist);
}
