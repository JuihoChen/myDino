#include <QWidget>

#include <dirent.h>
#include <linux/bsg.h>
#include <scsi/scsi_bsg_mpi3mr.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

#include "smp_mptctl_glue.h"
#include "mpi30_transport.h"
#include "mpi30_sas.h"
#include "mpi3mr_app.h"
#include "smp_lib.h"
#include "widget.h"

/* make a space reserved for mpb (mpi3mr bsg packet)
 *      mpi3mr_bsg_packet - mpi3mr_bsg_drv_cmd
 *                        - mpi3mr_bsg_mptcmd - mpi3mr_buf_entry_list - mpi3mr_buf_entry
 *                                                                    + mpi3mr_buf_entry * 10
 */
#define NUM_BYTES (sizeof(struct mpi3mr_bsg_packet) + (10 * sizeof(struct mpi3mr_buf_entry)))

static char mbp_pool[NUM_BYTES];
static char request_m[1024];
static char reply_m[1024];
static const char * dev_bsg = "/dev/bsg";
static struct mpi3mr_bsg_packet & mbp = *(struct mpi3mr_bsg_packet *)mbp_pool;

/* Returns 0 on success else -1 . */
int
send_req_mpi3mr_bsg(int fd, int subvalue, int64_t target_sa, smp_req_resp * rresp, int vb)
{
    struct sg_io_v4 hdr;
    struct mpi3_smp_passthrough_request * mpi_request;
    struct mpi3_smp_passthrough_reply * mpi_reply;
    u16 request_sz = sizeof(struct mpi3_smp_passthrough_request);
    u16 reply_sz = sizeof(struct mpi3_smp_passthrough_reply);
    int bi = 0, res;

    memset(&hdr, 0, sizeof(hdr));
    memset(mbp_pool, 0, sizeof(mbp_pool));
    memset(request_m, 0, sizeof(request_m));
    memset(reply_m, 0, sizeof(reply_m));

    memcpy(request_m, rresp->request, rresp->request_len);
    mpi_request = (struct mpi3_smp_passthrough_request *)(request_m + rresp->request_len);
    mpi_request->function = MPI3_FUNCTION_SMP_PASSTHROUGH;
    mpi_request->io_unit_port = 0xFF;       // ?invalid port number (rphy)
    mpi_request->sas_address = target_sa;

    mbp.cmd_type = MPI3MR_MPT_CMD;
    mbp.cmd.mptcmd.timeout = 180;           // ?a hacked value
    mbp.cmd.mptcmd.mrioc_id = subvalue;     // Set the IOC number prior to issuing this command.
    mbp.cmd.mptcmd.buf_entry_list.num_of_entries = 4;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[bi].buf_type = MPI3MR_BSG_BUFTYPE_DATA_OUT;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[bi].buf_len = rresp->request_len;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[++bi].buf_type = MPI3MR_BSG_BUFTYPE_DATA_IN;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[bi].buf_len = rresp->max_response_len;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[++bi].buf_type = MPI3MR_BSG_BUFTYPE_MPI_REPLY;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[bi].buf_len = reply_sz;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[++bi].buf_type = MPI3MR_BSG_BUFTYPE_MPI_REQUEST;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[bi].buf_len = request_sz;

    if (vb > 2) {
        qDebug() << "mpi3mr_bsg_packet:";
        hex2stdout((const char *)mbp_pool, NUM_BYTES, 0);
    }

    hdr.guard = 'Q';
    hdr.protocol = BSG_PROTOCOL_SCSI;
    hdr.subprotocol = BSG_SUB_PROTOCOL_SCSI_TRANSPORT;

    hdr.request_len = sizeof(mbp) + (bi * sizeof(struct mpi3mr_buf_entry));
    hdr.request = (uintptr_t) mbp_pool;

    hdr.max_response_len = sizeof(reply_m);
    hdr.response = (uintptr_t) reply_m;

    hdr.dout_xfer_len = rresp->request_len + request_sz;
    hdr.dout_xferp = (uintptr_t) request_m;

    hdr.din_xfer_len = rresp->max_response_len + reply_sz;
    hdr.din_xferp = (uintptr_t) reply_m;

    hdr.timeout = DEF_TIMEOUT_MS;

    if (vb > 1)
        qDebug("%s: dout_xfer_len=%u, din_xfer_len=%u, timeout=%u ms",
               __func__, hdr.dout_xfer_len, hdr.din_xfer_len, hdr.timeout);

    res = ioctl(fd, SG_IO, &hdr);
    if (res) {
        perror("send_req_mpi3mr_bsg: SG_IO ioctl");
        return -1;
    }

    memcpy(rresp->response, reply_m, rresp->max_response_len);
    mpi_reply = (struct mpi3_smp_passthrough_reply *)(reply_m + rresp->max_response_len);

    rresp->act_response_len = mpi_reply->response_data_length;
    /* was: rresp->act_response_len = -1; */
    if (vb > 1) {
        qDebug("%s: driver_status=%u, transport_status=%u", __func__, hdr.driver_status, hdr.transport_status);
        qDebug("    device_status=%u, duration=%u, info=%u", hdr.device_status, hdr.duration, hdr.info);
        qDebug("    din_resid=%d, dout_resid=%d", hdr.din_resid, hdr.dout_resid);
        qDebug("  smp_req_resp::max_response_len=%d act_response_len=%d", rresp->max_response_len, rresp->act_response_len);
        hex2stdout((const char *)rresp->response, rresp->act_response_len, 1);
        hex2stdout((const char *)mpi_reply, reply_sz, 1);
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
mpi3mrdev_scan_select(const struct dirent * s)
{
    if (strstr(s->d_name, "mpi3mrctl")) {
        return 1;
    }
    /* Still need to filter out "." and ".." */
    return 0;
}

void
mpi3mr_discover(int vb)
{
    int num, k, res;
    struct dirent ** namelist;
    smp_target_obj tobj;
    QString device_name;

    if (vb)
        qDebug("Discovering...");

    num = scandir(dev_bsg, &namelist, mpi3mrdev_scan_select, nullptr);
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

        res = smp_initiator_open(device_name, I_SGV4_MPI, &tobj, vb);
        if (res < 0) {
            continue;
        }

        // assign the IOC number for multiple adapters case
        tobj.subvalue = k;

        for (int i = 0; i < 4; ++i) {
            // assign sas address for path-through
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

void
mpi3mr_slot_discover(int vb)
{
    int num, k, res;
    struct dirent ** namelist;
    smp_target_obj tobj;
    QString device_name;

    if (vb)
        qDebug("Slot discovering...");

    num = scandir(dev_bsg, &namelist, mpi3mrdev_scan_select, alphasort);
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

        res = smp_initiator_open(device_name, I_SGV4_MPI, &tobj, vb);
        if (res < 0) {
            continue;
        }

        // assign the IOC number for multiple adapters case
        tobj.subvalue = k;

        for (int i = 0; i < 4; ++i) {
            // check if the next expander is to be discovered
            if (true == gControllers.bsgPath(i).isEmpty()) {
                // assign sas address for path-through
                tobj.sas_addr64 = gControllers.wwid64(i);
                if (0 != tobj.sas_addr64) {
                    if (vb) {
                        qDebug("   -> with SAS address=0x%lx", tobj.sas_addr64);
                    }
                    res = do_multiple_slot(&tobj, vb);
                    if (res) {
                        qDebug("Exit status %d indicates error detected", res);
                    }
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
