#ifndef WIDGET_H
#define WIDGET_H

#include <QCheckBox>
#include <QFileSystemWatcher>
#include <QGroupBox>
#include <QProcess>
#include <QSystemTrayIcon>
#include <QVBoxLayout>
#include <QWidget>

#include "smp_discover.h"

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

#define NEXPDR 4
#define NSLOT_PEREXP 28
#define NSLOT (NEXPDR * NSLOT_PEREXP)

typedef enum {
    SMP = 0,
    SG3,
    FIO,
    FIO2,
    Info
} ENUM_TAB;

typedef enum {
    WWID = 0,
    SDx
} ENUM_COMBO;

typedef struct ST_SLOTINFO {
    QCheckBox * cb_slot;
    QString d_name;
    QString wwid;
    QString block;
    uchar discover_resp[SMP_FN_DISCOVER_RESP_LEN];
    int resp_len;
} _ST_SLOTINFO;

class DeviceFunc
{
public:
    DeviceFunc() {}
    ~DeviceFunc() {};

    void clear(bool uncheck);
    void setSlot(QString dir_name, QString device, QString expander, uint64_t wwid);
    void setSlot(QString dir_name, QString device, QString enclosure_device_name);
    void setDiscoverResp(int dsn, uchar * src, int len);
    void setSlotLabel(int sl);
    bool slotVacant(int sl) { return (sl == valiIndex(sl)) ? SlotInfo[sl].d_name.isEmpty() : false; }
    int count() { return myCount; }

    QCheckBox *& cbSlot(int sl) { return (sl == valiIndex(sl)) ? SlotInfo[sl].cb_slot : dummyCbSlot(); }
    const QString& block(int sl) { return (sl == valiIndex(sl)) ? SlotInfo[sl].block : dummySlotInfo.block; }
    int slotPhyId(int sl) { return (sl == valiIndex(sl) && SlotInfo[sl].resp_len > 9) ? SlotInfo[sl].discover_resp[9] : -1; }

private:
    void clrSlot(int sl, bool uncheck = true);
    void setSlot(QString dir_name, QString device, int sl);
    int valiIndex(int sl) {
        if ((unsigned)sl < NSLOT)
            return sl;
        else {
            qDebug("%s: incorrect device slot indexing: %d", __func__, sl);
            return 0;
        }
    }
    // QWidget: Must construct a QApplication before a QWidget
    // TODO: Create a method to make the allocated Checkbox to be pointed by the dummy slot
    QCheckBox *& dummyCbSlot() {
        if (nullptr == dummySlotInfo.cb_slot) {
            dummySlotInfo.cb_slot = new QCheckBox;
        }
        return dummySlotInfo.cb_slot;
    }

private:
    _ST_SLOTINFO dummySlotInfo = { .cb_slot = nullptr, .resp_len = 0 };
    _ST_SLOTINFO SlotInfo[NSLOT];
    int myCount;
};

typedef struct ST_GBOXINFO {
    QGroupBox * gbox;
    QString d_name;
    QString bsg_path;
    int ioc_num;
    uint64_t wwid64;
    uchar discover_resp[SMP_FN_DISCOVER_RESP_LEN];
    int resp_len;
} _ST_GBOXINFO;

class ExpanderFunc
{
public:
    ExpanderFunc() {}
    ~ExpanderFunc() {}

    void clear();
    void setController(QString expander, uint64_t wwid);
    void setDiscoverResp(QString path, int subvalue, uint64_t ull, uint64_t sa, uchar * src, int len);
    int count() { return myCount; }

    QGroupBox *& gbThe(int el) { return (el == valiIndex(el)) ? GboxInfo[el].gbox : dummyGbox(); }
    const QString& bsgPath(int el) { return (el == valiIndex(el)) ? GboxInfo[el].bsg_path : dummyGboxInfo.bsg_path; }
    const int subvalue(int el) { return (el == valiIndex(el)) ? GboxInfo[el].ioc_num : dummyGboxInfo.ioc_num; }
    uint64_t wwid64(int el) { return (el == valiIndex(el)) ? GboxInfo[el].wwid64 : dummyGboxInfo.wwid64; }

private:
    int valiIndex(int el) {
        if ((unsigned)el < NEXPDR)
            return el;
        else {
            qDebug("%s: incorrect expander indexing: %d", __func__, el);
            return 0;
        }
    }
    // QWidget: Must construct a QApplication before a QWidget
    // TODO: Create a method to make the allocated Checkbox to be pointed by the dummy slot
    QGroupBox *& dummyGbox() {
        if (nullptr == dummyGboxInfo.gbox) {
            dummyGboxInfo.gbox = new QGroupBox;
        }
        return dummyGboxInfo.gbox;
    }

private:
    _ST_GBOXINFO dummyGboxInfo = { .gbox = nullptr, .ioc_num = -1, .resp_len = 0 };
    _ST_GBOXINFO GboxInfo[NEXPDR];
    int myCount;
};

class Widget : public QWidget
{
    Q_OBJECT

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

    void appendMessage(QString message);

private slots:
    void cbxSlotIndexChanged(int index);
    void btnRefreshClicked();
    void btnSelectAllClicked();
    void btnListSdxClicked();
    void btnClearTBClicked();
    void btnSmpDoitClicked();
    void btnFio2GoClicked();
    void tabSelected();
    void showModified(const QString & path);

protected:
    void closeEvent(QCloseEvent *event) {
        qDebug() << "Close button is pressed!!";
        m_closed++;
        QWidget::closeEvent(event);
    }

private:
    void filloutCanvas(bool uncheck = true);
    int phySetDisabled(bool disable);
    void sdxlist_sit(QTextStream & stream, int sl = -1);
    void sdxlist_wl1(QTextStream & stream, int sl = -1);
    void sdxlist_wl2(QTextStream & stream, int sl = -1);
    void autofio_wls(int wl);
    void invokeProcess(const QString & program, const QStringList & arguments, int progress_maxms = 0);
    void setFanDuty(const QString duty);
    void pauseBar(const int pause_ms);

    Ui::Widget * ui;
    QVBoxLayout * m_layout;
    QSystemTrayIcon * m_trayIcon;
    QFileSystemWatcher * m_Watcher;
    int m_closed;
};

extern DeviceFunc gDevices;
extern ExpanderFunc gControllers;

void gAppendMessage(QString message);

#endif // WIDGET_H
