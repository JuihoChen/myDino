#include <QString>
#include <QRegularExpression>
#include <byteswap.h>
#include <dirent.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>

#include "widget.h"
#include "lsscsi.h"

#define FT_OTHER 0
#define FT_BLOCK 1
#define FT_CHAR 2

#ifdef PATH_MAX
#define LMAX_PATH PATH_MAX
#else
#define LMAX_PATH 2048
#endif

#ifdef NAME_MAX
#define LMAX_NAME (NAME_MAX + 1)
#else
#define LMAX_NAME 256
#endif

#define LMAX_DEVPATH (LMAX_NAME + 128)

#define UINT64_LAST ((uint64_t)~0)

static const char * sysfsroot = "/sys";
static const char * bus_scsi_devs = "/bus/scsi/devices";

static char errpath[LMAX_PATH];

/* For SCSI 'h' is host_num, 'c' is channel, 't' is target, 'l' is LUN is
 * uint64_t and lun_arr[8] is LUN as 8 byte array. For NVMe, h=0x7fff
 * (NVME_HOST_NUM) and displayed as 'N'; 'c' is Linux's NVMe controller
 * number, 't' is NVMe Identify controller CTNLID field, and 'l' is
 * namespace id (1 to (2**32)-1) rendered as a little endian 4 byte sequence
 * in lun_arr, last 4 bytes are zeros. invalidate_hctl() puts -1 in
 * integers, 0xff in bytes */
struct addr_hctl {
    int h;                 /* if h==0x7fff, display as 'N' for NVMe */
    int c;
    int t;
    uint64_t l;           /* SCSI: Linux word flipped; NVME: uint32_t */
    uint8_t lun_arr[8];   /* T10, SAM-5 order; NVME: little endian */
};

struct item_t {
    QString name;
    int ft;
    int d_type;
};

struct item_t aa_first;
struct item_t enclosure_device;

static const char * scsi_device_types[] =
{
    "Direct-Access",
    "Sequential-Access",
    "Printer",
    "Processor",
    "Write-once",
    "CD-ROM",
    "Scanner",
    "Optical memory",
    "Medium Changer",
    "Communications",
    "Unknown (0xa)",
    "Unknown (0xb)",
    "Storage array",
    "Enclosure",
    "Simplified direct-access",
    "Optical card read/writer",
    "Bridge controller",
    "Object based storage",
    "Automation Drive interface",
    "Security manager",
    "Zoned Block",
    "Reserved (0x15)", "Reserved (0x16)", "Reserved (0x17)",
    "Reserved (0x18)", "Reserved (0x19)", "Reserved (0x1a)",
    "Reserved (0x1b)", "Reserved (0x1c)", "Reserved (0x1e)",
    "Well known LU",
    "No device",
};

static inline void sg_put_unaligned_be16(uint16_t val, void *p)
{
    uint16_t u = bswap_16(val);

    memcpy(p, &u, 2);
}

/* Returns true if dirent entry is either a symlink or a directory
 * starting_with given name. If starting_with is NULL choose all that are
 * either symlinks or directories other than . or .. (own directory or
 * parent) . Can be tricked cause symlink could point to .. (parent), for
 * example. Otherwise return false. */
static bool
dir_or_link(const struct dirent * s, const char * starting_with)
{
    if (DT_LNK == s->d_type) {
        if (starting_with)
            return 0 == strncmp(s->d_name, starting_with, strlen(starting_with));
        return true;
    } else if (DT_DIR != s->d_type)
        return false;
    else {  /* Assume can't have zero length directory name */
        size_t len = strlen(s->d_name);

        if (starting_with)
            return 0 == strncmp(s->d_name, starting_with, strlen(starting_with));
        if (len > 2)
            return true;
        if ('.' == s->d_name[0]) {
            if (1 == len)
                return false;   /* this directory: '.' */
            else if ('.' == s->d_name[1])
                return false;   /* parent: '..' */
        }
        return true;
    }
}

/* Parse colon_list into host/channel/target/lun ("hctl") array, return true
 * if successful, else false. colon_list should point at first character of
 * hctl (i.e. a digit) and yields a new value in *outp when true returned. */
static bool
parse_colon_list(const char * colon_list, struct addr_hctl * outp)
{
    int k;
    uint64_t z;
    const char * elem_end;

    if ((! colon_list) || (! outp))
        return false;

    if (1 != sscanf(colon_list, "%d", &outp->h))
            return false;
    if (NULL == (elem_end = strchr(colon_list, ':')))
        return false;
    colon_list = elem_end + 1;
    if (1 != sscanf(colon_list, "%d", &outp->c))
        return false;
    if (NULL == (elem_end = strchr(colon_list, ':')))
        return false;
    colon_list = elem_end + 1;
    if (1 != sscanf(colon_list, "%d", &outp->t))
        return false;
    if (NULL == (elem_end = strchr(colon_list, ':')))
        return false;
    colon_list = elem_end + 1;
    if (1 != sscanf(colon_list, "%" SCNu64 , &outp->l))
        return false;
    z = outp->l;
    for (k = 0; k < 8; k += 2, z >>= 16)
        sg_put_unaligned_be16((uint16_t)z, outp->lun_arr + k);
    return true;
}

/* Compare <host:controller:target:lun> tuples (aka <h:c:t:l> or hctl) */
static int
cmp_hctl(const struct addr_hctl * le, const struct addr_hctl * ri)
{
    if (le->h == ri->h) {
        if (le->c == ri->c) {
            if (le->t == ri->t)
                return ((le->l == ri->l) ? 0 :
                            ((le->l < ri->l) ? -1 : 1));
            else
                return (le->t < ri->t) ? -1 : 1;
        } else
            return (le->c < ri->c) ? -1 : 1;
    } else
        return (le->h < ri->h) ? -1 : 1;
}

/* Return 1 for directory entry that is link or directory (other than
 * a directory name starting with dot). Else return 0.  */
static int
first_dir_scan_select(const struct dirent * s)
{
    if (FT_OTHER != aa_first.ft)
        return 0;
    if (! dir_or_link(s, NULL))
        return 0;
    aa_first.name = s->d_name;
    aa_first.ft = FT_CHAR;  /* dummy */
    aa_first.d_type = s->d_type;
    return 1;
}

/* scan for directory entry that is either a symlink or a directory. Returns
 * number found or -1 for error. */
static int
scan_for_first(const char * dir_name)
{
    int num, k;
    struct dirent ** namelist;

    aa_first.ft = FT_OTHER;
    num = scandir(dir_name, &namelist, first_dir_scan_select, NULL);
    if (num < 0) {
        snprintf(errpath, LMAX_PATH, "%s: scandir: %s", __func__, dir_name);
        perror(errpath);
        return -1;
    }
    for (k = 0; k < num; ++k)
        free(namelist[k]);
    free(namelist);
    return num;
}

static int
sdev_dir_scan_select(const struct dirent * s)
{
    /* Following no longer needed but leave for early lk 2.6 series */
    if (strstr(s->d_name, "mt"))
        return 0;       /* st auxiliary device names */
    if (strstr(s->d_name, "ot"))
        return 0;       /* osst auxiliary device names */
    if (strstr(s->d_name, "gen"))
        return 0;
    /* Above no longer needed but leave for early lk 2.6 series */
    if (!strncmp(s->d_name, "host", 4)) /* SCSI host */
        return 0;
    if (!strncmp(s->d_name, "target", 6)) /* SCSI target */
        return 0;
    if (strchr(s->d_name, ':')) {
        return 1;
    }
    /* Still need to filter out "." and ".." */
    return 0;
}

/* This is a compare function for numeric sort based on hctl tuple.
 * Returns -1 if (a->d_name < b->d_name) ; 0 if they are equal
 * and 1 otherwise. */
static int
sdev_scandir_sort(const struct dirent ** a, const struct dirent ** b)
{
    const char * lnam = (*a)->d_name;
    const char * rnam = (*b)->d_name;
    struct addr_hctl left_hctl;
    struct addr_hctl right_hctl;

    if (! parse_colon_list(lnam, &left_hctl)) {
        qDebug("%s: left parse failed: %.20s", __func__, (lnam ? lnam : "<null>"));
        return -1;
    }
    if (! parse_colon_list(rnam, &right_hctl)) {
        qDebug("%s: right parse failed: %.20s", __func__, (rnam ? rnam : "<null>"));
        return 1;
    }
    return cmp_hctl(&left_hctl, &right_hctl);
}

static int
enclosure_dir_scan_select(const struct dirent * s)
{
    if (dir_or_link(s, "enclosure")) {
        if (dir_or_link(s, "enclosure_device")) {
            cardType = ENUM_CARDTYPE::HBA9500;
            enclosure_device.name = s->d_name;
            enclosure_device.ft = FT_CHAR;  /* dummy */
            enclosure_device.d_type = s->d_type;
            return 0;
        }
        return 1;
    }
    return 0;
}

/* Return true for directory entry that is link or directory (other than a
 * directory name starting with dot) that contains "enclosure", not "enclosure_device".
 * Else return false.  */
static bool
enclosure_dir_scan(const char * dir_name)
{
    int num, k;
    struct dirent ** namelist;

    num = scandir(dir_name, &namelist, enclosure_dir_scan_select, NULL);
    if (num < 0) {
        snprintf(errpath, LMAX_PATH, "%s: scandir: %s", __func__, dir_name);
        perror(errpath);
        return false;
    }
    for (k = 0; k < num; ++k)
        free(namelist[k]);
    free(namelist);
    return !! num;
}

/* If 'dir_name'/'base_name' is a directory chdir to it. If that is successful
   return true, else false */
static bool
if_directory_chdir(const char * dir_name, const char * base_name)
{
    char b[LMAX_PATH];
    struct stat a_stat;

    snprintf(b, sizeof(b), "%s/%s", dir_name, base_name);
    if (stat(b, &a_stat) < 0)
        return false;
    if (S_ISDIR(a_stat.st_mode)) {
        if (chdir(b) < 0)
            return false;
        return true;
    }
    return false;
}

/* If 'dir_name'/'base_name' is found places corresponding value in 'value'
 * and returns true . Else returns false.
 */
static bool
get_value(QString dir_name, QString base_name, char * value, int max_value_len)
{
    int len;
    FILE * f;
    QString b = dir_name + "/" + base_name;

    if (NULL == (f = fopen(b.toStdString().c_str(), "r"))) {
        return false;
    }

    if (NULL == fgets(value, max_value_len, f)) {
        /* assume empty */
        value[0] = '\0';
        fclose(f);
        return true;
    }

    len = strlen(value);
    if ((len > 0) && (value[len - 1] == '\n')) {
        value[len - 1] = '\0';
    }

    fclose(f);
    return true;
}

static uint64_t
expander_wwid(QString dir_name, QString dev_name, int vb)
{
    int vlen;
    char value[LMAX_NAME];
    QString wd = QString("%1/%2").arg(dir_name, dev_name);

    vlen = sizeof(value);
    if (get_value(wd, "sas_address", value, vlen)) {
        if (vb) {
            qDebug("Found an expander sas address: %s", value);
        }
        int len = strlen(value);
        if (len >= 16) len -= 16;   // wwid is 16-digit long
        /**
         * The lowest 6 bits of the expander SAS address must be set to 0x1
         */
        return QString(value + len).toULong(0, 16) | 0x3F;
    }
    return 0;
}

bool
get_myValue(QString dir_name, QString name, QString& myvalue)
{
    int vlen;
    char value[LMAX_NAME];

    vlen = sizeof(value);
    if (get_value(dir_name, name, value, vlen)) {
        myvalue = value;
        return true;
    }
    return false;
}

QString
get_blockname(QString dir_name)
{
    if (1 == scan_for_first(dir_name.toStdString().c_str()))
        return aa_first.name;
    else {
        qDebug("unexpected scan_for_first error");
        return "";
    }
}

/* This is a function to determine the distance between device and the expander */
int
compute_device_index(const char * device, const char * expander)
{
    struct addr_hctl dev_hctl;
    struct addr_hctl exp_hctl;

    if (! parse_colon_list(device, &dev_hctl)) {
        return -1;
    }
    if (! parse_colon_list(expander, &exp_hctl)) {
        return -1;
    }
    if (dev_hctl.h != exp_hctl.h || dev_hctl.c != exp_hctl.c) {
        return -1;
    }
    return exp_hctl.t - dev_hctl.t;
}

// Struct to hold drive info
struct DriveInfo {
    int eid;
    int slot;
    QString serialNumber;
};

/* Routine 1: Run storcli command for all controllers and get raw drive blocks */
QStringList getRawDriveBlocks()
{
    QString command = "sudo /opt/MegaRAID/storcli/storcli64 /call /eall /sall show all";

    QProcess process;
    process.start("bash", QStringList() << "-c" << command);
    process.waitForFinished();

    QString output = process.readAllStandardOutput();
    QString error = process.readAllStandardError();

    if (!error.isEmpty()) {
        qDebug() << "Command Error:" << error;
    }

    // Extract each drive block from the raw output
    QRegularExpression driveStartRegex(R"(Drive\s+\/c\d+\/e\d+\/s\d+\s+:)");
    QRegularExpressionMatchIterator iterator = driveStartRegex.globalMatch(output);

    QList<int> positions;
    while (iterator.hasNext()) {
        QRegularExpressionMatch match = iterator.next();
        positions.append(match.capturedStart());
    }

    QStringList driveBlocks;
    for (int i = 0; i < positions.size(); ++i) {
        int start = positions[i];
        int end = (i + 1 < positions.size()) ? positions[i + 1] : output.length();
        QString block = output.mid(start, end - start).trimmed();
        if (!block.isEmpty()) {
            driveBlocks.append(block);
        }
    }

    return driveBlocks;
}

/* Routine 2: Parse a single drive block */
DriveInfo parseDriveBlock(const QString& block) {
    DriveInfo drive;

    // Extract EID and Slot
    QRegularExpression eidSltRegex(R"(Drive\s+\/c\d+\/e(\d+)\/s(\d+)\s+:)");
    QRegularExpressionMatch match = eidSltRegex.match(block);
    if (match.hasMatch()) {
        drive.eid = match.captured(1).toInt();
        drive.slot = match.captured(2).toInt();
    }

    // Extract Serial Number
    QRegularExpression snRegex(R"(SN\s+=\s+(\S+))");
    QRegularExpressionMatch snMatch = snRegex.match(block);
    if (snMatch.hasMatch()) {
        drive.serialNumber = snMatch.captured(1);
    } else {
        drive.serialNumber = "Unknown";
    }

    return drive;
}

/* Routine 3: Full processing to get structured vector */
QVector<DriveInfo> getAllControllerDriveInfo()
{
    QStringList driveBlocks = getRawDriveBlocks();
    QVector<DriveInfo> driveList;

    for (const QString& block : driveBlocks) {
        driveList.append(parseDriveBlock(block));
    }

    return driveList;
}

// Struct to hold disk information
struct DiskInfo {
    QString name;
    QString wwn;
    QString serial;
    QString model;
};

QVector<DiskInfo> getRealHddInfo()
{
    QString command = "lsblk -o NAME,WWN,SERIAL,MODEL -n";
    QProcess process;

    process.start("bash", QStringList() << "-c" << command);
    process.waitForFinished();

    QString output = process.readAllStandardOutput();
    QString error = process.readAllStandardError();

    if (!error.isEmpty()) {
        qDebug() << "Command Error:\n" << error;
    }

    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    QVector<DiskInfo> diskList;

    for (const QString &line : lines) {
        QStringList tokens = line.simplified().split(' '); // Clean and split

        if (tokens.size() >= 4) {
            DiskInfo disk;
            disk.name = tokens[0];
            disk.wwn = tokens[1];
            disk.serial = tokens[2];
            disk.model = tokens[3];

            // Skip virtual disks, loops, CD-ROMs, and optionally NVMe
            if (disk.name.startsWith("loop")) continue;
            if (disk.name.startsWith("sr")) continue;
            if (disk.serial.startsWith("Virtual")) continue;
            if (disk.model.contains("Virtual", Qt::CaseInsensitive)) continue;
            // Optional: Skip NVMe SSDs
            if (disk.name.startsWith("nvme")) continue;

            // Keep real HDDs only
            diskList.append(disk);
        }
    }

    return diskList;
}

/* Utility function: returns pointer or nullptr if not found */
DiskInfo* findDiskBySerial(QVector<DiskInfo>& disks, const QString& serial) {
    for (DiskInfo& disk : disks) {
        if (disk.serial == serial) {
            return &disk; // Return pointer to the found disk
        }
    }
    return nullptr; // Not found
}

/* List SCSI devices (LUs). */
void
list_sdevices(int vb)
{
    int num, k, prev;
    struct dirent ** namelist;
    QString buff, name;

    if (vb) {
        qDebug("listing...");
    }

    buff = QString(sysfsroot) + bus_scsi_devs;

    num = scandir(buff.toStdString().c_str(), &namelist, sdev_dir_scan_select, sdev_scandir_sort);
    if (num < 0) {  /* scsi mid level may not be loaded */
        name = QString("%1: scandir: %2").arg(__func__, buff);
        perror(name.toStdString().c_str());
        gAppendMessage("SCSI mid level module may not be loaded.");
        return;
    }

    for (prev = k = 0; k < num; ++k) {
        name = namelist[k]->d_name;
        QString dir_name = QString("%1/%2").arg(buff, name);
        if (enclosure_dir_scan(dir_name.toStdString().c_str())) {
            uint64_t wwid = expander_wwid(buff, name, vb);
            if (0 == wwid) {
                gAppendMessage(QString("error: cannot get expander[%1] wwid!").arg(name));
            } else {
                if (cardType == ENUM_CARDTYPE::HBA9600) {
                    for (; prev < k; ++prev) {
                        gDevices.setSlot(buff, namelist[prev]->d_name, namelist[k]->d_name, wwid);
                    }
                    prev = k + 1;
                }
                gControllers.setController(namelist[k]->d_name, wwid);
            }
        } else if (cardType == ENUM_CARDTYPE::HBA9500) {
            /* HBA9500 disk has enclosure_device:ArrayDevicexx, whereas HBA9600 disk has not */
            gDevices.setSlot(buff, name, enclosure_device.name);
        }
    }

    // If no devices found by methods of HBA9500/HBA9600, possibly a RAID card is present
    if (gControllers.count() == 0 && gDevices.count() == 0) {
        cardType = ENUM_CARDTYPE::RAID9x60;

        QVector<DriveInfo> drives = getAllControllerDriveInfo();
        if (vb) {
            qDebug("re-enumerate SCSI devices as RAID9x60 cards assumed present");
            for (const DriveInfo& drive : drives) {
                qDebug() << QString("Drive: %1:%2, SN: %3").arg(drive.eid).arg(drive.slot).arg(drive.serialNumber);
            }
        }

        QVector<DiskInfo> disks = getRealHddInfo();
        if (vb) {
            qDebug() << "Detected Real HDDs:";
            for (const DiskInfo &disk : disks) {
                qDebug() << "Name:" << disk.name << "WWN:" << disk.wwn << "SN:" << disk.serial << "Model:" << disk.model;
            }
        }

        for (const DriveInfo& drive : drives) {
            DiskInfo* disk = findDiskBySerial(disks, drive.serialNumber);
            if (disk) {
                gDevices.setSlot(drive.slot, QString("[%1:%2]").arg(drive.eid).arg(drive.slot), disk->wwn, disk->name);
            } else {
                gAppendMessage(QString("Drive %1:%2 with SN %3 not found in real HDDs!").arg(drive.eid).arg(drive.slot).arg(drive.serialNumber));
            }
        }

        // If still no devices found by methods of a RAID card
        if (gDevices.count() == 0) {
            cardType = ENUM_CARDTYPE::UNKNOWN;
        } else {
            // Set dummy path for RAID controllers
            const uint8_t raid_ids[] = {0x3F, 0x7F, 0xBF, 0xFF};
            for (uint8_t id : raid_ids) {
                gControllers.setBsgPath("dummy path for raid", id);
            }
        }
    }

    for (k = 0; k < num; ++k) {
        free(namelist[k]);
    }
    free(namelist);
}
