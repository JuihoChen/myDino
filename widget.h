#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QCheckBox>
#include <QGroupBox>

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

typedef struct ST_SLOTINFO _ST_SLOTINFO;
typedef struct ST_GBOXINFO _ST_GBOXINFO;

class DeviceFunc;
class ExpanderFunc;

extern DeviceFunc gDevices;
extern ExpanderFunc gControllers;

class DeviceFunc
{
public:
    DeviceFunc(_ST_SLOTINFO * gs) : pSlotInfo(gs)
    {}
    ~DeviceFunc()
    {}

    void clear();
    void setSlot(QString path, QString device, QString expander, int iexp);
    void setSlot(QString path, QString device, QString enclosure_device_name);
    void setDiscoverResp(int dsn, uchar * src, int len);
    void setSlotLabel(int sl);
    int count() { return myCount; }

private:
    void setSlot(QString path, QString device, int sl);

    _ST_SLOTINFO * const pSlotInfo;
    int myCount;
};

class ExpanderFunc
{
public:
    ExpanderFunc(_ST_GBOXINFO * ge) : pGboxInfo(ge)
    {}
    ~ExpanderFunc()
    {}

    void clear();
    void setController(QString path, QString expander, int iexp);
    void setDiscoverResp(uint64_t ull, uint64_t sa, uchar * src, int len);
    int count() { return myCount; }

private:
    _ST_GBOXINFO * const pGboxInfo;
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
    void btnRefreshClicked();
    void cbxSlotIndexChanged(int index);
    void btnSmpDoitClicked();
    void btnClearTBClicked();

private:
    void refreshSlots();

    Ui::Widget *ui;
};

void gAppendMessage(QString message);

#endif // WIDGET_H
