#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

#include "ui_widget.h"
#include "widget.h"
#include "worker.h"
#include "lsscsi.h"
#include "smp_lib.h"
#include "smp_discover.h"
#include "mpi3mr_app.h"

extern int verbose;

static QTabWidget * gTab = nullptr;
static QComboBox * gCombo = nullptr;
static QTextBrowser * gText = nullptr;

DeviceFunc gDevices;
ExpanderFunc gControllers;

ENUM_CARDTYPE cardType = ENUM_CARDTYPE::HBA9600;

void DeviceFunc::clear(bool uncheck)
{
    for (int i = 0; i < NSLOT; i++) {
        clrSlot(i, uncheck);
    }
    myCount = 0;
}

void DeviceFunc::clrSlot(int sl, bool uncheck)
{
    // validate the index passed
    if (sl == valiIndex(sl)) {

        // clear slot stuff
        SlotInfo[sl].cb_slot->setText(QString("Slot %1").arg(sl+1));
        SlotInfo[sl].cb_slot->setStyleSheet("QCheckBox:enabled{color: black;} QCheckBox:disabled{color: grey;}");
        SlotInfo[sl].cb_slot->setEnabled(false);
        if (uncheck) SlotInfo[sl].cb_slot->setCheckState(Qt::CheckState::Unchecked);
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
        // Get wwid of this device (some BMC exposed "Virtual" devices does not have wwid attribute)
        QString wwid, wd = dir_name.append("/%1").arg(device);
        if (get_myValue(wd, "wwid", wwid) && !wwid.isEmpty()) {

            SlotInfo[sl].wwid = wwid;

            // Set slot occupied by something
            SlotInfo[sl].d_name = device;

            // Get block name of this device
            wd += "/block";
            SlotInfo[sl].block = get_blockname(wd);

            SlotInfo[sl].cb_slot->setEnabled(true);
            setSlotLabel(sl);
            myCount++;
        }
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

void DeviceFunc::setSlot(int slp, QString d_name, QString wwid, QString block)
{
    // the device should be within this expander's domain
    if (slp <= 0 || slp > NSLOT) {
        qDebug() << "Device [" << d_name << "] setting error!";
        return;
    }

    // validate the index passed
    int sl = valiIndex(slp - 1);
    {
        // Set slot occupied by something
        SlotInfo[sl].d_name = d_name;

        // Get wwid of this device
        SlotInfo[sl].wwid = wwid;

        // Get block name of this device
        SlotInfo[sl].block = block;

        SlotInfo[sl].cb_slot->setEnabled(true);
        setSlotLabel(sl);
        myCount++;
    }
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
        if (nullptr != gTab && ENUM_TAB::FIO != gTab->currentIndex() && ENUM_TAB::FIO2 != gTab->currentIndex()) {
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

        switch (gCombo->currentIndex()) {
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

void ExpanderFunc::setDiscoverResp(QString path, uint64_t ull, uint64_t sa, uchar * src, int len)
{
    int el = WWID_TO_INDEX(ull);

    // save the working path for later usages
    GboxInfo[el].bsg_path = path;

    /**
     * IOC number is just derived from the trailing digit of bsg_path
     */
    //GboxInfo[el].ioc_num = subvalue;

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

    m_layout = new QVBoxLayout;
    m_layout->addWidget(ui->tabWidget);
    m_layout->addWidget(ui->groupBox_0);
    m_layout->addWidget(ui->groupBox_1);
    m_layout->addWidget(ui->groupBox_2);
    m_layout->addWidget(ui->groupBox_3);
    //m_layout->addStretch();
    m_layout->addWidget(ui->textBrowser);
    setLayout(m_layout);

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
    connect(ui->btnSelectAll, &QPushButton::clicked, this, &Widget::btnSelectAllClicked);
    connect(ui->btnListSdx, &QPushButton::clicked, this, &Widget::btnListSdxClicked);
    connect(ui->btnSmpDoit, &QPushButton::clicked, this, &Widget::btnSmpDoitClicked);
    connect(ui->btnFio2Go, &QPushButton::clicked, this, &Widget::btnFio2GoClicked);
    connect(ui->btnClearTB, &QPushButton::clicked, this, &Widget::btnClearTBClicked);
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &Widget::tabSelected);

    m_Watcher = new QFileSystemWatcher();
    if (false == m_Watcher->addPath(dev_bsg)) {
        qDebug() << "QFileSystemWatcher: failed to add path";
    }
    connect(m_Watcher, &QFileSystemWatcher::directoryChanged, this, &Widget::showModified);

    // Configure for systray icon
    QIcon icon = QIcon(":/arrows.png");
    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(icon);
    m_trayIcon->show();
    setWindowIcon(icon);

    // Init global ui controls handy for manipulation
    gTab = ui->tabWidget;
    gCombo = ui->cbxSlot;
    gText = ui->textBrowser;

    ui->progress_afio->hide();
    ///ui->radDiscover->hide();    // temporarily hide for release

    appendMessage("Here lists the messages:");
    filloutCanvas();
}

Widget::~Widget()
{
    delete ui;
    delete m_layout;
    delete m_trayIcon;
    delete m_Watcher;
}

void Widget::appendMessage(QString message)
{
    ui->textBrowser->append(message);
}

void Widget::showModified(const QString & path)
{
    appendMessage("Slots information refreshed due to being modified");

    // Refreshing ?...
    filloutCanvas(false);
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

void Widget::btnSelectAllClicked()
{
    for (int i = 0; i < NSLOT; i++) {
        if (true == gDevices.cbSlot(i)->isEnabled()) {
            gDevices.cbSlot(i)->setChecked(true);
        }
    }
}

void Widget::sdxlist_sit(QTextStream & stream, int sl)
{
    Q_UNUSED(sl);

    // loop through the expanders discovered
    for (int k = 0; k < NEXPDR; ++k) {
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

void Widget::sdxlist_wl1(QTextStream & stream, int sl)
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
    for (int k = 0; k < NEXPDR; ++k) {
        stream << "## Expander-" << k+1 << Qt::endl;
        if (false == gControllers.bsgPath(k).isEmpty()) {
            // Workload 1
            for (int i = k*NSLOT_PEREXP; i < (k+1)*NSLOT_PEREXP; ++i) {
                // check if this slot is occupied?
                if (false == gDevices.slotVacant(i)) {
                    stream << "[job" << ++jobn << "]" << Qt::endl
                           << "filename=/dev/" << gDevices.block(i) << Qt::endl;
                    // check if this slot is the target?
                    if ((i == sl) || ((-1 == sl) && gDevices.cbSlot(i)->isChecked())) {
                        stream << "bs=512k" << Qt::endl
                               << "rw=write" << Qt::endl;
                    }
                    stream << Qt::endl;
                }
            }
        }
    }
}

void Widget::sdxlist_wl2(QTextStream & stream, int sl)
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
    for (int k = 0; k < NEXPDR; ++k) {
        stream << "## Expander-" << k+1 << Qt::endl;
        if (false == gControllers.bsgPath(k).isEmpty()) {
            // Workload 2
            for (int i = k*NSLOT_PEREXP; i < (k+1)*NSLOT_PEREXP; ++i) {
                // check if this slot is occupied?
                if (false == gDevices.slotVacant(i)) {
                    stream << "[job" << ++jobn << "]" << Qt::endl
                           << "filename=/dev/" << gDevices.block(i) << Qt::endl;
                    // check if this slot is the target?
                    if ((i == sl) || ((-1 == sl) && gDevices.cbSlot(i)->isChecked())) {
                        stream << "bs=4k" << Qt::endl
                               << "rw=randwrite" << Qt::endl;
                    }
                    stream << Qt::endl;
                }
            }
        }
    }
}

/*
 * Use resultReady signal needs one more QCoreApplication::processEvents call after workerThread->isFinished,
 * So a member m_errMsg is created and used for message passing between threads
 */
void Widget::startWorkInAThread(const QString & program, const QStringList & arguments, int progress_maxms)
{
    /*
     * Check if ui is closed before the thread gets started
     */
    if (0 != m_closed) {
        throw QString("Close button is pressed!");
    }

    WorkerThread * workerThread = new WorkerThread(program, arguments, this);
    //connect(workerThread, &WorkerThread::resultReady, this, &Widget::handleResults);
    connect(workerThread, &WorkerThread::finished, workerThread, &QObject::deleteLater);
    workerThread->start();

    if (0 != progress_maxms)
    {
        ui->progress_afio->setRange(0, progress_maxms);
        ui->progress_afio->setValue(0);
        ui->progress_afio->show();
    }
    QElapsedTimer timer;
    timer.start();
    while (false == workerThread->isFinished()) {
        extern QApplication *gApp;
        gApp->processEvents();
        /*
         * Do NOT throw (too premature to end the thread) if ui is closed till here,
         * Just kill the process and wait the thread to be finished instead.
         */
        if (0 != m_closed) {
            workerThread->killProcess();
        }
        if (0 != progress_maxms) {
            ui->progress_afio->setValue(timer.elapsed());
        }
    }
    ui->progress_afio->hide();

    if (false == workerThread->m_errMsg.isEmpty()) {
        throw(workerThread->m_errMsg);
    }
}

void Widget::setFanDuty(const QString duty)
{
    appendMessage("ipmitool sets fan duty to " + duty + "%");

    QString program = "ipmitool";
    QStringList arguments;

    // set this command every AC cycle.
    arguments << "raw" << "0x2e" << "0x40" << "0x16" << "0x7d" << "0x00" << "0x01";
    startWorkInAThread(program, arguments);

    // set pwm duty cycle
    arguments.clear();
    arguments << "raw" << "0x2e" << "0x44" << "0x16" << "0x7d" << "0x00" << "0xff" << duty;
    startWorkInAThread(program, arguments);
}

void Widget::pauseBar(const int pause_ms)
{
    if (pause_ms) {
        ui->progress_afio->setRange(0, pause_ms);
        ui->progress_afio->setValue(0);

        QPalette r, p;
        r = p = ui->progress_afio->palette();
        p.setColor(QPalette::Highlight, Qt::green);
        ui->progress_afio->setPalette(p);
        ui->progress_afio->show();

        QElapsedTimer timer;
        timer.start();
        int progress = 0;
        while (progress < pause_ms && 0 == m_closed) {
            // Run a function with a delay in QT
            QEventLoop loop;
            QTimer::singleShot(100, &loop, &QEventLoop::quit);
            loop.exec();
            ui->progress_afio->setValue(progress = timer.elapsed());
        }

        ui->progress_afio->setPalette(r);
        ui->progress_afio->hide();
    }
}

void Widget::autofio_wls(int wl)
{
    QMessageBox msgBox(this);
    msgBox.setIconPixmap(QPixmap(":/listsdx_48.png"));
    msgBox.setText(QString("Auto FIO (Workload %1)").arg(wl));
    msgBox.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);

    if (QMessageBox::Ok == msgBox.exec()) {

        QCheckBox * cbfd[] = { ui->cb_fd50, ui->cb_fd60, ui->cb_fd70, ui->cb_fd80, ui->cb_fd90, ui->cb_fd100 };
        QString fd[] = { "50", "60", "70", "80", "90", "100" };
        for (int l = 0; l < sizeof(cbfd)/sizeof(cbfd[0]); ++l) {
            cbfd[l]->setEnabled(false);
        }

        int loops = 0;
        if (ui->cb_fd100->isChecked()) loops++;
        if (ui->cb_fd90->isChecked()) loops++;
        if (ui->cb_fd80->isChecked()) loops++;
        if (ui->cb_fd70->isChecked()) loops++;
        if (ui->cb_fd60->isChecked()) loops++;
        if (ui->cb_fd50->isChecked()) loops++;
        if (loops == 0) {
            ui->cb_fd100->setChecked(true);
            loops = 1;
        }

        try {
            int devCount = 0;
            for (int i = 0; i < NSLOT; i++) {
                if (false == gDevices.slotVacant(i) && true == gDevices.cbSlot(i)->isChecked()) {
                    ++devCount;
                }
            }
            if (0 == devCount) {
                throw QString("No device selected to test!");
            }

            bool tested = false;
            int processed = 0;

            // loop through fan duty 50% -> 100%
            for (int l = 0; l < sizeof(cbfd)/sizeof(cbfd[0]); ++l) {

                // check if this fan duty is to loop
                if (cbfd[l]->isChecked()) {
                    // ipmitool sets fan duty to 50%, 60%, ...
                    setFanDuty(fd[l]);

                    // loop through the expanders discovered
                    for (int k = 0; k < NEXPDR; ++k) {
                        appendMessage(QString::asprintf("## Expander-%d", k+1));
                        if (false == gControllers.bsgPath(k).isEmpty()) {
                            // Workload 1
                            for (int i = k*NSLOT_PEREXP; i < (k+1)*NSLOT_PEREXP; ++i) {
                                // check if this slot is occupied?
                                if (false == gDevices.slotVacant(i) && true == gDevices.cbSlot(i)->isChecked()) {

                                    // check if pause time need to insert between tests
                                    if (tested) {
                                        pauseBar(ui->spinAfwl->value() * 1000);
                                    }
                                    tested = true;

                                    QDateTime date(QDateTime::currentDateTime());
                                    QString time = date.toString("_yyyyMMdd_hhmmss");
                                    QString head = (1 == wl) ? "512k_SeqW_" : "4k_RandW_";
                                    QString sln = QString::asprintf("_sl%03d_", i + 1);
                                    QString fio = head + fd[l] + sln + gDevices.block(i) + time + ".fio";
                                    QString out = head + fd[l] + sln + gDevices.block(i) + time + ".txt";
                                    QString msg = fio + "  -->  " + out + QString::asprintf(" (%d/%d)", ++processed, loops * devCount);
                                    appendMessage(msg);

                                    QFile file(fio);
                                    if (false == file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                                        throw QString("FIO script failed to open for write!");
                                    }

                                    // We're going to streaming text to the file
                                    QTextStream stream(&file);

                                    // Do the listing
                                    (1 == wl) ? sdxlist_wl1(stream, i) : sdxlist_wl2(stream, i);

                                    // Close the script file
                                    file.close();

                                    // Execute FIO test
                                    QStringList arguments;
                                    arguments << fio << "--output" << out;
                                    startWorkInAThread("fio", arguments, 150 * 1000);

                                    // Finally, remove the script file
                                    file.remove();
                                }
                            }
                        }
                    }
                }
            }
            // Test is over!
            appendMessage("Batch test is completed!");

        } catch (QString errMsg) {
            appendMessage(errMsg);
        }

        for (int l = 0; l < sizeof(cbfd)/sizeof(cbfd[0]); ++l) {
            cbfd[l]->setEnabled(true);
        }
    }

    // Modal QMessageBox greys out the tab page, repaint the tab widget
    ui->tabWidget->repaint();
}

void Widget::btnListSdxClicked()
{
    const char * SDX_LIST_FILE[] = { "Dino_sdx_list.txt", "512k_SeqW_4k_RandR.fio", "4k_RandW_4k_RandR.fio" };
    void (Widget::*do_list[])(QTextStream & stream, int sl) = { &Widget::sdxlist_sit, &Widget::sdxlist_wl1, &Widget::sdxlist_wl2 };

    int choice = 0;
    if (ui->tabWidget->currentIndex() == ENUM_TAB::FIO) {
        if (ui->radWl1->isChecked()) {
            choice = 1;
        }
        if (ui->radWl2->isChecked()) {
            choice = 2;
        }
        if (ui->radAf1->isChecked()) {
            autofio_wls(1);
            return;
        }
        if (ui->radAf2->isChecked()) {
            autofio_wls(2);
            return;
        }
    }

    QMessageBox msgBox(this);
    msgBox.setIconPixmap(QPixmap(":/listsdx_48.png"));
    msgBox.setText("SDx Listing");
    msgBox.setInformativeText(QString::asprintf("Please check file: %s", SDX_LIST_FILE[choice]));
    msgBox.setStandardButtons(QMessageBox::Ok);

    QFile file(SDX_LIST_FILE[choice]);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {

        // We're going to streaming text to the file
        QTextStream stream(&file);

        // Do the listing
        (this->*do_list[choice])(stream, -1);

        // Close the output file
        file.close();
    }

    msgBox.exec();

    // Modal QMessageBox greys out the tab page, repaint the tab widget
    ui->tabWidget->repaint();
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
        delay = phySetDisabled(true);
    }
    else if (ui->radPhyReset->isChecked()) {
        appendMessage("Enable phys...");
        delay = phySetDisabled(false);
    }
    else if (ui->radDiscover->isChecked()) {
        appendMessage("Discover expanders...");
        mpi3mr_discover(verbose);
        return;
    }

    // Run a function with a delay in QT
    if (delay > 0) {
        QEventLoop loop;
        QTimer::singleShot(delay, &loop, &QEventLoop::quit);
        loop.exec();

        // Refreshing ?...
        filloutCanvas();
    }
}

void Widget::btnFio2GoClicked()
{
    enum { RANDREAD=0, RANDWRITE, SEQREAD, SEQWRITE, RW_ALL };
    QString fioname[] = { "randread", "randwrite", "read", "write" };
    QString bs[] = { "4K", "64K", "128K", "256", "512K", "1M" };
    QString iodepth[] = { "8", "16" };
    QString group[] = { "group_reporting=0", "#group_reporting"};
    int ramp_time[] = { 5, 10, 20, 30 };
    int runtime[] = { 60, 120, 180, 240 };

    try {
        int devCount = 0;
        for (int i = 0; i < NSLOT; i++) {
            if (false == gDevices.slotVacant(i) && true == gDevices.cbSlot(i)->isChecked()) {
                ++devCount;
            }
        }
        if (0 == devCount) {
            throw QString("No device selected to test!");
        }

        // ipmitool sets fan duty to 100%
        setFanDuty("100");

        int il, loops;
        if (RW_ALL == ui->cbxRW->currentIndex()) {
            il = RANDREAD;
            loops = 4;
        } else {
            il = ui->cbxRW->currentIndex();
            loops = 1;
        }

        bool tested = false;
        int processed = 0;

        // loop through the RW action
        for (int m = 0; m < loops; ++m, ++il) {

            // check if pause time need to insert between tests
            if (tested) {
                pauseBar(ui->spinFio2Wl->value() * 1000);
            }
            tested = true;

            QDateTime date(QDateTime::currentDateTime());
            QString time = date.toString("_yyyyMMdd_hhmmss");
            QString fio = "fio2_" + fioname[il] + "_" + time + ".fio";
            QString out = "fio2_" + fioname[il] + "_" + time + ".txt";
            QString msg = fio + "  -->  " + out + QString::asprintf(" (%d/%d)", ++processed, loops);
            appendMessage(msg);

            QFile file(fio);
            if (false == file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                throw QString("FIO script failed to open for write!");
            }

            // We're going to streaming text to the file
            QTextStream stream(&file);

            // Do the listing
            stream << "[global]" << Qt::endl
                   << "bs=" << bs[ui->cbxBS->currentIndex()] << Qt::endl
                   << "iodepth=" << iodepth[ui->cbxIODepth->currentIndex()] << Qt::endl
                   << "direct=1" << Qt::endl
                   << "ioengine=libaio" << Qt::endl
                   << group[ui->cbxGroup->currentIndex()] << Qt::endl
                   << "time_based" << Qt::endl
                   << "ramp_time=" << ramp_time[ui->cbxRamp->currentIndex()] << Qt::endl
                   << "runtime=" << runtime[ui->cbxRuntime->currentIndex()] << Qt::endl
                   << "name=" << fioname[il] << Qt::endl
                   << "rw=" << fioname[il] << Qt::endl << Qt::endl;

            int jobn = 0;

            // loop through the expanders discovered
            for (int k = 0; k < NEXPDR; ++k) {
                stream << "## Expander-" << k+1 << Qt::endl;
                if (false == gControllers.bsgPath(k).isEmpty()) {
                    // Workload 1
                    for (int i = k*NSLOT_PEREXP; i < (k+1)*NSLOT_PEREXP; ++i) {
                        // check if this slot is occupied?
                        if (false == gDevices.slotVacant(i) && true == gDevices.cbSlot(i)->isChecked()) {
                            stream << "[job" << ++jobn << "]" << Qt::endl
                                   << "filename=/dev/" << gDevices.block(i) << Qt::endl << Qt::endl;
                        }
                    }
                }
            }

            // Close the script file
            file.close();

            // Execute FIO test
            QStringList arguments;
            arguments << fio << "--output" << out;
            startWorkInAThread("fio", arguments, (ramp_time[ui->cbxRamp->currentIndex()] + runtime[ui->cbxRuntime->currentIndex()]) * 1000);

            // Finally, remove the script file
            //file.remove();
        }
        // Test is over!
        appendMessage("Batch test is completed!");

    } catch (QString errMsg) {
        appendMessage(errMsg);
    }

    // Modal QMessageBox greys out the tab page, repaint the tab widget
    ui->tabWidget->repaint();
}

void Widget::tabSelected()
{
    static int lastIndex = 0;
    int currIndex = ui->tabWidget->currentIndex();
    switch (currIndex) {
    case ENUM_TAB::SMP:
        if (lastIndex == ENUM_TAB::FIO || lastIndex == ENUM_TAB::FIO2) {
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
        // Fall-through ...
    case ENUM_TAB::FIO2:
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
        if (cardType == ENUM_CARDTYPE::HBA9500) {
            ui->textInfo->append("HBA is 9500");
            break;
        }
        if (cardType == ENUM_CARDTYPE::RAID9x60) {
            ui->textInfo->append("RAID9x60 plug-in card");
            break;
        }
        ui->textInfo->append(get_infofacts());
        // Scroll QTextBrowser to the top
        QTextCursor cursor = ui->textInfo->textCursor();
        cursor.setPosition(0);
        ui->textInfo->setTextCursor(cursor);
        break;
    }
}

void Widget::filloutCanvas(bool uncheck)
{
    gDevices.clear(uncheck);
    gControllers.clear();
    list_sdevices(verbose);

    if (cardType == ENUM_CARDTYPE::HBA9500) {
        slot_discover(verbose);
    }

    if (cardType == ENUM_CARDTYPE::HBA9600) {
        // Discover the expanders and devices
        mpi3mr_slot_discover(verbose);

        if (ui->tabWidget->currentIndex() == ENUM_TAB::Info) {
            ui->textInfo->clear();
            ui->textInfo->append(get_infofacts());
            // Scroll QTextBrowser to the top
            QTextCursor cursor = ui->textInfo->textCursor();
            cursor.setPosition(0);
            ui->textInfo->setTextCursor(cursor);
        }
    }
}

/* return value is the delay time */
int Widget::phySetDisabled(bool disable)
{
    smp_target_obj tobj;
    int k, i, ret = 0;

    // loop through the expanders discovered
    for (k = 0; k < NEXPDR; ++k) {
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
                        IntfEnum sel = (cardType == ENUM_CARDTYPE::HBA9500) ? I_SGV4 : I_SGV4_MPI;
                        // assign the IOC number for multiple adapters case
                        int res = smp_initiator_open(gControllers.bsgPath(k), sel, &tobj, verbose);
                        if (res < 0) {
                            break;
                        }
                        // signal Delay after function return
                        ret = 10;
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
