#ifndef SYSTEMLOGGER_H
#define SYSTEMLOGGER_H

#include <QObject>
#include <QtWidgets/QPlainTextEdit>
#include <QDateTime>

class SystemLogger : public QObject
{
    Q_OBJECT
public:
    static SystemLogger* instance(){
        static SystemLogger instance;
        return &instance;
    }
signals:
    void logMessageReceived(const QString &message);

public:
    void logMessage(const QString &message)
    {
        QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss");
        QString fullMsg = QString("[%1] %2").arg(timeStr, message);
        emit logMessageReceived(fullMsg);
    }

private:
    explicit SystemLogger(QObject* parent = nullptr):QObject(parent){}
};

#endif // SYSTEMLOGGER_H