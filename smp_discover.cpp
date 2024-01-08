#include <QWidget>

#include <byteswap.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/bsg.h>
#include <scsi/sg.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "mpi_type.h"
#include "mpi.h"
#include "mpi_sas.h"
#ifndef __user
#define __user
#endif
#ifndef u8
typedef unsigned char u8;
#endif
#ifndef u16
typedef unsigned short u16;
#endif
#ifndef u32
typedef unsigned int u32;
#endif
#include "mptctl.h"

#include "widget.h"
#include "smp_discover.h"

static const char * dev_bsg = "/dev/bsg";
static const char * dev_mpt = "/dev";

static int mptcommand = (int)MPT2COMMAND;

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

static const char * smp_short_attached_device_type[] = {
    "",         /* was "no " */
    "",         /* was "end" */
    "exp",
    "fex",      /* obsolete in sas2r05a */
    "res",
    "res",
    "res",
    "res",
};

static inline uint64_t sg_get_unaligned_be64(const void *p)
{
    uint64_t u;

    memcpy(&u, p, 8);
    return bswap_64(u);
}

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
smp_memalign(uint32_t num_bytes, uint32_t align_to, uint8_t ** buff_to_free, int vb)
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
            qDebug("%s: posix_memalign: error [%d], out of memory?", __func__, err);
            return NULL;
        }
        memset(wp, 0, num_bytes);
        if (buff_to_free)
            *buff_to_free = (uint8_t *)wp;
        res = (uint8_t *)wp;

        if (vb > 2) {
            QString msg = QString::asprintf("%s: posix_ma, len=%d, ", __func__, num_bytes);
            if (buff_to_free) {
                msg += QString::asprintf("wrkBuffp=%p, ", res);
            }
            msg += QString::asprintf("psz=%u, rp=%p", (unsigned)psz, res);
            qDebug() << msg;
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
        if (vb > 2) {
            QString msg = QString::asprintf("%s: hack, len=%d, ", __func__, num_bytes);
            if (buff_to_free) {
                msg += QString::asprintf("buff_to_free=%p, ", wrkBuff);
            }
            msg += QString::asprintf("align_1=%lu, rp=%p", (unsigned long)align_1, res);
            qDebug() << msg;
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

/* Note the ASCII-hex output goes to stream stdout.
 * 'no_ascii' allows for 3 output types:
 *     > 0     each line has address then up to 16 ASCII-hex bytes
 *     = 0     in addition, the bytes are listed in ASCII to the right
 *     < 0     only the ASCII-hex bytes are listed (i.e. without address) */
static void
hex2stdout(const char* str, int len, int no_ascii)
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
        formatstr = "%.76s";
    else if (no_ascii > 0)
        formatstr = "%s";     /* was: "%.58s\n" */
    else /* negative: no address at left and no ASCII at right */
        formatstr = "%s";     /* was: "%.48s\n"; */
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
                qDebug(formatstr, buff);
                bpos = bpstart;
                memset(buff, ' ', 80);
            } else
                bpos += 3;
        }
        if (bpos > bpstart) {
            buff[bpos + 2] = '\0';
            trimTrailingSpaces(buff);
            qDebug() << buff;
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
            qDebug(formatstr, buff);
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
        qDebug() << buff;
    }
}

/* Returns 0 on success else -1 . */
static int
send_req_lin_bsg(int fd, struct smp_req_resp * rresp, int vb)
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

    if (vb > 1)
        qDebug("%s: dout_xfer_len=%u, din_xfer_len=%u, timeout=%u ms",
               __func__, hdr.dout_xfer_len, hdr.din_xfer_len, hdr.timeout);

    res = ioctl(fd, SG_IO, &hdr);
    if (res) {
        perror("send_req_lin_bsg: SG_IO ioctl");
        return -1;
    }
    res = hdr.din_xfer_len - hdr.din_resid;
    rresp->act_response_len = res;
    /* was: rresp->act_response_len = -1; */
    if (vb > 1) {
        qDebug("%s: driver_status=%u, transport_status=%u", __func__, hdr.driver_status, hdr.transport_status);
        qDebug("    device_status=%u, duration=%u, info=%u", hdr.device_status, hdr.duration, hdr.info);
        qDebug("    din_resid=%d, dout_resid=%d", hdr.din_resid, hdr.dout_resid);
        qDebug("  smp_req_resp::max_response_len=%d act_response_len=%d", rresp->max_response_len, res);
        qDebug("  response (din_resid might exclude CRC):");
        hex2stdout((const char *)rresp->response, (res > 0) ? res : (int)hdr.din_xfer_len, 1);
    }
    if (hdr.driver_status)
        rresp->transport_err = hdr.driver_status;
    else if (hdr.transport_status)
        rresp->transport_err = hdr.transport_status;
    else if (hdr.device_status)
        rresp->transport_err = hdr.device_status;
    return 0;
}

typedef struct mpt_ioctl_command mpiIoctlBlk_t;

/*****************************************************************
 * issueMptIoctl
 *
 * Generic command to issue the MPT command using the special
 * SDI_IOC | 0x01 Ioctl Function.
 *****************************************************************/
int
issueMptCommand(int fd, int ioc_num, mpiIoctlBlk_t *mpiBlkPtr)
{
    int status = -1;

    /* Set the IOC number prior to issuing this command.
     */
    mpiBlkPtr->hdr.iocnum = ioc_num;
    mpiBlkPtr->hdr.port = 0;

    if (ioctl(fd, mptcommand, (char *) mpiBlkPtr) != 0)
        perror("MPTCOMMAND or MPT2COMMAND ioctl failed");
    else {
        status = 0;
    }

    return status;
}

/* Part of interface to upper level. */
static int
send_req_mpt(int fd, int64_t target_sa, struct smp_req_resp * rresp, int vb)
{
    mpiIoctlBlk_t * mpiBlkPtr = NULL;
    pSmpPassthroughRequest_t smpReq;
    pSmpPassthroughReply_t smpReply;
    uint numBytes;
    int  status;
    char reply_m[1200];
    U16  ioc_stat;
    int ret = -1;

    if (vb && (0 == target_sa)) {
        qDebug("The MPT interface typically needs SAS address of target (e.g. expander).");
    }
    if (vb > 2) {
        qDebug("SAS address=0x%lX", target_sa);
    }
    numBytes = offsetof(SmpPassthroughRequest_t, SGL) + (2 * sizeof(SGESimple64_t));
    mpiBlkPtr = (mpiIoctlBlk_t *)malloc(sizeof(mpiIoctlBlk_t) + numBytes);
    if (mpiBlkPtr == NULL)
        goto err_out;
    memset(mpiBlkPtr, 0, sizeof(mpiIoctlBlk_t) + numBytes);
    mpiBlkPtr->replyFrameBufPtr = reply_m;
    memset(mpiBlkPtr->replyFrameBufPtr, 0, sizeof(reply_m));
    mpiBlkPtr->maxReplyBytes = sizeof(reply_m);
    smpReq = (pSmpPassthroughRequest_t)mpiBlkPtr->MF;
    mpiBlkPtr->dataSgeOffset = offsetof(SmpPassthroughRequest_t, SGL) / 4;
    smpReply = (pSmpPassthroughReply_t)mpiBlkPtr->replyFrameBufPtr;

    /* send smp request */
    mpiBlkPtr->dataOutSize = rresp->request_len - 4;
    mpiBlkPtr->dataOutBufPtr = (char *)rresp->request;
    mpiBlkPtr->dataInSize = rresp->max_response_len + 4;
    mpiBlkPtr->dataInBufPtr = (char *)malloc(mpiBlkPtr->dataInSize);
    if(mpiBlkPtr->dataInBufPtr == NULL)
        goto err_out;
    memset(mpiBlkPtr->dataInBufPtr, 0, mpiBlkPtr->dataInSize);

    /* Populate the SMP Request */

    /* PassthroughFlags
     * Bit7: 0=two SGLs 1=Payload returned in Reply
     */
    /* >>> memo LSI: bug fix on following line's 3rd argument (thanks to clang compiler)
     */
    memset(smpReq, 0, sizeof(*smpReq));

    smpReq->RequestDataLength = rresp->request_len - 4; // <<<<<<<<<<<< ??
    smpReq->Function = MPI_FUNCTION_SMP_PASSTHROUGH;
    memcpy(&smpReq->SASAddress, &target_sa, 8);

    status = issueMptCommand(fd, 0, mpiBlkPtr);

    if (status != 0) {
        qDebug("ioctl failed");
        goto err_out;
    }

    ioc_stat = smpReply->IOCStatus & MPI_IOCSTATUS_MASK;
    if ((ioc_stat != MPI_IOCSTATUS_SUCCESS) ||
        (smpReply->SASStatus != MPI_SASSTATUS_SUCCESS)) {
        if (vb) {
            switch(smpReply->SASStatus) {
            case MPI_SASSTATUS_UNKNOWN_ERROR:
                qDebug("Unknown SAS (SMP) error");
                break;
            case MPI_SASSTATUS_INVALID_FRAME:
                qDebug("Invalid frame");
                break;
            case MPI_SASSTATUS_UTC_BAD_DEST:
                qDebug("Unable to connect (bad destination)");
                break;
            case MPI_SASSTATUS_UTC_BREAK_RECEIVED:
                qDebug("Unable to connect (break received)");
                break;
            case MPI_SASSTATUS_UTC_CONNECT_RATE_NOT_SUPPORTED:
                qDebug("Unable to connect (connect rate not supported)");
                break;
            case MPI_SASSTATUS_UTC_PORT_LAYER_REQUEST:
                qDebug("Unable to connect (port layer request)");
                break;
            case MPI_SASSTATUS_UTC_PROTOCOL_NOT_SUPPORTED:
                qDebug("Unable to connect (protocol (SMP target) not supported)");
                break;
            case MPI_SASSTATUS_UTC_WRONG_DESTINATION:
                qDebug("Unable to connect (wrong destination)");
                break;
            case MPI_SASSTATUS_SHORT_INFORMATION_UNIT:
                qDebug("Short information unit");
                break;
            case MPI_SASSTATUS_DATA_INCORRECT_DATA_LENGTH:
                qDebug("Incorrect data length");
                break;
            case MPI_SASSTATUS_INITIATOR_RESPONSE_TIMEOUT:
                qDebug("Initiator response timeout");
                break;
            default:
                if (smpReply->SASStatus != MPI_SASSTATUS_SUCCESS) {
                    qDebug("Unrecognized SAS (SMP) error 0x%x", smpReply->SASStatus);
                    break;
                }
                if (smpReply->IOCStatus == MPI_IOCSTATUS_SAS_SMP_REQUEST_FAILED)
                    qDebug("SMP request failed (IOCStatus)");
                else if (smpReply->IOCStatus == MPI_IOCSTATUS_SAS_SMP_DATA_OVERRUN)
                    qDebug("SMP data overrun (IOCStatus)");
                else if (smpReply->IOCStatus == MPI_IOCSTATUS_SCSI_DEVICE_NOT_THERE)
                    qDebug("Device not there (IOCStatus)");
                else
                    qDebug("IOCStatus=0x%x", smpReply->IOCStatus);
            }
        }
        if (vb > 1)
            qDebug("IOCStatus=0x%X IOCLogInfo=0x%X SASStatus=0x%X",
                    smpReply->IOCStatus,
                    smpReply->IOCLogInfo,
                    smpReply->SASStatus);
    } else
        ret = 0;

    memcpy(rresp->response, mpiBlkPtr->dataInBufPtr, rresp->max_response_len);
    rresp->act_response_len = -1;

err_out:
    if (mpiBlkPtr) {
        if (mpiBlkPtr->dataInBufPtr)
            free(mpiBlkPtr->dataInBufPtr);
        free(mpiBlkPtr);
    }
    return ret;
}

static int
smp_send_req(const struct smp_target_obj * tobj, struct smp_req_resp * rresp, int vb)
{
    if ((NULL == tobj) || (0 == tobj->opened)) {
        qDebug("%s: nothing open??", __func__);
        return -1;
    }
    if (I_SGV4 == tobj->selector)
        return send_req_lin_bsg(tobj->fd, rresp, vb);
    else if (I_MPT == tobj->selector)
        return send_req_mpt(tobj->fd, tobj->sas_addr64, rresp, vb);
#if 0
    else if (I_AAC == tobj->selector)
        return send_req_aac(tobj->fd, tobj->subvalue, tobj->sas_addr, rresp, vb);
#endif
    else {
        qDebug("%s: no transport??", __func__);
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
get_num_phys(struct smp_target_obj * top, uint8_t * rp, bool * t2t_routingp, int vb)
{
    bool t2t;
    int len, res, k, act_resplen;
    char * cp;
    uint8_t smp_req[] = {SMP_FRAME_TYPE_REQ, SMP_FN_REPORT_GENERAL, 0, 0, 0, 0, 0, 0};
    struct smp_req_resp smp_rr;
    char b[256];

    if (vb) {
        QString msg = "    Report general request: ";
        for (k = 0; k < (int)sizeof(smp_req); ++k)
            msg += QString::asprintf("%02x ", smp_req[k]);
        qDebug() << msg;
    }
    memset(&smp_rr, 0, sizeof(smp_rr));
    smp_rr.request_len = sizeof(smp_req);
    smp_rr.request = smp_req;
    smp_rr.max_response_len = SMP_FN_REPORT_GENERAL_RESP_LEN;
    smp_rr.response = rp;
    res = smp_send_req(top, &smp_rr, vb);

    if (res) {
        qDebug("RG smp_send_req failed, res=%d", res);
        return -1;
    }
    if (smp_rr.transport_err) {
        qDebug("RG smp_send_req transport_error=%d", smp_rr.transport_err);
        return -1;
    }
    act_resplen = smp_rr.act_response_len;
    if ((act_resplen >= 0) && (act_resplen < 4)) {
        qDebug("RG response too short, len=%d", act_resplen);
        return -4 - SMP_LIB_CAT_MALFORMED;
    }
    len = rp[3];
    if ((0 == len) && (0 == rp[2])) {
        len = smp_get_func_def_resp_len(rp[1]);
        if (len < 0) {
            len = 0;
            if (vb)
                qDebug("unable to determine RG response length");
        }
    }
    len = 4 + (len * 4);        /* length in bytes, excluding 4 byte CRC */
    if ((act_resplen >= 0) && (len > act_resplen)) {
        if (vb)
            qDebug("actual RG response length [%d] less than deduced length [%d]", act_resplen, len);
        len = act_resplen;
    }
    /* ignore --hex and --raw */
    if (SMP_FRAME_TYPE_RESP != rp[0]) {
        qDebug("RG expected SMP frame response type, got=0x%x", rp[0]);
        return -4 - SMP_LIB_CAT_MALFORMED;
    }
    if (rp[1] != smp_req[1]) {
        qDebug("RG Expected function code=0x%x, got=0x%x", smp_req[1], rp[1]);
        return -4 - SMP_LIB_CAT_MALFORMED;
    }
    if (rp[2]) {
        if (vb) {
            cp = smp_get_func_res_str(rp[2], sizeof(b), b);
            qDebug("Report General result: %s", cp);
        }
        return -4 - rp[2];
    }
    t2t = (len > 10) ? !!(0x80 & rp[10]) : false;
    if (t2t_routingp)
        *t2t_routingp = t2t;
    if (vb > 1)
        qDebug("%s: len=%d, number of phys: %u, t2t=%d", __func__, len, rp[9], (int)t2t);
    return (len > 9) ? rp[9] : 0;
}

/* Returns length of response in bytes, excluding the CRC on success,
   -3 (or less) -> SMP_LIB errors negated (-4 - smp_err),
   -1 for other errors */
static int
do_discover(struct smp_target_obj * top, int disc_phy_id, uint8_t * resp, int max_resp_len, int vb)
{
    int len, res, k, act_resplen;
    char * cp;
    uint8_t smp_req[] = {SMP_FRAME_TYPE_REQ, SMP_FN_DISCOVER, 0, 0,
                         0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0};
    char b[256];
    struct smp_req_resp smp_rr;

    memset(resp, 0, max_resp_len);
    len = (max_resp_len - 8) / 4;
    smp_req[2] = (len < 0x100) ? len : 0xff; /* Allocated Response Len */
    smp_req[3] = 2; /* Request Length: in dwords */
    smp_req[9] = disc_phy_id;
    if (vb) {
        QString msg = "    Discover request: ";
        for (k = 0; k < (int)sizeof(smp_req); ++k)
            msg += QString::asprintf("%02x ", smp_req[k]);
        qDebug() << msg;
    }
    memset(&smp_rr, 0, sizeof(smp_rr));
    smp_rr.request_len = sizeof(smp_req);
    smp_rr.request = smp_req;
    smp_rr.max_response_len = max_resp_len;
    smp_rr.response = resp;
    res = smp_send_req(top, &smp_rr, vb);

    if (res) {
        qDebug("smp_send_req failed, res=%d", res);
        return -1;
    }
    if (smp_rr.transport_err) {
        qDebug("smp_send_req transport_error=%d", smp_rr.transport_err);
        return -1;
    }
    act_resplen = smp_rr.act_response_len;
    if ((act_resplen >= 0) && (act_resplen < 4)) {
        qDebug("response too short, len=%d", act_resplen);
        return -4 - SMP_LIB_CAT_MALFORMED;
    }
    len = resp[3];
    if ((0 == len) && (0 == resp[2])) {
        len = smp_get_func_def_resp_len(resp[1]);
        if (len < 0) {
            len = 0;
            if (vb)
                qDebug("unable to determine response length");
        }
    }
    len = 4 + (len * 4);        /* length in bytes, excluding 4 byte CRC */
    if ((act_resplen >= 0) && (len > act_resplen)) {
        if (vb)
            qDebug("actual response length [%d] less than deduced length [%d]", act_resplen, len);
        len = act_resplen;
    }
    /* ignore --hex and --raw */
    if (SMP_FRAME_TYPE_RESP != resp[0]) {
        qDebug("expected SMP frame response type, got=0x%x", resp[0]);
        return -4 - SMP_LIB_CAT_MALFORMED;
    }
    if (resp[1] != smp_req[1]) {
        qDebug("RG Expected function code=0x%x, got=0x%x", smp_req[1], resp[1]);
        return -4 - SMP_LIB_CAT_MALFORMED;
    }
    if (resp[2]) {
        if (vb) {
            cp = smp_get_func_res_str(resp[2], sizeof(b), b);
            qDebug("Discover result: %s", cp);
        }
        return -4 - resp[2];
    }
    return len;
}

/* Calls do_discover() multiple times. Summarizes info into one
 * line per phy. Returns 0 if ok, else function result. */
static int
do_multiple(struct smp_target_obj * top, int vb)
{
    int ret = 0;
    bool has_t2t = false;
    bool virt;
    int len, k, num, adt, negot, dsn;
    uint64_t ull, adn, expander_sa = 0, enclid;
    const char * cp;
    uint8_t * rp = NULL;
    uint8_t * free_rp = NULL;
    QString os;

    len = qMax(SMP_FN_DISCOVER_RESP_LEN, SMP_FN_REPORT_GENERAL_RESP_LEN);
    rp = smp_memalign(len, 0, &free_rp, vb);
    if (NULL == rp) {
        qDebug("%s: heap allocation problem", __func__);
        return SMP_LIB_RESOURCE_ERROR;
    }

    num = get_num_phys(top, rp, &has_t2t, vb);
    // ENCLOSURE LOGICAL IDENTIFIER (bytes 12-19, in RG response)
    enclid = sg_get_unaligned_be64(rp + 12);
    qDebug("  Enclosure Logical Identifier: %lx", enclid);

    for (k = 0; k < num; ++k) {
        len = do_discover(top, k, rp, SMP_FN_DISCOVER_RESP_LEN, vb);
        if (len < 0)
            ret = (len < -2) ? (-4 - len) : len;
        else
            ret = 0;

        if (SMP_FRES_NO_PHY == ret) {
            ret = 0;   /* expected, end condition */
            goto finish;
        } else if (SMP_FRES_PHY_VACANT == ret) {
            printf("  phy %3d: inaccessible (phy vacant)\n", k);
            continue;
        } else if (ret)
            goto finish;

        /* SAS Address (bytes 16-23) */
        ull = sg_get_unaligned_be64(rp + 16);
        if (0 == expander_sa)
            expander_sa = ull;
        else {
            if (ull != expander_sa) {
                if (ull > 0) {
                    qDebug(">> expander's SAS address is changing?? phy_id=%d, was=%lx, now=%lx", rp[9], expander_sa, ull);
                    expander_sa = ull;
                } else if (vb)
                    qDebug(">> expander's SAS address shown as 0 at phy_id=%d", rp[9]);
            }
        }

        /* Routing Attribute */
        switch(rp[44] & 0xf) {
        case 0:
            cp = "D";
            break;
        case 1:
            cp = "S";
            break;
        case 2:
            /* table routing phy when expander does t2t is Universal */
            cp = has_t2t ? "U" : "T";
            break;
        default:
            cp = "R";
            break;
        }

        /* Device Slot Number */
        dsn = ((len > 108) && (0xff != rp[108])) ? rp[108] : -1;

        /* Negotiated Logical Link Rate */
        negot = rp[13] & 0xf;
        switch (negot) {
        case 1:
            qDebug("  phy %3d:%s:disabled  dsn=%d", rp[9], cp, dsn);
            continue;   /* N.B. not break; finished with this line/phy */
        case 2:
            qDebug("  phy %3d:%s:reset problem  dsn=%d", rp[9], cp, dsn);
            continue;
        case 3:
            qDebug("  phy %3d:%s:spinup hold  dsn=%d", rp[9], cp, dsn);
            continue;
        case 4:
            qDebug("  phy %3d:%s:port selector  dsn=%d", rp[9], cp, dsn);
            continue;
        case 5:
            qDebug("  phy %3d:%s:reset in progress  dsn=%d", rp[9], cp, dsn);
            continue;
        case 6:
            qDebug("  phy %3d:%s:unsupported phy attached  dsn=%d", rp[9], cp, dsn);
            continue;
        default:
            /* keep going in this loop, probably attached to something */
            break;
        }

        /* attached SAS device type: 0-> none, 1-> (SAS or SATA end) device,
         * 2-> expander, 3-> fanout expander (obsolete), rest-> reserved */
        adt = ((0x70 & rp[12]) >> 4);
        if (0 == adt)
            continue;

        /* Phy Identifier (byte 9) */
        if (k != rp[9])
            qDebug(">> requested phy_id=%d differs from response phy=%d", k, rp[9]);

        if ((0 == adt) || (adt > 3)) {
            os = QString::asprintf("  phy %3d:%s:attached:[0000000000000000:00]", k, cp);
            if (len < 64) {
                qDebug() << os;
                continue;
            }
            if (-1 != dsn) {
                os += QString::asprintf("  dsn=%d", dsn);
                qDebug() << os;
            }
            continue;
        }

        /* Attached SAS Address (bytes 16-23) */
        ull = sg_get_unaligned_be64(rp + 24);
        /* Virtual Phy (byte 43 bit 8) */
        virt = !!(0x80 & rp[43]);
        if (len > 59) {
            /* Attached Device Name (bytes 52-59), Attached Phy Identifier (byte 32) */
            adn = sg_get_unaligned_be64(rp + 52);
            os = QString::asprintf("  phy %3d:%s:attached:[%016lx:%02d %016lx %s%s",
                        k, cp, ull, rp[32], adn, smp_short_attached_device_type[adt], (virt ? " V" : ""));
        } else
            os = QString::asprintf("  phy %3d:%s:attached:[%016lx:%02d %s%s",
                        k, cp, ull, rp[32], smp_short_attached_device_type[adt], (virt ? " V" : ""));

        /* 0 : 0 : 0 : 0 :
           ATTACHED SSP INITIATOR : ATTACHED STP INITIATOR : ATTACHED SMP INITIATOR : ATTACHED SATA HOST */
        if (rp[14] & 0xf) {
            QString plus = "";
            os += " i(";
            if (rp[14] & 0x8) {
                os += "SSP";
                plus = "+";
            }
            if (rp[14] & 0x4) {
                os += plus + "STP";
                plus = "+";
            }
            if (rp[14] & 0x2) {
                os += plus + "SMP",
                plus = "+";
            }
            if (rp[14] & 0x1) {
                os += plus + "SATA";
                plus = "+";
            }
            os += ")";
        }
        /* ATTACHED SATA PORT SELECTOR : 0 : 0 : 0 :
           ATTACHED SSP TARGET : ATTACHED STP TARGET : ATTACHED SMP TARGET : ATTACHED SATA DEVICE */
        if (rp[15] & 0xf) {
            QString plus = "";
            os += " t(";
            if (rp[15] & 0x80) {
                os += "PORT_SEL";
                plus = "+";
            }
            if (rp[15] & 0x8) {
                os += plus + "SSP";
                plus = "+";
            }
            if (rp[15] & 0x4) {
                os += plus + "STP";
                plus = "+";
            }
            if (rp[15] & 0x2) {
                os += plus + "SMP";
                plus = "+";
            }
            if (rp[15] & 0x1) {
                os += plus + "SATA";
                plus = "+";
            }
            os += ")";
        }
        os += "]";
        switch(negot) {
        case 8:
            cp = "  1.5 Gbps";
            break;
        case 9:
            cp = "  3 Gbps";
            break;
        case 0xa:
            cp = "  6 Gbps";
            break;
        case 0xb:
            cp = "  12 Gbps";
            break;
        case 0xc:
            cp = "  22.5 Gbps";
            break;
        default:
            cp = "";
            break;
        }
        os += cp;
        if (-1 != dsn) {
            os += QString::asprintf("  dsn=%d", dsn);
        }
        qDebug() << os;
    }

finish:
    if (free_rp)
        free(free_rp);
    return ret;
}

/* Returns open file descriptor to dev_name bsg device or -1 */
static int
open_lin_bsg_device(QString dev_name, int vb)
{
    int ret = -1;

    ret = open(dev_name.toStdString().c_str(), O_RDWR);
    if (ret < 0) {
        if (vb) {
            perror(QString("%1: open() device node failed").arg(__func__).toStdString().c_str());
            qDebug() << "\t\ttried to open " << dev_name;
        }
    }
    return ret;
}

/* Part of interface to upper level. */
int
open_mpt_device(QString dev_name, int vb)
{
    int res;
    struct stat st;

    try {
        res = open(dev_name.toStdString().c_str(), O_RDWR);
        if (res < 0) {
            throw QString("%1: open() device node failed").arg(__func__);
        } else if (fstat(res, &st) >= 0) {
            if ((S_ISCHR(st.st_mode)) && (MPT_DEV_MAJOR == major(st.st_rdev)) &&
                ((MPT2_DEV_MINOR == minor(st.st_rdev)) || (MPT3_DEV_MINOR == minor(st.st_rdev)))) {
                mptcommand = (int)MPT2COMMAND;
            } else {
                res = -1;
                throw QString("%1: device node major, minor unmatched").arg(__func__);
            }
        } else {
            res = -1;
            throw QString("%1: stat failed").arg(__func__);
        }

    } catch (QString msg) {
        if (vb) {
            perror(msg.toStdString().c_str());
            qDebug() << "\t\ttried to open " << dev_name;
        }
    }
    return res;
}

int
smp_initiator_open(QString device_name, Interface sel, struct smp_target_obj * tobj, int vb)
{
    int res = 0;
    tobj->opened = 0;
    // It's silly to "memset" a struct with QString elements
    //memset(tobj, 0, sizeof(struct smp_target_obj));

    try {
        if (I_SGV4 == sel) {
            res = open_lin_bsg_device(device_name, vb);
            if (res < 0) {
                throw res;
            }
        }
        if (I_MPT == sel) {
            res = open_mpt_device(device_name, vb);
            if (res < 0) {
                throw res;
            }
        }
    } catch (...) {
        gAppendMessage(QString("failed to open ") + device_name);
        return res;
    }

    tobj->device_name = device_name;
    tobj->selector = sel;
    tobj->fd = res;
    tobj->opened = 1;
    return 0;
}

int
smp_initiator_close(struct smp_target_obj * tobj)
{
    int res;

    if ((NULL == tobj) || (0 == tobj->opened)) {
        qDebug("%s: nothing open??", __func__);
        return -1;
    }

    res = close(tobj->fd);
    if (res < 0) {
        qDebug() << "failed to close " << tobj->device_name;
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

static int
mptdev_scan_select(const struct dirent * s)
{
    if (strstr(s->d_name, "mpt3ctl")) {
        return 1;
    }
    /* Still need to filter out "." and ".." */
    return 0;
}

void
smp_discover(int vb)
{
    int num, k, res;
    struct dirent ** namelist;
    struct smp_target_obj tobj;
    QString device_name;

    if (vb)
        qDebug("discovering...");

    num = scandir(dev_bsg, &namelist, bsgdev_scan_select, alphasort);
    if (num <= 0) {  /* HBA mid level may not be loaded */
        perror("scandir");
        gAppendMessage("HBA mid level module may not be loaded.");
        return;
    }

    for (k = 0; k < num; ++k) {
        device_name = QString("%1/%2").arg(dev_bsg, namelist[k]->d_name);
        gAppendMessage(device_name);
        if (vb) {
            qDebug() << "----> exploring " << device_name;
        }

        res = smp_initiator_open(device_name, I_SGV4, &tobj, vb);
        if (res < 0) {
            continue;
        }

        res = do_multiple(&tobj, vb);
        if (res) {
            qDebug("Exit status %d indicates error detected", res);
        }

        smp_initiator_close(&tobj);
    }

    for (k = 0; k < num; ++k) {
        free(namelist[k]);
    }
    free(namelist);
}

void
mpt_discover(int vb)
{
    int num, k, res;
    struct dirent ** namelist;
    struct smp_target_obj tobj;
    QString device_name;

    if (vb)
        qDebug("Discovering...");

    num = scandir(dev_mpt, &namelist, mptdev_scan_select, nullptr);
    if (num <= 0) {  /* HBA mid level may not be loaded */
        perror("scandir");
        gAppendMessage("HBA mid level module may not be loaded.");
        return;
    }

    for (k = 0; k < num; ++k) {
        device_name = QString("%1/%2").arg(dev_mpt, namelist[k]->d_name);
        gAppendMessage(device_name);
        if (vb) {
            qDebug() << "----> exploring " << device_name;
        }

        res = smp_initiator_open(device_name, I_MPT, &tobj, vb);
        if (res < 0) {
            continue;
        }

        for (int i = 0; i < 4; ++i) {
            tobj.sas_addr64 = gControllers.wwid64(i);
            if (0 != tobj.sas_addr64) {
                if (vb) {
                    qDebug("   -> with SAS address=0x%lx", tobj.sas_addr64);
                }
                res = do_multiple(&tobj, vb);
                if (res) {
                    qDebug("Exit status %d indicates error detected", res);
                }
            }
        }

        smp_initiator_close(&tobj);
    }

    for (k = 0; k < num; ++k) {
        free(namelist[k]);
    }
    free(namelist);
}

static int
do_multiple_slot(struct smp_target_obj * top, int vb)
{
    int ret = 0;
    int len, k, num, dsn;
    uint64_t ull, expander_sa = 0, hba_sa = 0;
    uint8_t * rp = NULL;
    uint8_t * free_rp = NULL;

    len = SMP_FN_DISCOVER_RESP_LEN;
    rp = smp_memalign(len, 0, &free_rp, false);
    if (NULL == rp) {
        qDebug("%s: heap allocation problem", __func__);
        return SMP_LIB_RESOURCE_ERROR;
    }

    for (k = 0; k < 32; ++k) {
        len = do_discover(top, k, rp, SMP_FN_DISCOVER_RESP_LEN, false);
        if (len < 0)
            ret = (len < -2) ? (-4 - len) : len;
        else
            ret = 0;

        if (SMP_FRES_NO_PHY == ret) {
            ret = 0;   /* expected, end condition */
            goto finish;
        } else if (SMP_FRES_PHY_VACANT == ret) {
            printf("  phy %3d: inaccessible (phy vacant)\n", k);
            continue;
        } else if (ret)
            goto finish;

        /* SAS Address (bytes 16-23) */
        ull = sg_get_unaligned_be64(rp + 16);
        if (0 == expander_sa)
            expander_sa = ull;
        else {
            if (ull != expander_sa) {
                if (ull > 0) {
                    qDebug(">> expander's SAS address is changing?? phy_id=%d, was=%lx, now=%lx", rp[9], expander_sa, ull);
                    expander_sa = ull;
                } else if (vb)
                    qDebug(">> expander's SAS address shown as 0 at phy_id=%d", rp[9]);
            }
        }

        /* 0 : 0 : 0 : 0 :
           ATTACHED SSP INITIATOR : ATTACHED STP INITIATOR : ATTACHED SMP INITIATOR : ATTACHED SATA HOST */
        if (rp[14] & 0xf) {
            /* ATTACHED DEVICE NAME (bytes 52-59) */
            uint64_t sa = sg_get_unaligned_be64(rp + 52);
            if (0 != sa && 0 == hba_sa) {
                hba_sa = sa;
                gControllers.setDiscoverResp(top->device_name, ull, sa, rp, len);
            }
        } else {
            /* Device Slot Number */
            dsn = ((len > 108) && (0xff != rp[108])) ? rp[108] : -1;
            gDevices.setDiscoverResp(dsn, rp, len);
        }
    }

finish:
    if (free_rp)
        free(free_rp);
    return ret;
}

void
slot_discover(int vb)
{
    int num, k, res;
    struct dirent ** namelist;
    struct smp_target_obj tobj;
    QString device_name;

    if (vb)
        qDebug("Slot discovering...");

    num = scandir(dev_bsg, &namelist, bsgdev_scan_select, alphasort);
    if (num <= 0) {  /* HBA mid level may not be loaded */
        perror("scandir");
        gAppendMessage("HBA mid level module may not be loaded.");
        return;
    }

    for (k = 0; k < num; ++k) {
        device_name = QString("%1/%2").arg(dev_bsg, namelist[k]->d_name);
        if (vb) {
            qDebug() << "----> exploring " << device_name;
        }

        res = smp_initiator_open(device_name, I_SGV4, &tobj, vb);
        if (res < 0) {
            continue;
        }

        res = do_multiple_slot(&tobj, vb);
        if (res) {
            qDebug("Exit status %d indicates error detected", res);
        }

        smp_initiator_close(&tobj);
    }

    for (k = 0; k < num; ++k) {
        free(namelist[k]);
    }
    free(namelist);
}

void
phy_control(struct smp_target_obj * tobj, int phy_id, bool disable, int vb)
{
    int k, res;
    uint8_t smp_req[] = {
        SMP_FRAME_TYPE_REQ, SMP_FN_PHY_CONTROL, 0, 9,
        0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    };
    uint8_t smp_resp[8];
    struct smp_req_resp smp_rr;

    smp_req[9] = phy_id;
    smp_req[10] = disable ? 3 : 2;  // 02h: HARD RESET, 03h: DISABLE

    if (vb) {
        QString msg = QString::asprintf("    Phy %s request: ", disable ? "off" : "on");
        for (k = 0; k < (int)sizeof(smp_req); ++k) {
            if (0 == (k % 16)) {
                qDebug() << msg;
                msg = "      ";
            } else if (0 == (k % 8)) {
                msg += " ";
            }
            msg += QString::asprintf("%02x ", smp_req[k]);
        }
        qDebug() << msg;
    }

    memset(&smp_rr, 0, sizeof(smp_rr));
    smp_rr.request_len = sizeof(smp_req);
    smp_rr.request = smp_req;
    smp_rr.max_response_len = sizeof(smp_resp);
    smp_rr.response = smp_resp;
    res = smp_send_req(tobj, &smp_rr, vb);

    if (res) {
        qDebug("smp_send_req failed, res=%d", res);
        if (0 == vb)
            qDebug("    try adding '-v' option for more debug");
    }
}
