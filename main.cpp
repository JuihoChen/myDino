#include "widget.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    Widget w;
    w.setWindowTitle("myDino");
    w.show();

    w.appendMessage("Here lists the messages:");
    //w.appendMessage("bbbb");
    //w.appendMessage("aaaa");
    //w.appendMessage("bbbb");
    //w.appendMessage("aaaa");

    //wwn[10]->setDisabled(false);

    return a.exec();
}
