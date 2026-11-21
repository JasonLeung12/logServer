#include "databasemanager.h"
#include <QVector>
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QDir>
#include "systemLogger.h"
#include <QMessageBox>
#include "monitormanager.h"

DatabaseManager* DatabaseManager::m_instance = nullptr;

DatabaseManager* DatabaseManager::instance()
{
    if(!m_instance) m_instance = new DatabaseManager();
    return m_instance;
}

DatabaseManager::DatabaseManager(){}
 
bool DatabaseManager::initDb(const QString& dbName)
{
    m_db = QSqlDatabase::addDatabase("QSQLITE");
    QString dataDir = QProcessEnvironment::systemEnvironment().value("PUBLIC");
    if (dataDir.isEmpty())
    {
        dataDir = "C:/Users/Public";
    }
    const QString appDataDir = dataDir + "/HsCAELogServer";
    QDir dir;
    if (!dir.mkpath(appDataDir)) {
        qCritical() << "Failed to create directory:" << appDataDir;
    }

    const QString dbPath = appDataDir + "/" + dbName;
    m_db.setDatabaseName(dbPath);

    if (!m_db.open()) {
        QMessageBox::critical(nullptr, QStringLiteral("数据库错误"),
            QStringLiteral("无法打开数据库文件：%1")
            .arg(m_db.lastError().text()));
        return false;
    }
    QSqlQuery q;
    // 日志表格：记录每次事件对应的licenseId/时间/类型/操作名称
    // 可通过该表查询历史事件或者恢复在线片段和30min格点状态
    // 图表A和图表B的数据基础
    q.exec("CREATE TABLE IF NOT EXISTS raw_logs("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "license_id TEXT,"
        "time DATETIME,"
        "type INTEGER,"
        "op_name TEXT)"
    );
    // 记录每天每个liceense的有效时长
    // 历史数据【平均有效使用时长】的数据基础
    q.exec("CREATE TABLE IF NOT EXISTS license_daily_usage ("
        "date DATE, "
        "license_id TEXT, "
        "hours DOUBLE DEFAULT 0, "
        "PRIMARY KEY (date, license_id)"
        ")");
    // 并发统计表：记录每天是否发生过并发（即是否有过重叠的在线时段），即所有客户端是否同时在线
    //该表一共有两个字段，即日期和is_success，在服务器监控期间只要在图表A中判定某一个周期所有的license满足活跃日条件，那么该日对应的is_success值就为1
    //历史数据表中活跃日数据的来源就是该表
    q.exec("CREATE TABLE IF NOT EXISTS concurrency_stats("
        "date DATE PRIMARY KEY,"
        "is_success BOOLEAN)"
    );
    // 客户端信息表：记录licenseId和显示名称的映射关系
    // 只要有新的客户端与服务器连接，该表中就会插入新客户端的ip和计算机名
    //如果以前连过该服务器，那么不会重复插入数据
    q.exec(
        "CREATE TABLE IF NOT EXISTS clients("
        "license_id TEXT PRIMARY KEY, "
        "display_name TEXT)"
    );

    q.exec("PRAGMA journal_mode = WAL");
    return true;
}

int DatabaseManager::getSystemActiveDaysInRange(QDate start, QDate end) {
    if(!m_db.isOpen()) return 0;

    QSqlQuery q(m_db);
    q.prepare("SELECT COUNT(*) FROM concurrency_stats "
        "WHERE is_success = 1 AND date BETWEEN ? AND ? ");
    q.addBindValue(start.toString("yyyy-MM-dd"));
    q.addBindValue(end.toString("yyyy-MM-dd"));
    
    if (q.exec() && q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

void DatabaseManager::syncWhitelistWithFile() {
    QString filePath = QCoreApplication::applicationDirPath() + "/whitelist.txt";
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        SystemLogger::instance()->logMessage(QString(QStringLiteral("无法打开白名单文件，默认授权数为 0，端口默认 8080")));
        m_purchasedCount = 0;
        return;
    }

    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();

        // 忽略空行或可能的注释
        if (line.isEmpty() || line.startsWith("#")) {
            continue;
        }

        // 以等号分割键和值
        QStringList parts = line.split('=');
        if (parts.size() == 2) {
            QString key = parts[0].trimmed();
            QString value = parts[1].trimmed();

            // 解析 LicenseNum
            if (key == QStringLiteral("LicenseNum")) {
                m_purchasedCount = value.toInt();
            }
            // 解析 Port
            else if (key == QStringLiteral("Port")) {
                m_serverPort = static_cast<quint16>(value.toInt());
            }
            else if (key == QStringLiteral("VersionNum")) {
                m_versionNum = value;
            }
        }
    }

    file.close();
    SystemLogger::instance()->logMessage(QString(QStringLiteral("同步完成，当前受保护授权总数:%1,监听端口为:%2").arg(m_purchasedCount).arg(m_serverPort)));
}

void DatabaseManager::logEvent(QString licenseId, int type, const QString &opName){
    if(!m_db.isOpen())
    {
        qDebug() << "Database is not open!";
        return;
    }
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO raw_logs (license_id, time, type, op_name) VALUES(?, ?, ?, ?)");
    q.addBindValue(licenseId);
    q.addBindValue(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    q.addBindValue(type);
    q.addBindValue(opName);
    if(!q.exec()){
        qDebug() << "SQL Error:" << q.lastError().text();
        qDebug() << "Executed SQL:" << q.executedQuery();
    } else {
        qDebug() << "Successfully inserted log for ID:" << licenseId;
    }
}

void DatabaseManager::addEffectiveTime(QString licenseId, QDate date, double hours){
    if(!m_db.isOpen()) return;

    QSqlQuery q(m_db);
    q.prepare("INSERT INTO license_daily_usage (date, license_id, hours) VALUES (?, ?, ?) "
              "ON CONFLICT(date, license_id) DO UPDATE SET hours = hours + ?");
    q.addBindValue(date.toString("yyyy-MM-dd"));
    q.addBindValue(licenseId);
    q.addBindValue(hours);
    q.addBindValue(hours);
    
    if(!q.exec()){
        qDebug() << "DB Error (addEffectiveTime):" << q.lastError().text();
    } else {
        qDebug() << "DB Success: Duration updated for ID" << licenseId;
    }
}

void DatabaseManager::saveConcurrencyVerdict(QDate date, bool success){
    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO concurrency_stats (date, is_success) VALUES (?, ?)");
    q.addBindValue(date.toString("yyyy-MM-dd"));
    q.addBindValue(success);
    q.exec();
}

// 获取所有 License 的导出统计数据
QList<QStringList> DatabaseManager::getExportData() {
    QList<QStringList> data;
    QSqlQuery q(m_db);

    // 我们关联查询每日时长和每日并发判定结果
    // 这样导出的每一行都包含了：日期、License ID、有效时长、当天全员并发是否成功
    QString sql = "SELECT a.date, a.license_id, a.hours, b.is_success "
                  "FROM license_daily_usage a "
                  "LEFT JOIN concurrency_stats b ON a.date = b.date "
                  "ORDER BY a.date DESC, a.license_id ASC";

    if (q.exec(sql)) {
        while (q.next()) {
            QStringList row;
            row << q.value(0).toString(); // 日期
            row << q.value(1).toString(); // License ID
            row << q.value(2).toString(); // 有效时长
            row << (q.value(3).toBool() ? QStringLiteral("是") : QStringLiteral("否")); // 是否满足并发达标
            data.append(row);
        }
    }
    return data;
}

void DatabaseManager::updateClientName(QString licenseId, const QString &name)
{
    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO clients (license_id, display_name) VALUES (?, ?)");
    q.addBindValue(licenseId);
    q.addBindValue(name);
    if(!q.exec()) qDebug() << q.lastError().text();
}

QList<ClientInfo> DatabaseManager::getAllHistoryClients()
{
    QList<ClientInfo> list;
    QSqlQuery q(m_db);
    if(q.exec("SELECT license_id, display_name FROM clients GROUP BY license_id")){
        while(q.next()){
            // 删除空格
            QString id = q.value(0).toString().trimmed(); 
            list.append({id, q.value(1).toString().trimmed()});
        }
    }
    return list;
}

QList<RawLogs> DatabaseManager::getLogsByDate(QString licenseId, QDate date) {
    QList<RawLogs> logs;
    QSqlQuery q(m_db);
    
    QDateTime start = date.startOfDay();
    QDateTime end = start.addDays(1);

    q.prepare("SELECT time, type, op_name FROM raw_logs "
              "WHERE license_id = ? AND time >= ? AND time < ? "
              "ORDER BY time ASC");
    q.addBindValue(licenseId);
    q.addBindValue(start.toString("yyyy-MM-dd HH:mm:ss"));
    q.addBindValue(end.toString("yyyy-MM-dd HH:mm:ss"));
    
    if (q.exec()) {
        while (q.next()) {
            QDateTime dt = QDateTime::fromString(q.value(0).toString(), "yyyy-MM-dd HH:mm:ss");
            if(dt.isValid()) {
                logs.append({dt.toSecsSinceEpoch(), q.value(1).toInt(), q.value(2).toString()});
            }
        }
    } else {
         qDebug() << "getLogsByDate SQL Error:" << q.lastError().text();
    }
    return logs;
}

double DatabaseManager::getTotalSystemHours(QDate start, QDate end) {
    if(!m_db.isOpen()) return 0.0;

    QSqlQuery q(m_db);
    q.prepare("SELECT SUM(a.hours) FROM license_daily_usage a "
              "INNER JOIN clients b ON a.license_id = b.license_id "
              "WHERE a.date BETWEEN ? AND ? "); 

    q.addBindValue(start.toString("yyyy-MM-dd"));
    q.addBindValue(end.toString("yyyy-MM-dd"));

    if (q.exec() && q.next()) {
        return q.value(0).toDouble();
    }
    return 0.0;
}

bool DatabaseManager::isDateActive(QDate date)
{
    if (!m_db.isOpen())   return false;

    QSqlQuery q(m_db);
    q.prepare("SELECT is_success FROM concurrency_stats WHERE date = ?");
    q.addBindValue(date.toString("yyyy-MM-dd"));
    if (q.exec() && q.next())
    {
        return q.value(0).toBool();
    }
    return false;
}

void DatabaseManager::recalculateDailyUsage(QDate dateStart, QDate dateEnd)
{
    if (!m_db.isOpen()) return;

    QMap<QDate, QMap<QString, QSet<int>>> rangeSlots;

    {
        QSqlQuery q(m_db);
        q.prepare("SELECT license_id, time FROM raw_logs WHERE type = 1");
        if (!q.exec())
        {
            qDebug() << "DB Error (recalculateDailyUsage):" << q.lastError().text();
            return;
        }

        while (q.next()) {
            QString licId = q.value(0).toString().trimmed();
            QString timeStr = q.value(1).toString();
            QDateTime dt = QDateTime::fromString(timeStr, "yyyy-MM-dd HH:mm:ss");
            if (!dt.isValid()) continue;

            QDate date = dt.date();
            if (date < dateStart || date > dateEnd) continue;
            int slot = (dt.time().hour() * 60 + dt.time().minute()) / 30;
            if (slot >= 0 && slot < 48)
            {
                rangeSlots[date][licId].insert(slot);
            }
        }
        q.finish();
    }

    if (rangeSlots.isEmpty()) return;


    QSqlQuery insertSql(m_db);
    insertSql.prepare("INSERT INTO license_daily_usage (date, license_id, hours) VALUES (?, ?, ?) "
                      "ON CONFLICT(date, license_id) DO UPDATE SET hours = ?");


    bool transOk = m_db.transaction();

    for (auto dateIt = rangeSlots.begin(); dateIt != rangeSlots.end(); ++dateIt)
    {
        QDate date = dateIt.key();
        const auto& clientMap = dateIt.value();
        for (auto clientIt = clientMap.begin(); clientIt != clientMap.end(); ++clientIt)
        {
            QString licId = clientIt.key();
            int activeSlotCount = clientIt.value().size();
            double totalHours = activeSlotCount * 0.5;
            insertSql.prepare("INSERT INTO license_daily_usage (date, license_id, hours) VALUES (?, ?, ?) "
                "ON CONFLICT(date, license_id) DO UPDATE SET hours = ?");
            insertSql.addBindValue(date.toString("yyyy-MM-dd"));
            insertSql.addBindValue(licId);
            insertSql.addBindValue(totalHours);
            insertSql.addBindValue(totalHours);

            if (!insertSql.exec()) {
                qDebug() << "DB Error (recalculateRangeDailyUsage batch insert):" << insertSql.lastError().text();
            }
        }
    }

    if (transOk)
    {
        if (!m_db.commit()) {
            qDebug() << "Commit failed:" << m_db.lastError().text();
            m_db.rollback();
        }
    }
}