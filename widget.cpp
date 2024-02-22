#include <QFile>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QSystemTrayIcon>
#include <QTimer>

#include "ui_widget.h"
#include "widget.h"
#include "lsscsi.h"
#include "smp_lib.h"
#include "smp_discover.h"
#include "mpi3mr_app.h"

#define WWID_TO_INDEX(wwid) ((wwid & 0xFF) >> 6)

extern int verbose;

static QTabWidget * gTab = nullptr;
static QComboBox * gCombo = nullptr;
static QTextBrowser * gText = nullptr;

DeviceFunc gDevices;
ExpanderFunc gControllers;

void DeviceFunc::clear()
{
    for (int i = 0; i < NSLOT; i++) {
        clrSlot(i);
    }
    myCount = 0;
}

void DeviceFunc::clrSlot(int sl)
{
    // validate the index passed
    if (sl == valiIndex(sl)) {

        // clear slot stuff
        SlotInfo[sl].cb_slot->setText(QString("Slot %1").arg(sl+1));
        SlotInfo[sl].cb_slot->setStyleSheet("QCheckBox:enabled{color: black;} QCheckBox:disabled{color: grey;}");
        SlotInfo[sl].cb_slot->setEnabled(false);
        SlotInfo[sl].cb_slot->setCheckState(Qt::CheckState::Unchecked);
        SlotInfo[sl].d_name.clear();
        SlotInfo[sl].wwid.clear();
        SlotInfo[sl].block.clear();
        SlotInfo[sl].resp_len = 0;

        // decrement the slot count
        myCount--;
    }
}

void DeviceFunc::setSlot(QString dir_name, QString device, int sl)
{
    // validate the index passed
    if (sl == valiIndex(sl)) {

        // Set slot occupied by something
        SlotInfo[sl].d_name = device;

        // Get wwid of this device
        QString wd = dir_name.append("/%1").arg(device);
        if (false == get_myValue(wd, "wwid", SlotInfo[sl].wwid)) {
            SlotInfo[sl].wwid.clear();
        }

        // Get block name of this device
        wd += "/block";
        SlotInfo[sl].block = get_blockname(wd);

        SlotInfo[sl].cb_slot->setEnabled(true);
        setSlotLabel(sl);
        myCount++;
    }
}

void DeviceFunc::setSlot(QString dir_name, QString device, QString expander, uint64_t wwid)
{
    int sl = compute_device_index(device.toStdString().c_str(), expander.toStdString().c_str());

    // the device should be within this expander's domain
    if (sl <= 0 || sl > NSLOT_PEREXP) {
        qDebug() << "Device [" << device << "] setting error!";
        return;
    }
    sl = (WWID_TO_INDEX(wwid) + 1) * NSLOT_PEREXP - sl;
    setSlot(dir_name, device, sl);
}

void DeviceFunc::setSlot(QString dir_name, QString device, QString enclosure_device_name)
{
    setSlot(dir_name, device, enclosure_device_name.right(2).toShort(0, 16) - 1);
}

void DeviceFunc::setDiscoverResp(int dsn, uchar * src, int len)
{
    int sl = dsn - 1;
    // validate the converted index
    if (sl == valiIndex(sl)) {

        // check NEGOTIATED LOGICAL LINK RATE
        if (len > 13) {
            int negot = src[13] & 0xf;
            if (negot == 1) {
                // SCSI driver lags refreshing device info.
                if (false == slotVacant(sl)) {
                    clrSlot(sl);
                }
                QString title = SlotInfo[sl].cb_slot->text();
                SlotInfo[sl].cb_slot->setText(title.append(" (phy off)"));
                SlotInfo[sl].cb_slot->setStyleSheet("QCheckBox:enabled{color: red;} QCheckBox:disabled{color: grey;}");
            }
            /* attached SAS device type: 0-> none, 1-> (SAS or SATA end) device,
             * 2-> expander, 3-> fanout expander (obsolete), rest-> reserved */
            int adt = ((0x70 & src[12]) >> 4);
            if (0 == adt && false == slotVacant(sl)) {
                gAppendMessage(QString::asprintf("[%s] slot %d setting error!", __func__, dsn));
            }
        }

        // src area is bigger than discover_resp and zero set before discovery
        memcpy(SlotInfo[sl].discover_resp, src, SMP_FN_DISCOVER_RESP_LEN);
        SlotInfo[sl].resp_len = len;
        if (nullptr != gTab && ENUM_TAB::FIO != gTab->currentIndex()) {
            SlotInfo[sl].cb_slot->setEnabled(true);
        }

        // Re-set label for (SSP, SATA) appendix
        if ((nullptr != gCombo) && (false == slotVacant(sl)) && (ENUM_COMBO::SDx == gCombo->currentIndex())) {
            setSlotLabel(sl);
        }
    }
}

void DeviceFunc::setSlotLabel(int sl)
{
    if ((nullptr != gCombo) && (sl == valiIndex(sl)) && (false == slotVacant(sl))) {

        switch (gCombo->currentIndex())
        {
        case ENUM_COMBO::WWID:
            SlotInfo[sl].cb_slot->setText(SlotInfo[sl].wwid.right(16));
            break;
        case ENUM_COMBO::SDx:
        {
            QString target;
            int prot = SlotInfo[sl].discover_resp[15];
            if ((SlotInfo[sl].resp_len > 15) && (prot & 0xf)) {
                if (prot & 0x8) target = " (SSP)";
                if (prot & 0x4) target = " (STP)";
                if (prot & 0x2) target = " (SMP)";
                if (prot & 0x1) target = " (SATA)";
            }
            SlotInfo[sl].cb_slot->setText(QString("%1. ").arg(sl+1) + SlotInfo[sl].block + target);
            break;
        }
        default:
            SlotInfo[sl].cb_slot->setText(QString("%1. ").arg(sl+1) + SlotInfo[sl].d_name);
            break;
        }
    }
}

void ExpanderFunc::clear()
{
    for (int i = 0; i < NEXPDR; i++) {
        GboxInfo[i].gbox->setTitle(QString("Expander-%1").arg(i+1));
        GboxInfo[i].d_name.clear();
        GboxInfo[i].bsg_path.clear();
        GboxInfo[i].ioc_num = 0;;
        GboxInfo[i].wwid64 = 0;;
        GboxInfo[i].resp_len = 0;
    }
    myCount = 0;
}

void ExpanderFunc::setController(QString expander, uint64_t wwid)
{
    // Only expanders 0-3 should be taken care of...
    int el = WWID_TO_INDEX(wwid);

    GboxInfo[el].d_name = expander;
    GboxInfo[el].wwid64 = wwid;

    QString title = GboxInfo[el].gbox->title();
    GboxInfo[el].gbox->setTitle(title + QString::asprintf(" [%lX]", wwid));

    myCount++;
}

void ExpanderFunc::setDiscoverResp(QString path, int subvalue, uint64_t ull, uint64_t sa, uchar * src, int len)
{
    int el = WWID_TO_INDEX(ull);

    // save the working path for later use cases
    GboxInfo[el].bsg_path = path;

    // save the IOC number for the very Controller's ID
    GboxInfo[el].ioc_num = subvalue;

    // src area is bigger than discover_resp and zero set before discovery
    memcpy(GboxInfo[el].discover_resp, src, SMP_FN_DISCOVER_RESP_LEN);
    GboxInfo[el].resp_len = len;

    const char* cp = "";
    if (len > 13) {
        int negot = src[13] & 0xf;
        if (negot == 0x8) cp = "1.5";
        if (negot == 0x9) cp = "3";
        if (negot == 0xa) cp = "6";
        if (negot == 0xb) cp = "12";
        if (negot == 0xc) cp = "22.5";
    }

    QString title = GboxInfo[el].gbox->title();
    GboxInfo[el].gbox->setTitle(
        title.append(QString::asprintf(" [HBA:%lX/%s Gbps]", sa, cp)));
}

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{
    // Only when memory is allocated can the label be used,
    // so the ui->setupUi(this) must be placed at the beginning of the function.
    ui->setupUi(this);

    ui->textBrowser->clear();  // Clear the default ui text

    QVBoxLayout *layout = new QVBoxLayout;
    layout->addWidget(ui->tabWidget);
    layout->addWidget(ui->groupBox_0);
    layout->addWidget(ui->groupBox_1);
    layout->addWidget(ui->groupBox_2);
    layout->addWidget(ui->groupBox_3);
    //layout->addStretch();
    layout->addWidget(ui->textBrowser);
    setLayout(layout);

    // Use a temporary QCheckBox array _slot to assign all QCheckbox vector to the global gSlot
    QCheckBox *_slot[NSLOT] = {
        ui->checkBox_1,   ui->checkBox_2,   ui->checkBox_3,   ui->checkBox_4,   ui->checkBox_5,   ui->checkBox_6,   ui->checkBox_7,
        ui->checkBox_8,   ui->checkBox_9,   ui->checkBox_10,  ui->checkBox_11,  ui->checkBox_12,  ui->checkBox_13,  ui->checkBox_14,
        ui->checkBox_15,  ui->checkBox_16,  ui->checkBox_17,  ui->checkBox_18,  ui->checkBox_19,  ui->checkBox_20,  ui->checkBox_21,
        ui->checkBox_22,  ui->checkBox_23,  ui->checkBox_24,  ui->checkBox_25,  ui->checkBox_26,  ui->checkBox_27,  ui->checkBox_28,
        ui->checkBox_29,  ui->checkBox_30,  ui->checkBox_31,  ui->checkBox_32,  ui->checkBox_33,  ui->checkBox_34,  ui->checkBox_35,
        ui->checkBox_36,  ui->checkBox_37,  ui->checkBox_38,  ui->checkBox_39,  ui->checkBox_40,  ui->checkBox_41,  ui->checkBox_42,
        ui->checkBox_43,  ui->checkBox_44,  ui->checkBox_45,  ui->checkBox_46,  ui->checkBox_47,  ui->checkBox_48,  ui->checkBox_49,
        ui->checkBox_50,  ui->checkBox_51,  ui->checkBox_52,  ui->checkBox_53,  ui->checkBox_54,  ui->checkBox_55,  ui->checkBox_56,
        ui->checkBox_57,  ui->checkBox_58,  ui->checkBox_59,  ui->checkBox_60,  ui->checkBox_61,  ui->checkBox_62,  ui->checkBox_63,
        ui->checkBox_64,  ui->checkBox_65,  ui->checkBox_66,  ui->checkBox_67,  ui->checkBox_68,  ui->checkBox_69,  ui->checkBox_70,
        ui->checkBox_71,  ui->checkBox_72,  ui->checkBox_73,  ui->checkBox_74,  ui->checkBox_75,  ui->checkBox_76,  ui->checkBox_77,
        ui->checkBox_78,  ui->checkBox_79,  ui->checkBox_80,  ui->checkBox_81,  ui->checkBox_82,  ui->checkBox_83,  ui->checkBox_84,
        ui->checkBox_85,  ui->checkBox_86,  ui->checkBox_87,  ui->checkBox_88,  ui->checkBox_89,  ui->checkBox_90,  ui->checkBox_91,
        ui->checkBox_92,  ui->checkBox_93,  ui->checkBox_94,  ui->checkBox_95,  ui->checkBox_96,  ui->checkBox_97,  ui->checkBox_98,
        ui->checkBox_99,  ui->checkBox_100, ui->checkBox_101, ui->checkBox_102, ui->checkBox_103, ui->checkBox_104, ui->checkBox_105,
        ui->checkBox_106, ui->checkBox_107, ui->checkBox_108, ui->checkBox_109, ui->checkBox_110, ui->checkBox_111, ui->checkBox_112
    };
    for (int i = 0; i < NSLOT; i++) {
        gDevices.cbSlot(i) = _slot[i];
    }

    // Setup Groubox 0-3 to be globally accessed
    gControllers.gbThe(0) = ui->groupBox_0;
    gControllers.gbThe(1) = ui->groupBox_1;
    gControllers.gbThe(2) = ui->groupBox_2;
    gControllers.gbThe(3) = ui->groupBox_3;

    // Connect Widget signals to the related slots
    connect(ui->cbxSlot, &QComboBox::currentIndexChanged, this, &Widget::cbxSlotIndexChanged);
    connect(ui->btnRefresh, &QPushButton::clicked, this, &Widget::btnRefreshClicked);
    connect(ui->btnListSdx, &QPushButton::clicked, this, &Widget::btnListSdxClicked);
    connect(ui->btnSmpDoit, &QPushButton::clicked, this, &Widget::btnSmpDoitClicked);
    connect(ui->btnClearTB, &QPushButton::clicked, this, &Widget::btnClearTBClicked);
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &Widget::tabSelected);

    m_Watcher = new QFileSystemWatcher();
    if (false == m_Watcher->addPath(dev_bsg)) {
        qDebug() << "QFileSystemWatcher: failed to add path";
    }
    connect(m_Watcher, &QFileSystemWatcher::directoryChanged, this, &Widget::showModified);

    // Configure for systray icon
    QIcon icon = QIcon(":/arrows.png");
    QSystemTrayIcon *trayIcon = new QSystemTrayIcon(this);
    trayIcon->setIcon(icon);
    trayIcon->show();
    setWindowIcon(icon);

    // Init global ui controls handy for manipulation
    gTab = ui->tabWidget;
    gCombo = ui->cbxSlot;
    gText = ui->textBrowser;

    ui->radDiscover->hide();    // temporarily hide for release

    appendMessage("Here lists the messages:");
    filloutCanvas();
}

Widget::~Widget()
{
    delete ui;
    delete m_Watcher;
}

void Widget::appendMessage(QString message)
{
    ui->textBrowser->append(message);
}

void Widget::showModified(const QString & path)
{
    appendMessage("Slots information should refresh due to being modified");
}

void Widget::cbxSlotIndexChanged(int index)
{
    for (int i=0; i<NSLOT; i++) {
        gDevices.setSlotLabel(i);
    }
}

void Widget::btnRefreshClicked()
{
    appendMessage("Refresh slots information...");

    filloutCanvas();
    appendMessage(QString::asprintf("Found %d expanders and %d devices", gControllers.count(), gDevices.count()));
}

void Widget::sdxlist_sit(QTextStream & stream)
{
    // loop through the expanders discovered
    for (int k = 0; k < 4; ++k) {
        stream << "Expander-" << k+1 << Qt::endl;
        if (false == gControllers.bsgPath(k).isEmpty()) {
            // bool value to determine if a colon to follow
            bool listed = false;
            // format 1: loop through the slots numbered
            for (int i = k*NSLOT_PEREXP; i < (k+1)*NSLOT_PEREXP; ++i) {
                // check if the slot is occupied?
                if (false == gDevices.slotVacant(i)) {
                    if (listed) {
                        stream << ":";
                    }
                    stream << "/dev/" << gDevices.block(i);
                    listed = true;
                }
            }
            if (listed) {
                stream << Qt::endl;
            }
            // format 2: loop through the slots numbered
            for (int i = k*NSLOT_PEREXP; i < (k+1)*NSLOT_PEREXP; ++i) {
                // check if the slot is occupied?
                if (false == gDevices.slotVacant(i)) {
                    stream << "/dev/" << gDevices.block(i) << Qt::endl;
                }
            }
        }
    }
}

void Widget::sdxlist_wl1(QTextStream & stream)
{
    stream << "[global]"        << Qt::endl
           << "bs=4K"           << Qt::endl
           << "#numjobs=1"      << Qt::endl
           << "iodepth=8"       << Qt::endl
           << "direct=1"        << Qt::endl
           << "ioengine=libaio" << Qt::endl
           << "#group_reporting=0" << Qt::endl
           << "time_based"      << Qt::endl
           << "ramp_time=30"    << Qt::endl
           << "runtime=120"     << Qt::endl
           << "name=Workload 1" << Qt::endl
           << "rw=randread"     << Qt::endl << Qt::endl;

    int jobn = 0;

    // loop through the expanders discovered
    for (int k = 0; k < 4; ++k) {
        stream << "## Expander-" << k+1 << Qt::endl;
        if (false == gControllers.bsgPath(k).isEmpty()) {
            // Workload 1
            for (int i = k*NSLOT_PEREXP; i < (k+1)*NSLOT_PEREXP; ++i) {
                // check if this slot is occupied?
                if (false == gDevices.slotVacant(i)) {
                    stream << "[job" << ++jobn << "]" << Qt::endl
                           << "filename=/dev/" << gDevices.block(i) << Qt::endl;
                    // check if this slot is the target?
                    if (gDevices.cbSlot(i)->isChecked()) {
                        stream << "bs=512k" << Qt::endl
                               << "rw=write" << Qt::endl;
                    }
                    stream << Qt::endl;
                }
            }
        }
    }
}

void Widget::sdxlist_wl2(QTextStream & stream)
{
    stream << "[global]"        << Qt::endl
           << "bs=4K"           << Qt::endl
           << "#numjobs=1"      << Qt::endl
           << "iodepth=8"       << Qt::endl
           << "direct=1"        << Qt::endl
           << "ioengine=libaio" << Qt::endl
           << "#group_reporting=0" << Qt::endl
           << "time_based"      << Qt::endl
           << "ramp_time=30"    << Qt::endl
           << "runtime=120"     << Qt::endl
           << "name=Workload 2" << Qt::endl
           << "rw=randread"     << Qt::endl << Qt::endl;

    int jobn = 0;

    // loop through the expanders discovered
    for (int k = 0; k < 4; ++k) {
        stream << "## Expander-" << k+1 << Qt::endl;
        if (false == gControllers.bsgPath(k).isEmpty()) {
            // Workload 2
            for (int i = k*NSLOT_PEREXP; i < (k+1)*NSLOT_PEREXP; ++i) {
                // check if this slot is occupied?
                if (false == gDevices.slotVacant(i)) {
                    stream << "[job" << ++jobn << "]" << Qt::endl
                           << "filename=/dev/" << gDevices.block(i) << Qt::endl;
                    // check if this slot is the target?
                    if (gDevices.cbSlot(i)->isChecked()) {
                        stream << "bs=4k" << Qt::endl
                               << "rw=randwrite" << Qt::endl;
                    }
                    stream << Qt::endl;
                }
            }
        }
    }
}

void Widget::btnListSdxClicked()
{
    const char * SDX_LIST_FILE[] = { "Dino_sdx_list.txt", "512k_SeqW_4k_RandR.fio", "4k_RandW_4k_RandR.fio" };
    void (Widget::*do_list[])(QTextStream & stream) = { &Widget::sdxlist_sit, &Widget::sdxlist_wl1, &Widget::sdxlist_wl2 };

    int choice = 0;
    if (ui->tabWidget->currentIndex() == ENUM_TAB::FIO) {
        if (ui->radWl1->isChecked()) {
            choice = 1;
        }
        if (ui->radWl2->isChecked()) {
            choice = 2;
        }
    }

    QMessageBox *msgBox = new QMessageBox(this);
    msgBox->setIconPixmap(QPixmap(":/listsdx_48.png"));
    msgBox->setText("SDx Listing");
    msgBox->setWindowModality(Qt::NonModal);
    msgBox->setInformativeText(QString::asprintf("Please check file: %s", SDX_LIST_FILE[choice]));
    msgBox->setStandardButtons(QMessageBox::Ok);

    QFile file(SDX_LIST_FILE[choice]);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {

        // We're going to streaming text to the file
        QTextStream stream(&file);

        // Do the listing
        (this->*do_list[choice])(stream);

        // Close the output file
        file.close();
    }

    msgBox->exec();
}

void Widget::btnClearTBClicked()
{
    ui->textBrowser->clear();  // Clear the default ui text
}

void Widget::btnSmpDoitClicked()
{
    int delay = 0;
    if (ui->radPhyDisable->isChecked()) {
        appendMessage("Disable phys...");
        if (phySetDisabled(true))
            delay = 4000;
    }
    else if (ui->radPhyReset->isChecked()) {
        appendMessage("Enable phys...");
        if (phySetDisabled(false))
            delay = 2500;
    }
    else if (ui->radDiscover->isChecked()) {
        appendMessage("Discover expanders...");
        mpi3mr_discover(verbose);
        return;
    }
#if 1
    Q_UNUSED(delay);
#else
    // Run a function with a delay in QT
    if (delay) {
        QEventLoop loop;
        QTimer::singleShot(delay, &loop, &QEventLoop::quit);
        loop.exec();
        refreshSlots();
    }
#endif
}

void Widget::tabSelected()
{
    static int lastIndex = 0;
    int currIndex = ui->tabWidget->currentIndex();
    switch (currIndex)
    {
    case ENUM_TAB::SMP:
        if (lastIndex == ENUM_TAB::FIO) {
            for (int i = 0; i < NSLOT; i++) {
                if (true == gDevices.slotVacant(i) && gDevices.slotPhyId(i) > 0) {
                    gDevices.cbSlot(i)->setEnabled(true);
                }
            }
        }
        // Save it!
        lastIndex = currIndex;
        break;
    case ENUM_TAB::FIO:
        ui->radSit->setChecked(true);
        for (int i = 0; i < NSLOT; i++) {
            if (true == gDevices.slotVacant(i) && gDevices.slotPhyId(i) > 0) {
                gDevices.cbSlot(i)->setEnabled(false);
            }
        }
        // Save it!
        lastIndex = currIndex;
        break;
    case ENUM_TAB::Info:
        ui->textInfo->clear();
        if (hba9500) {
            ui->textInfo->append("HBA is 9500");
            break;
        }
        mpi3mr_iocfacts(verbose);
        ui->textInfo->append(get_infofacts());
        break;
    }
}

void Widget::filloutCanvas()
{
    gDevices.clear();
    gControllers.clear();
    list_sdevices(verbose);
    hba9500 ? slot_discover(verbose) : mpi3mr_slot_discover(verbose);
}

int Widget::phySetDisabled(bool disable)
{
    smp_target_obj tobj;
    int k, i, ret = 0;

    // loop through the expanders discovered
    for (k = 0; k < 4; ++k) {
        if (false == gControllers.bsgPath(k).isEmpty()) {
            if (verbose) {
                qDebug() << "----> phy controlling " << gControllers.bsgPath(k);
            }
            tobj.opened = 0;
            // loop through the slots listed
            for (i = k*NSLOT_PEREXP; i < (k+1)*NSLOT_PEREXP; ++i) {
                // check if the slot is selected or not?
                if (gDevices.cbSlot(i)->isChecked()) {
                    // the expander is to be opened for the 1st selected slot
                    if (0 == tobj.opened) {
                        IntfEnum sel = hba9500 ? I_SGV4 : I_SGV4_MPI;
                        // assign the IOC number for multiple adapters case
                        int res = smp_initiator_open(gControllers.bsgPath(k), gControllers.subvalue(k), sel, &tobj, verbose);
                        if (res < 0) {
                            break;
                        }
                        // signal Delay after function return
                        ret = 1;
                    }
                    // check if a resonable phy id (4 - 31)
                    int phy_id = gDevices.slotPhyId(i);
                    if (phy_id > 3 && phy_id < 32) {
                        // assign sas address for path-through
                        tobj.sas_addr64 = gControllers.wwid64(k);
                        // to issue PHY CONTROL request
                        phy_control(&tobj, phy_id, disable, verbose);
                    } else {
                        gAppendMessage(QString::asprintf("found a phy id(%d) illegal on slot #%d", phy_id, i + 1));
                    }
                    gDevices.cbSlot(i)->setCheckState(Qt::CheckState::Unchecked);
                }
            }
            // check if close the opened expander
            if (tobj.opened) {
                smp_initiator_close(&tobj);
            }
        }
    }
    return ret;
}

void gAppendMessage(QString message)
{
    if (gText) gText->append(message);
}
