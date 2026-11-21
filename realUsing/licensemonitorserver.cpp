#include "licensemonitorserver.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QMessageBox>
#include <QSet>


#include "systemLogger.h"

LicenseMonitorServer::LicenseMonitorServer(QObject *parent)
    : QTcpServer{parent}
{}

bool LicenseMonitorServer::startServer(quint16 port)
{
    if(!this->listen(QHostAddress::Any, port)){
        QMessageBox::warning(nullptr,
            QStringLiteral("端口错误"),
            QStringLiteral("监控服务器启动失败: %1\n请检查端口 %2 是否被占用。")
            .arg(this->errorString())
            .arg(port));
        return false;
    }

    SystemLogger::instance()->logMessage(QString(QStringLiteral("Server started on start:%1")).arg(port));
    return true;
}

void LicenseMonitorServer::incomingConnection(qintptr socketDescriptor) {
    QTcpSocket* socket = new QTcpSocket(this);
    if (socket->setSocketDescriptor(socketDescriptor)) {
        connect(socket, &QTcpSocket::readyRead, this, &LicenseMonitorServer::onReadyReadHttp);
        connect(socket, &QTcpSocket::disconnected, this, &LicenseMonitorServer::onDisconnected);

        m_buffers[socket] = QByteArray();
    }
    else {
        delete socket;
    }
}

//自定义协议头+显示长度+循环缓冲区 解决粘包/半包问题
void LicenseMonitorServer::onReadyRead(){
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if(!socket) return;
    QMutexLocker locker(&m_mutex);
    if (!m_buffers.contains(socket))    return;
    //将新收到的数据添加到对应的socket缓冲区
    m_buffers[socket].append(socket->readAll());
    
    //循环处理缓冲区，直到没有完整的包为止(解决粘包/半包)
    while(m_buffers[socket].size()>=HEADER_SIZE&&m_buffers.contains(socket))
    {
        QByteArray& buffer = m_buffers[socket];
        quint16 magic = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(buffer.constData()));
        if(magic!=MAGIC_NUMBER){
            buffer.remove(0,1);
            continue;
        }

        //读取body长度
        quint32 bodyLen = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(buffer.constData() + 2));
        if (bodyLen > 1024 * 1024)
        {
            buffer.clear();
            return;
        }
        //检查缓冲区是否包含完整的body
        if(buffer.size() < HEADER_SIZE + bodyLen){
            //数据不全
            break;
        }
        //提取完整的JSON数据包
        QByteArray bodyData = buffer.mid(HEADER_SIZE, bodyLen);
        buffer.remove(0, HEADER_SIZE + bodyLen);

        locker.unlock();
        //解析业务逻辑
        processPacket(socket,bodyData);
        locker.relock();
    }
}

void LicenseMonitorServer::onReadyReadHttp()
{
	QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;
	if (!m_buffers.contains(socket)) return;
	m_buffers[socket].append(socket->readAll());

	QByteArray& buffer = m_buffers[socket];
	int headerEndIndex = buffer.indexOf("\r\n\r\n");
	if (headerEndIndex == -1) return;

    int bodylen = 0;
	QByteArray headerData = buffer.left(headerEndIndex);
	QList<QByteArray> lines = headerData.split('\n');
    for(auto line:lines){
        line = line.trimmed();
        if (line.startsWith("Content-Length:")) {
            bodylen = line.mid(15).trimmed().toInt();
            break;
        }
	}

	int totalExpectedLength = headerEndIndex + 4 + bodylen;
	if (buffer.size() < totalExpectedLength) return;

	QByteArray bodyData = buffer.left(headerEndIndex + 4 + bodylen);
	buffer.remove(0, totalExpectedLength);
	processHttpPacket(socket, bodyData);
}

void LicenseMonitorServer::processHttpPacket(QTcpSocket* socket, const QByteArray& data)
{
	int headerEnd = data.indexOf("\r\n\r\n");
    if (headerEnd == -1)
    {
		sendHttpResponse(socket, 400, "Bad Request", "Invalid HTTP request");
        return;
    }
	QByteArray headerBytes = data.left(headerEnd);
	QByteArray bodyBytes = data.mid(headerEnd + 4);
	QList<QByteArray> lines = headerBytes.split('\n');
    if (lines.isEmpty())
    {
        return;
    }
	QList<QByteArray> reqLineParts = lines.first().split(' ');
    if (reqLineParts.size() < 2)
    {
        return;
    }
	QString method = QString::fromUtf8(reqLineParts[0]).toUpper();
	QString path = QString::fromUtf8(reqLineParts[1]);

	QJsonDocument doc = QJsonDocument::fromJson(bodyBytes);
    if (doc.isNull() || !doc.isObject())
    {
		sendHttpResponse(socket, 400, "Bad Request", "Invalid JSON body");
        return;
    }

    QJsonObject obj = doc.object();
    QString licId = obj["license_id"].toString().trimmed(); // 获取计算机名
    if (licId.isEmpty()) return;
    QMutexLocker locker(&m_mutex);
    if (!m_socketToLicenseMap.contains(socket))
    {
        QSet<QString> uniqueIds;
        for (const auto value : m_socketToLicenseMap.values())
        {
            uniqueIds.insert(value);
        }

        if (!uniqueIds.contains(licId)) {
            int limit = DatabaseManager::instance()->getPurchasedCount();
            if (uniqueIds.size() >= limit) {
                SystemLogger::instance()->logMessage(QString(QStringLiteral("拒绝 ID:%1 | 授权已满"))
                    .arg(licId));
                m_buffers.remove(socket);
				sendHttpResponse(socket, 403, "Forbidden", "License limit reached");
                //socket->disconnectFromHost();
                return;
            }
        }
    }
    m_socketToLicenseMap[socket] = licId;
    QString clientIp = socket->peerAddress().toString();
    if (clientIp.startsWith("::ffff:")) clientIp = clientIp.mid(7);

    // 如果是该 Socket 第一次发包，说明是刚上线
    if (!m_socketToLicenseMap.contains(socket) && !licId.isEmpty())
    {
        m_socketToLicenseMap[socket] = licId;
        SystemLogger::instance()->logMessage(
            QString(QStringLiteral("客户端上线 | ID: %1 | IP: %2")).arg(licId).arg(clientIp)
        );
    }
    //locker.unlock();

    // 后续分发信号
    if (path == "/api/heartbeat") {
        emit heartBeatReceived(licId, clientIp);
    }
    else if (path == "/api/operation") {
        QString opName = obj["op_name"].toString();
        emit operationReceived(licId, clientIp, opName);
    }
    sendHttpResponse(socket, 200, "OK", "License send finished");
}
//新的客户端进来，先从m_socketToLicenseMap获取到所有已连接的客户端ip
//判断当前客户端的数量和许可证数量是否匹配，如果相等则拒绝连接
//

void LicenseMonitorServer::processPacket(QTcpSocket* socket, const QByteArray& data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) return;
     
    QJsonObject obj = doc.object();
    QString licId = obj["license_id"].toString().trimmed(); // 获取计算机名
    if (licId.isEmpty()) return;
    QString clientIp = socket->peerAddress().toString();
    if (clientIp.startsWith("::ffff:")) clientIp = clientIp.mid(7);

    QMutexLocker locker(&m_mutex);
    if (!m_socketToLicenseMap.contains(socket))
    {
        QSet<QString> uniqueIds;
        for (const auto value : m_socketToLicenseMap.values())
        {
            uniqueIds.insert(value);
        }

        if (!uniqueIds.contains(licId)) {
            int limit = DatabaseManager::instance()->getPurchasedCount();
            if (uniqueIds.size() >= limit) {
                SystemLogger::instance()->logMessage(QString(QStringLiteral("拒绝 ID:%1 | 授权已满"))
                    .arg(licId));
                m_buffers.remove(socket);
                socket->disconnectFromHost();
                return;
            }
        }

        m_socketToLicenseMap[socket] = licId;
        SystemLogger::instance()->logMessage(
            QString(QStringLiteral("客户端上线 | ID: %1 | IP: %2")).arg(licId).arg(clientIp)
        );
    }
    else
    {
        m_socketToLicenseMap[socket] = licId;
    }
	locker.unlock();

    // 后续分发信号
    int type = obj["type"].toInt();
    if (type == static_cast<int>(MsgType::HeartBeat)) {
        emit heartBeatReceived(licId, clientIp);
    }
    else if (type == static_cast<int>(MsgType::KeyOperation)) {
        QString opName = obj["op_name"].toString();
        emit operationReceived(licId, clientIp, opName);
    }
}

void LicenseMonitorServer::sendHttpResponse(QTcpSocket* socket, int statusCode, const QString& statusText, const QString& body)
{
	if (!socket || socket->state() != QAbstractSocket::ConnectedState) return;

    QByteArray response;
	response.append(QString("HTTP/1.1 %1 %2\r\n").arg(statusCode).arg(statusText).toUtf8());
	response.append("Content-Type: application/json\r\n");
	response.append(QString("Content-Length: %1\r\n").arg(body.toUtf8().size()).toUtf8());
	response.append("Connection: close\r\n");
	response.append("\r\n");
	response.append(body.toUtf8());

	socket->write(response);
	//socket->flush();
 //   socket->disconnectFromHost();  
}

void LicenseMonitorServer::sendResponse(QTcpSocket* socket, const QString& body)
{
	if (!socket || socket->state() != QAbstractSocket::ConnectedState) return;

	QJsonObject responseArray;
	responseArray["message"] = body;

	QByteArray responseData = QJsonDocument(responseArray).toJson(QJsonDocument::Compact);

    QByteArray header;
	QDataStream out(&header, QIODevice::WriteOnly);
	out.setByteOrder(QDataStream::BigEndian);

	out << MAGIC_NUMBER;
	out << static_cast<quint32>(responseData.size());

	socket->write(header + responseData);
	socket->flush();
}

void LicenseMonitorServer::onDisconnected(){
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if(!socket) return;

    QString clientIp = socket->peerAddress().toString();
    if(clientIp.startsWith("::ffff:")) clientIp = clientIp.mid(7);

    QString licId = m_socketToLicenseMap.value(socket, "");
    
    // 只有在白名单映射表里存在的（即通过了之前包拦截的），才打印断开日志
    if(!licId.isEmpty()) {

        m_socketToLicenseMap.remove(socket);
    }

    m_buffers.remove(socket);

    bool stillHasOtherProcess = m_socketToLicenseMap.values().contains(licId);
    if (!licId.isEmpty() && !stillHasOtherProcess) {
        emit clientStatusChanged(licId, false);
        SystemLogger::instance()->logMessage(QString(QStringLiteral("客户端断开连接 | ID: %1 | IP: %2"))
            .arg(licId).arg(clientIp));
    }
    socket->deleteLater();
}

void LicenseMonitorServer::onDisconnectedHttp()
{

}