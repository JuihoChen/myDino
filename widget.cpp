#include "widget.h"
#include "./ui_widget.h"
#include "./lsscsi.h"
#include "./smp_discover.h"

#include <QVBoxLayout>
#include <QSystemTrayIcon>

#define NSLOT   112
#define NEXPDR  4

_ST_SLOTINFO gSlot[NSLOT];
_ST_GBOXINFO gExpander[NEXPDR];

DeviceFunc gDevices(gSlot);
ExpanderFunc gControllers(gExpander);

QComboBox *gCb = nullptr;

void DeviceFunc::clear()
{
    for (int i = 0; i < NSLOT; i++) {
        pSlotInfo[i].cb_slot->setText(QString("Slot %1").arg(i+1));
        pSlotInfo[i].cb_slot->setStyleSheet("QCheckBox:enabled{color: black;} QCheckBox:disabled{color: grey;}");
        pSlotInfo[i].cb_slot->setDisabled(true);
        pSlotInfo[i].cb_slot->setCheckState(Qt::CheckState::Unchecked);
        pSlotInfo[i].d_name.clear();
        pSlotInfo[i].wwid.clear();
        pSlotInfo[i].block.clear();
    }
    myCount = 0;
}

void DeviceFunc::setSlot(QString path, QString device, int sl)
{
    if ((unsigned)sl < NSLOT) {
        pSlotInfo[sl].d_name = device;

        // Get wwid of this device
        QString wd = path.append("/%1").arg(device);
        if (false == get_myValue(wd, "wwid", pSlotInfo[sl].wwid)) {
            pSlotInfo[sl].wwid.clear();
        }

        // Get block name of this device
        wd += "/block";
        pSlotInfo[sl].block = get_blockname(wd);

        pSlotInfo[sl].cb_slot->setEnabled(true);
        setSlotLabel(sl);
        myCount++;
    }
}

void DeviceFunc::setSlot(QString path, QString device, QString expander, int iexp)
{
    int sl = compute_device_index(device.toStdString().c_str(), expander.toStdString().c_str());

    // the device should be within this expander's domain
    if (sl <= 0 || sl > 28 || iexp < 0 || iexp > 3) {
        printf("Device [%s] setting error!\n", device.toStdString().c_str());
        return;
    }

    sl = (iexp + 1) * 28 - sl;

    setSlot(path, device, sl);
}

void DeviceFunc::setSlot(QString path, QString device, QString enclosure_device_name)
{
    setSlot(path, device, enclosure_device_name.right(2).toShort(0, 16) - 1);
}

void DeviceFunc::setSlotLabel(int sl)
{
    if (gCb && !pSlotInfo[sl].d_name.isEmpty())
    switch (gCb->currentIndex())
    {
    case 0:
        pSlotInfo[sl].cb_slot->setText(pSlotInfo[sl].wwid.right(16));
        break;
    case 1:
        pSlotInfo[sl].cb_slot->setText(QString("%1. ").arg(sl+1) + pSlotInfo[sl].block);
        break;
    default:
        pSlotInfo[sl].cb_slot->setText(QString("%1. ").arg(sl+1) + pSlotInfo[sl].d_name);
        break;
    }
}

void ExpanderFunc::clear()
{
    for (int i = 0; i < NEXPDR; i++) {
        pGboxInfo[i].gbox->setTitle(QString("Expander-%1").arg(i+1));
        pGboxInfo[i].d_name.clear();
        pGboxInfo[i].wwid.clear();
    }
    myCount = 0;
}

void ExpanderFunc::setController(QString path, QString expander, int iexp)
{
    // Only expanders 0-3 should be taken care of...
    if (iexp < 0 || iexp > 3) {
        printf("Expander [%s] setting error!\n", expander.toStdString().c_str());
        return;
    }

    pGboxInfo[iexp].d_name = expander;

    // Get wwid of this enclosure
    QString wd = path.append("/%1/enclosure/%1").arg(expander);
    if (false == get_myValue(wd, "id", pGboxInfo[iexp].wwid)) {
        pGboxInfo[iexp].wwid.clear();
    }

    QString title = pGboxInfo[iexp].gbox->title();
    pGboxInfo[iexp].gbox->setTitle(
        title + QString(" [%1]").arg(pGboxInfo[iexp].wwid.right(16).toUpper()));

    myCount++;
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
        gSlot[i].cb_slot = _slot[i];
    }

    // Setup Groubox 0-3 to be globally accessed
    gExpander[0].gbox = ui->groupBox_0;
    gExpander[1].gbox = ui->groupBox_1;
    gExpander[2].gbox = ui->groupBox_2;
    gExpander[3].gbox = ui->groupBox_3;

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

    gCb = ui->cbxSlot;
    gDevices.clear();
    gControllers.clear();
    list_sdevices(this, verbose);
}

Widget::~Widget()
{
    delete ui;
}

void Widget::appendMessage(QString message)
{
    ui->textBrowser->append(message);
}

void Widget::btnRefreshClicked()
{
    refreshSlots();
}

void Widget::cbxSlotIndexChanged(int index)
{
    for (int i=0; i<NSLOT; i++) {
        gDevices.setSlotLabel(i);
    }
}

void Widget::refreshSlots()
{
    appendMessage("Refresh slots information...");
    gDevices.clear();
    gControllers.clear();
    list_sdevices(this, verbose);
    appendMessage(QString::asprintf("Found %d expanders and %d devices", gControllers.count(), gDevices.count()));
}

void Widget::btnSmpDoitClicked()
{
    if (ui->radDiscover->isChecked()) {
        appendMessage("Discover expanders...");
        smpDiscover(this, verbose);
    }
}

void Widget::btnClearTBClicked()
{
    ui->textBrowser->clear();  // Clear the default ui text
}
