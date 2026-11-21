#include "mainwindow.h"
#include <QApplication>
#include <QLocalServer>
#include <QLocalSocket>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setQuitOnLastWindowClosed(false);

    QString serverName = "LicenseMonitor_SingleInstance_Pipe";

    QLocalSocket socket;
    socket.connectToServer(serverName);

    if (socket.waitForConnected(500)) {
        return 0; 
    }
    QLocalServer::removeServer(serverName); 
    QLocalServer server;
    if (!server.listen(serverName)) {
        return -1;
    }

    MainWindow w;
    QObject::connect(&server, &QLocalServer::newConnection, [&](){
        QLocalSocket *clientSocket = server.nextPendingConnection();
        clientSocket->deleteLater();
        w.show();
        w.showNormal(); 
        w.raise(); 
        w.activateWindow(); 
    });

    if (!w.isServerRunning())
    {
        return 0;
    }
    w.show();
    return a.exec();
}