/*
 * reference: https://doc.qt.io/qt-6/qthread.html
 */

#ifndef WORKER_H
#define WORKER_H

#include <QDebug>
#include <QProcess>
#include <QThread>

class WorkerThread : public QThread
{
    Q_OBJECT
    void run() override {
        /* ... here is the expensive or blocking operation ... */
        do {
            m_errMsg.clear();

            if (m_program.isEmpty()) {
                emit resultReady(m_errMsg = "Not assigned a program to be executed!");
                return;
            }

            qDebug() << m_program + " " + m_arguments.join(" ");

            m_process = new QProcess();
            m_process->setProcessChannelMode(QProcess::MergedChannels);
            m_process->start(m_program, m_arguments);

            if (false == m_process->waitForStarted()) {
                m_errMsg = "Process '" + m_program + "' failed to get started!";
                break;
            }

            /**
             * Warning: Calling this function from the main (GUI) thread might cause your user interface to freeze.
             * If msecs is -1, this function will not time out.
             */
            if (false == m_process->waitForFinished(-1)) {
                m_errMsg = "Process '" + m_program + "' not finished!";
                break;
            }

            QByteArray data;
            data.append(m_process->readAll());

            // Output the data
            qDebug() << data.data();

        } while (0);

        delete m_process;
        m_process = nullptr;

        emit resultReady(m_errMsg);
    }
signals:
    void resultReady(const QString & s);

public:
    WorkerThread(const QString & program, const QStringList & arguments, QObject *parent = nullptr) : QThread(parent) {
        m_process = nullptr;
        m_program = program;
        m_arguments = arguments;
        m_killed = false;
    }

    void killProcess() {
        if (nullptr != m_process && false == m_killed) {
            qDebug() << "kill process";
            /*
             * On Unix and macOS the SIGTERM signal is sent. (Only QProcess::terminate can kill fio)
             */
            m_process->terminate();
            m_killed = true;
        }
    }

    QString m_errMsg;

private:
    QProcess * m_process;
    QString m_program;
    QStringList m_arguments;
    bool m_killed;
};

#endif // WORKER_H
