#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QCheckBox>
#include <QGroupBox>

#include "smp_discover.h"

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

#define NSLOT   112
#define NEXPDR  4

typedef struct ST_SLOTINFO {
    QCheckBox *cb_slot;
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

    void clear();
    void setSlot(QString dir_name, QString device, QString expander, uint64_t wwid);
    void setSlot(QString dir_name, QString device, QString enclosure_device_name);
    void setDiscoverResp(int dsn, uchar * src, int len);
    void setSlotLabel(int sl);
    bool slotVacant(int sl) { return SlotInfo[sl].d_name.isEmpty(); }
    int count() { return myCount; }

    QCheckBox *& cbSlot(int sl) { return SlotInfo[sl].cb_slot; }
    int phyId(int sl) { return SlotInfo[sl].discover_resp[9]; }

private:
    void clrSlot(int sl);
    void setSlot(QString dir_name, QString device, int sl);

private:
    _ST_SLOTINFO SlotInfo[NSLOT];
    int myCount;
};

typedef struct ST_GBOXINFO {
    QGroupBox *gbox;
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

    QGroupBox *& gbThe(int gr) { return GboxInfo[gr].gbox; }
    const QString& bsgPath(int gr) { return GboxInfo[gr].bsg_path; }
    const int subvalue(int gr) { return GboxInfo[gr].ioc_num; }
    uint64_t wwid64(int gr) { return GboxInfo[gr].wwid64; }

private:
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
    void btnClearTBClicked();
    void btnSmpDoitClicked();

private:
    void filloutCanvas();
    void refreshSlots();
    int phySetDisabled(bool disable);

    Ui::Widget *ui;
};

extern DeviceFunc gDevices;
extern ExpanderFunc gControllers;

void gAppendMessage(QString message);

#endif // WIDGET_H
