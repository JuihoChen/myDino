#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QCheckBox>

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

class Widget : public QWidget
{
    Q_OBJECT

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

    void appendMessage(QString message);

private slots:
    void on_pushButton_clicked();
    void on_comboBox_currentIndexChanged(int index);

private:
    void refreshSlots();

    Ui::Widget *ui;
};
#endif // WIDGET_H
