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

static QComboBox *gCombo = nullptr;
static QTextBrowser *gText = nullptr;

DeviceFunc gDevices;
ExpanderFunc gControllers;

void DeviceFunc::clear()
{
    for (int i = 0; i < NSLOT; i++) {
        SlotInfo[i].cb_slot->setText(QString("Slot %1").arg(i+1));
        SlotInfo[i].cb_slot->setStyleSheet("QCheckBox:enabled{color: black;} QCheckBox:disabled{color: grey;}");
        SlotInfo[i].cb_slot->setDisabled(true);
        SlotInfo[i].cb_slot->setCheckState(Qt::CheckState::Unchecked);
        SlotInfo[i].d_name.clear();
        SlotInfo[i].wwid.clear();
        SlotInfo[i].block.clear();
        SlotInfo[i].resp_len = 0;
    }
    myCount = 0;
}

void DeviceFunc::setSlot(QString dir_name, QString device, int sl)
{
    if ((unsigned)sl < NSLOT) {
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
    if (sl <= 0 || sl > 28) {
        qDebug() << "Device [" << device << "] setting error!";
        return;
    }
    sl = (WWID_TO_INDEX(wwid) + 1) * 28 - sl;
    setSlot(dir_name, device, sl);
}

void DeviceFunc::setSlot(QString dir_name, QString device, QString enclosure_device_name)
{
    setSlot(dir_name, device, enclosure_device_name.right(2).toShort(0, 16) - 1);
}

void DeviceFunc::setDiscoverResp(int dsn, uchar * src, int len)
{
    if (0 != dsn && (unsigned)dsn <= NSLOT) {
        int sl = dsn - 1;
        // src area is bigger than discover_resp and zero set before discovery
        memcpy(SlotInfo[sl].discover_resp, src, SMP_FN_DISCOVER_RESP_LEN);
        SlotInfo[sl].resp_len = len;
        SlotInfo[sl].cb_slot->setEnabled(true);
        if (len > 0) {
            int negot = src[13] & 0xf;
            if (negot == 1) {
                QString title = SlotInfo[sl].cb_slot->text();
                SlotInfo[sl].cb_slot->setText(title.append(" (phy off)"));
            }
            /* attached SAS device type: 0-> none, 1-> (SAS or SATA end) device,
             * 2-> expander, 3-> fanout expander (obsolete), rest-> reserved */
            int adt = ((0x70 & src[12]) >> 4);
            if (0 == adt && !SlotInfo[sl].d_name.isEmpty())
                gAppendMessage(QString::asprintf("[%s] slot %d setting error!", __func__, dsn));
        }
    } else {
        qDebug("[%s] incorrect DSN numbering:%d", __func__, dsn);
    }
}

void DeviceFunc::setSlotLabel(int sl)
{
    if (gCombo && !SlotInfo[sl].d_name.isEmpty())
    switch (gCombo->currentIndex())
    {
    case 0:
        SlotInfo[sl].cb_slot->setText(SlotInfo[sl].wwid.right(16));
        break;
    case 1:
        SlotInfo[sl].cb_slot->setText(QString("%1. ").arg(sl+1) + SlotInfo[sl].block);
        break;
    default:
        SlotInfo[sl].cb_slot->setText(QString("%1. ").arg(sl+1) + SlotInfo[sl].d_name);
        break;
    }
}

void ExpanderFunc::clear()
{
    for (int i = 0; i < NEXPDR; i++) {
        GboxInfo[i].gbox->setTitle(QString("Expander-%1").arg(i+1));
        GboxInfo[i].d_name.clear();
        GboxInfo[i].wwid64 = 0;;
        GboxInfo[i].bsg_path.clear();
        GboxInfo[i].resp_len = 0;
    }
    myCount = 0;
}

void ExpanderFunc::setController(QString expander, uint64_t wwid)
{
    // Only expanders 0-3 should be taken care of...
    int ie = WWID_TO_INDEX(wwid);

    GboxInfo[ie].d_name = expander;
    GboxInfo[ie].wwid64 = wwid;

    QString title = GboxInfo[ie].gbox->title();
    GboxInfo[ie].gbox->setTitle(title + QString::asprintf(" [%lX]", wwid));

    myCount++;
}

void ExpanderFunc::setDiscoverResp(QString path, uint64_t ull, uint64_t sa, uchar * src, int len)
{
    int el = WWID_TO_INDEX(ull);
    // save the working path for later use cases
    GboxInfo[el].bsg_path = path;

    // src area is bigger than discover_resp and zero set before discovery
    memcpy(GboxInfo[el].discover_resp, src, SMP_FN_DISCOVER_RESP_LEN);
    GboxInfo[el].resp_len = len;
    if (len > 0) {
        int negot = src[13] & 0xf;
        const char* cp = "";
        switch(negot) {
        case 8:
                cp = "1.5";
                break;
        case 9:
                cp = "3";
                break;
        case 0xa:
                cp = "6";
                break;
        case 0xb:
                cp = "12";
                break;
        case 0xc:
                cp = "22.5";
                break;
        }
        QString title = GboxInfo[el].gbox->title();
        GboxInfo[el].gbox->setTitle(
            title.append(QString::asprintf(" [HBA:%lX/%s Gbps]", sa, cp)));
    }
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

    connect(ui->cbxSlot, &QComboBox::currentIndexChanged, this, &Widget::cbxSlotIndexChanged);
    connect(ui->btnRefresh, &QPushButton::clicked, this, &Widget::btnRefreshClicked);
    connect(ui->btnSmpDoit, &QPushButton::clicked, this, &Widget::btnSmpDoitClicked);
    connect(ui->btnClearTB, &QPushButton::clicked, this, &Widget::btnClearTBClicked);

    // Configure for systray icon
    QIcon icon = QIcon(":/arrows.png");
    QSystemTrayIcon *trayIcon = new QSystemTrayIcon(this);
    trayIcon->setIcon(icon);
    trayIcon->show();
    setWindowIcon(icon);

    appendMessage("Here lists the messages:");

    gCombo = ui->cbxSlot;
    gText = ui->textBrowser;
    gDevices.clear();
    gControllers.clear();
    list_sdevices(verbose);
    slot_discover(verbose);
}

Widget::~Widget()
{
    delete ui;
}

void Widget::appendMessage(QString message)
{
    ui->textBrowser->append(message);
}

void Widget::cbxSlotIndexChanged(int index)
{
    for (int i=0; i<NSLOT; i++) {
        gDevices.setSlotLabel(i);
    }
}

void Widget::btnRefreshClicked()
{
    refreshSlots();
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

void Widget::refreshSlots()
{
    appendMessage("Refresh slots information...");
    gDevices.clear();
    gControllers.clear();
    list_sdevices(verbose);
    slot_discover(verbose);
    appendMessage(QString::asprintf("Found %d expanders and %d devices", gControllers.count(), gDevices.count()));
}

int Widget::phySetDisabled(bool disable)
{
    smp_target_obj tobj;
    int k, i, ret = 0;

    // loop through the expanders discovered
    for (k = i = 0; k < 4; ++k) {
        if (false == gControllers.bsgPath(k).isEmpty()) {
            if (verbose) {
                qDebug() << "----> phy controlling " << gControllers.bsgPath(k);
            }
            tobj.opened = 0;
            // loop through the slots listed
            for ( ; i < (k+1)*28; ++i) {
                // check if the slot is selected or not?
                if (gDevices.cbSlot(i)->isChecked()) {
                    // the expander is to be opened for the 1st selected slot
                    if (0 == tobj.opened) {
                        int res = smp_initiator_open(gControllers.bsgPath(k), I_SGV4, &tobj, verbose);
                        if (res < 0) {
                            break;
                        }
                        // signal Delay after function return
                        ret = 1;
                    }
                    // check if a resonable phy id (4 - 31)
                    int phy_id = gDevices.phyId(i);
                    if (phy_id > 3 && phy_id < 32) {
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
