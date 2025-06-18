#include <QApplication>
#include <getopt.h>

#include "widget.h"

#if QT_NO_DEBUG
#define DEBUG_HL ""
#else
#define DEBUG_HL " (DEBUG MODE)"
#endif

int verbose = 0;
QApplication *gApp;

static struct option long_options[] = {
    { "verbose", no_argument, 0, 'v' },
    };

int main(int argc, char *argv[])
{
    int c;
    while((c = getopt_long(argc, argv, "v", long_options, NULL)) != -1) {
        switch (c) {
        case 'v':
            ++verbose;
            break;
        }
    }

    QApplication a(argc, argv);
    Widget w;
    w.setWindowTitle("myDino [0.15]" DEBUG_HL);
    w.show();

    gApp = &a;
    return a.exec();
}
