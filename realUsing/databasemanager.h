#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QtSql/QSqlError>
#include <QVector>

//数据库模块
//该模块主要用来对数据库继续操作

struct ClientInfo{
    QString id;
    QString name;
};
//用来记录原始日志
struct RawLogs{
    qint64 time;//操作时间
    int type;//类型（在线/关键操作）
    QString opName;//操作名称
};
class DatabaseManager
{
public:
    static DatabaseManager* instance();

    //初始化数据库并创建表
    bool initDb(const QString &dbName = "monitor_data.db");
    
    //该函数用来读取白名单中的端口号和许可证数
    void syncWhitelistWithFile();

    //统计某个时间段内 concurrency_stats 表中 is_success = 1 的记录数
    //该函数用来统计活跃日天数，再计算活跃率时使用
    int getSystemActiveDaysInRange(QDate start, QDate end);
    //写入 心跳/关键操作 事件
    void logEvent(QString licenseId, int type, const QString &opName ="");

    //更新每日有效时长-每判定一个30min有效，调用该函数给时长加0.5h
    void addEffectiveTime(QString licenseId,QDate date, double hours = 0.5);

    //记录每日并发判定结果
    void saveConcurrencyVerdict(QDate date, bool success);

    //查询接口
    QList<QStringList> getExportData();

    //在Client表中增加新的client
    void updateClientName(QString licenseId, const QString &name);
    //从Client表中获取所有的客户端
    QList<ClientInfo> getAllHistoryClients();
    //获取某个客户端某天内的心跳和关键操作信息
    QList<RawLogs> getLogsByDate(QString licenseId, QDate date);

    //获取某个时间段内的有效使用时长
    double getTotalSystemHours(QDate start, QDate end);
    int getPurchasedCount() { return m_purchasedCount; }
    quint16 getServerPort() const { return m_serverPort; }

    //判断某一天是否是活跃日
    bool isDateActive(QDate date);
	QString getVersionNum() const { return m_versionNum; }

    //重新计算并更新有效时长
    void recalculateDailyUsage(QDate dateStart, QDate dateEnd);

private:
    DatabaseManager();
    static DatabaseManager* m_instance;
    QSqlDatabase m_db;

    int m_purchasedCount = 0;
    quint16 m_serverPort = 8080;
    QString m_versionNum = "1.0.125";
};

#endif // DATABASEMANAGER_H
