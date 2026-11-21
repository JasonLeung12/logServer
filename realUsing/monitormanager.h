#ifndef MONITORMANAGER_H
#define MONITORMANAGER_H

#include <QObject>
#include <QDateTime>
#include <QVector>
#include <QMap>

#include <QObject>
#include <QMap>
#include <QVector>
#include <QDateTime>
#include <QTimer>
#include "databasemanager.h"

// 基础数据结构
struct OnlineInterval {
    qint64 startTime;
    qint64 endTime;
};

struct opRecord{
    qint64 time;
    QString name;
};

//用来记录每个客户端的状态
struct LicenseRuntimeState {
    QString licenseId;//计算机名
    QString displayName; //ip地址
    //在线状态
    bool isOnline = false;
    qint64 lastHeartbeatTime = 0;

    // 图表A逻辑：2h内的在线片段和操作点
    QVector<OnlineInterval> current2hIntervals;
    QVector<opRecord> current2hOps;
    // 图表B逻辑：30min采样格点 (0-47代表一天的48个半小时)
    // true表示在该30min格子内有关键操作
    bool daySlots30m[48] = {false};
};

//该类是整个系统的核心类，负责管理/判定所有客户端的状态和数据
class MonitorDataManager : public QObject {
    Q_OBJECT

public:
    static MonitorDataManager* instance();

    // 初始化监控列表m_licenseMap
    void initLicenses(const QList<ClientInfo>& clients);

    // 接收来自 License模块的信号
    void handleHeartbeat(QString licId, const QString &name);
    void handleOperation(QString licId, const QString &name, const QString &opName);

    // 获取数据接口
    const QMap<QString, LicenseRuntimeState>& getAllStates() const { return m_licenseMap; }
    const LicenseRuntimeState& getState(QString licId) { return m_licenseMap[licId]; }

    //下线处理
    void handleClientOffline(QString licId);
    //加载某天的历史数据
    void loadHistoryToState(QString licId, QDate date);

    //该函数是对某天的数据进行溯源计算兜底
    //扫描线算法
    void auditDayConcurrency(QDate date);

    void setLiveMode(bool live) { m_isLiveMode = live; }

signals:
    void dataChanged(); // 驱动 UI 重绘
    void kpiUpdated();  // 驱动 KPI 卡片刷新
    void clientAdded(QString licenseId, const QString &displayName);//发现一个新客户端时触发
    void systemDateRollover(QDate newDate);

private slots:
    // 每秒轮询：检测是否掉线/活跃日判定/清理图表数据(凌晨)
    void onOneSecondTimer();

private:
    explicit MonitorDataManager(QObject *parent = nullptr);
    static MonitorDataManager* m_instance;

    //用来存储所有在数据库中保存的客户端的状态和信息
    //这个变量是这个类的核心变量
    QMap<QString, LicenseRuntimeState> m_licenseMap;
    QTimer *m_mainTimer;

    // 核心判定算法-扫描线算法
    //void calculateEffectiveTime(QString licId);
    bool checkConcurrencyLogic(); // 判定当前2h是否满足全员10min在线+操作
    //上次判定达标的日期
    QDate m_lastVerdictDate;
	//当前现实的数据是实时模式还是历史模式，实时模式下才会触发并发判定
    bool m_isLiveMode = true;
};

#endif
