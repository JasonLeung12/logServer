#ifndef LICENSEMONITORSERVER_H
#define LICENSEMONITORSERVER_H

//通信模块，该模块主要是用来解析客户端的信息包
//同时根据信息包的类型发送不同的信号调用MonitorDataManager中的槽函数

#include <QTcpServer>
#include <QTcpSocket>
#include <QObject>
#include <QByteArray>
#include <QDataStream>
#include <QPlainTextEdit>
#include "databasemanager.h"
#include <QMutex>

enum class MsgType{
    HeartBeat = 0, //心跳包
    KeyOperation    //关键操作
};

class LicenseMonitorServer : public QTcpServer
{
    Q_OBJECT
public:
    explicit LicenseMonitorServer(QObject *parent = nullptr);  
    bool startServer(quint16 port);

signals:
    //通知：收到心跳包
    void heartBeatReceived(QString licenseId, QString hostName);
    //通知：收到关键操作
    void operationReceived(QString licenseId, QString hostName, QString opName);
    //客户端连接/断开信号
    void clientStatusChanged(QString licenseId, bool onLine);

protected:
    //处理新来的连接
    void incomingConnection(qintptr socketDescriptor) override;

private slots:
	//解析客户端发来的数据包
    void onReadyRead();
    void onReadyReadHttp();

    void onDisconnected();
    void onDisconnectedHttp();

private:
    //解析从客户端收到的JSON数据包，并根据类型发出相应的信号
    void processPacket(QTcpSocket* socket, const QByteArray& data);
    void processHttpPacket(QTcpSocket* socket, const QByteArray& data);
	void sendHttpResponse(QTcpSocket* socket, int statusCode, const QString& statusText, const QString& body);
	void sendResponse(QTcpSocket* socket, const QString& body);

private:
    const quint16 MAGIC_NUMBER = 0x55AA;
    const int HEADER_SIZE = 6;
    //该缓冲区是用来解决tcp粘包和半包问题的
    QMap<QTcpSocket*, QByteArray> m_buffers;
	//用来保存socket和licenseId的映射关系，便于在断开连接时获取licenseId
    //连接时增加。断开时移除
    QMap<QTcpSocket*, QString> m_socketToLicenseMap;
    QRecursiveMutex m_mutex;
};

#endif // LICENSEMONITORSERVER_H
