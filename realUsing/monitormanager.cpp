#include "monitormanager.h"

#include <QDebug>
#include "systemLogger.h"
#include <algorithm>

MonitorDataManager* MonitorDataManager::m_instance = nullptr;

MonitorDataManager* MonitorDataManager::instance() {
    if (!m_instance) m_instance = new MonitorDataManager();
    return m_instance;
}

MonitorDataManager::MonitorDataManager(QObject *parent) : QObject(parent) {
    m_mainTimer = new QTimer(this);
    connect(m_mainTimer, &QTimer::timeout, this, &MonitorDataManager::onOneSecondTimer);
    m_mainTimer->start(1000); // 每秒刷新一次状态
}

void MonitorDataManager::initLicenses(const QList<ClientInfo>& clients) {
    m_licenseMap.clear();
    for(const auto& info : clients) {
        QString compName = info.id.trimmed(); // 数据库 license_id -> 计算机名
        QString ipAddr = info.name.trimmed();         // 数据库 display_name -> IP 地址
        LicenseRuntimeState state;
        state.licenseId = compName; 
        
        state.displayName = ipAddr; 
        
        m_licenseMap[compName] = state;
    }
}

// 处理心跳：驱动图表A绿条生长
void MonitorDataManager::handleHeartbeat(QString licId, const QString& name) {
    licId = licId.trimmed();
    if (licId.isEmpty()) return;

    // 如果是全新的 ID（或服务器重启后首次看到该 ID）
    if (!m_licenseMap.contains(licId)) {
        LicenseRuntimeState newState;
        newState.licenseId = licId;
        newState.displayName = name;
        m_licenseMap[licId] = newState;

        // 在服务器重启时，加载连接该客户端今日的历史数据
        //将新接入的客户端对应的state进行初始化，避免出现脏数据
        loadHistoryToState(licId, QDate::currentDate());

        // 写入数据库 clients 表并通知 UI 增加行
        DatabaseManager::instance()->updateClientName(licId, name);
        emit clientAdded(licId, name);
    }

    // 无论是否是新 ID，都要更新最新的显示名称（应对 IP 变化）
    LicenseRuntimeState& state = m_licenseMap[licId];
    if (state.displayName != name) {
        state.displayName = name;
        // 如果 IP 变了，同步更新数据库记录和 UI 列表显示
        DatabaseManager::instance()->updateClientName(licId, name);
    }

    qint64 now = QDateTime::currentSecsSinceEpoch();
    state.lastHeartbeatTime = now;

    // 在线状态切换逻辑
    if (!state.isOnline) {
        state.isOnline = true;
        OnlineInterval interval;
        interval.startTime = now;
        interval.endTime = now + 1;
        state.current2hIntervals.append(interval);
    }
    else {
        // 持续在线，延伸当前绿条的终点
        if (!state.current2hIntervals.isEmpty()) {
            state.current2hIntervals.last().endTime = now;
        }
    }

    // 记录心跳到数据库，保证历史溯源准确
    DatabaseManager::instance()->logEvent(licId, 0, "Heartbeat");
    //通知 UI 刷新图表A和图表B
    emit dataChanged();
}


void MonitorDataManager::handleOperation(QString licId, const QString &name, const QString &opName) {
    QDateTime nowDt = QDateTime::currentDateTime();
    //qint64 nowSecs = nowDt.toSecsSinceEpoch();
    qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    
    if (!m_licenseMap.contains(licId)) {
        LicenseRuntimeState newState;
        newState.licenseId = licId;
        newState.displayName = name;
        m_licenseMap[licId] = newState;

        // 写入数据库 clients 表
        DatabaseManager::instance()->updateClientName(licId, name);

        loadHistoryToState(licId, QDate::currentDate());
        emit clientAdded(licId, name);
    }
    SystemLogger::instance()->logMessage(QString(QStringLiteral("接收到IP地址为:%1的关键操作: %2")).arg(name).arg(opName));
    LicenseRuntimeState &state = m_licenseMap[licId];
    //state.displayName = name;
    state.lastHeartbeatTime = nowSecs;

    state.current2hOps.append({nowSecs, opName});
    if(state.current2hOps.size() > 1000) state.current2hOps.removeFirst();

    int slot = (nowDt.time().hour() * 60 + nowDt.time().minute()) / 30;
    if (slot >= 0 && slot < 48) {
        if (!state.daySlots30m[slot]) {
            state.daySlots30m[slot] = true;
            DatabaseManager::instance()->addEffectiveTime(licId, nowDt.date(), 0.5);
        }
    }

    if (!state.isOnline) 
    {
        state.isOnline = true;
        OnlineInterval interval;
        interval.startTime = nowSecs;
        interval.endTime = nowSecs + 1;
        state.current2hIntervals.append(interval);
    }
    else 
    {
        if (!state.current2hIntervals.isEmpty()) {
            state.current2hIntervals.last().endTime = nowSecs;
        }
    }

    DatabaseManager::instance()->logEvent(licId, 1, opName); 
    //通知 UI 刷新图表A和图表B
    emit dataChanged(); 
}

void MonitorDataManager::onOneSecondTimer() {
    qint64 now = QDateTime::currentSecsSinceEpoch();
    QDate today = QDate::currentDate();

    // 掉线检测逻辑
    // 仅在实时模式下，检测心跳是否超时（30秒）
    if (m_isLiveMode) {
        bool statusChanged = false;
        for (auto& state : m_licenseMap) {
            if (state.isOnline && (now - state.lastHeartbeatTime > 60)) {
                state.isOnline = false;
                statusChanged = true;
            }
        }
        // 如果有状态变化，通知 UI 刷新图表A和图表B
        if (statusChanged) emit dataChanged();
    }

    // 凌晨内存清理
    // 为了支持图表 A 显示全天痕迹，数据不能每秒清理。
    // 只有在日期变更时，才清空内存，准备记录新的一天。
    static QDate lastProcessDate = QDate::currentDate();
    if (today != lastProcessDate) {
        m_lastVerdictDate = QDate();
        for (auto it = m_licenseMap.begin(); it != m_licenseMap.end(); it++)
        {
            LicenseRuntimeState& state = it.value();
            state.current2hIntervals.clear();
            state.current2hOps.clear();
            for (int i = 0; i < 48; i++)
            {
                state.daySlots30m[i] = false;
            }
        }
        lastProcessDate = today;
        emit dataChanged(); // 通知 UI 刷新图表A和图表B
		emit systemDateRollover(today); //通知 UI 日期变更，更改图表A和图表B的标题
        SystemLogger::instance()->logMessage(QStringLiteral("日期变更，清空昨日监控缓存。"));
    }  

    //实时模式拦截
    if (!m_isLiveMode) return;

    // 并发判定
    // 判定不再依赖于 onOneSecondTimer 的清理，而是交给 checkConcurrencyLogic 内部去筛选时间
    //判断今天是否是达标的日期/活跃日
    if (m_lastVerdictDate!=today && !DatabaseManager::instance()->isDateActive(today)) {

        // 执行重构后的判定算法
        if (this->checkConcurrencyLogic()) {
            m_lastVerdictDate = today;
            DatabaseManager::instance()->saveConcurrencyVerdict(today, true);
            emit kpiUpdated(); // 触发 UI 右上角勋章显示
            SystemLogger::instance()->logMessage(QStringLiteral("系统判定成功：满足 %1 人同时在线且活跃（10分钟累计），今日已达标。").arg(DatabaseManager::instance()->getPurchasedCount()));
        }
        // 每 5 分钟执行一次全天历史审计（防止内存判定因为极端断线情况漏掉）
        else if (now % 300 == 0) {
            this->auditDayConcurrency(today);
        }
    }
}

bool MonitorDataManager::checkConcurrencyLogic() {
    int N = DatabaseManager::instance()->getPurchasedCount();
    if (N <= 0) return false;

    qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 windowStart = now - 7200;

    //找出在当前 2 小时窗口内有操作的客户端
    //判断2小时内关键操作的客户端数量是否小于N
    //小于N则直接判定失败，不满足两小时内每个许可证都有关键操作的判定标准
    QVector<QString> qualifiedIds;
    for (auto it = m_licenseMap.begin(); it != m_licenseMap.end(); ++it) {
        for (const auto& op : it.value().current2hOps) {
            if (op.time >= windowStart) {
                qualifiedIds.append(it.key());
                break;
            }
        }
    }

    // 如果这 2 小时内“动过”的人数都不够 N，直接判定失败
    if (qualifiedIds.size() < N) return false;

    // 对所有的客户端进行在线统计,原因在于一个许可证槽位可以会有多个客户端占用
    //这里采用的是
    struct TimeEvent { qint64 time; int type; };
    QVector<TimeEvent> events;

    for (auto& state : m_licenseMap) {
        for (const auto& interval : state.current2hIntervals) {
            qint64 s = qMax(interval.startTime, windowStart);
            qint64 e = qMin(interval.endTime, now);
            if (e > s) {
                events.append({ s, 1 });
                events.append({ e, -1 });
            }
        }
    }

    std::sort(events.begin(), events.end(), [](const TimeEvent& a, const TimeEvent& b) { 
        return a.time != b.time ? a.time < b.time : a.type < b.type;
        });

    //统计活跃的客户端同时达到 N 的累积时长
    qint64 totalActiveConcurrentSecs = 0;
    int currentActiveCount = 0;
    qint64 lastEventTime = events.first().time;
    for (int i = 0; i < events.size(); ++i) {
        qint64 currentTime = events[i].time;
        // 只有当活跃且在线的人数 >= N 时，这段重叠时间才有效
        if (currentActiveCount >= N && i > 0) {
            totalActiveConcurrentSecs += (currentTime - lastEventTime);
        }
        currentActiveCount += events[i].type;
        lastEventTime = currentTime;
    }

    return (totalActiveConcurrentSecs >= 600);
}

void MonitorDataManager::handleClientOffline(QString licId) {
    if (m_licenseMap.contains(licId)) {
        m_licenseMap[licId].isOnline = false;
        // 记录最后一次在线的时间点，防止绿条无限延伸
        if (!m_licenseMap[licId].current2hIntervals.isEmpty()) {
            m_licenseMap[licId].current2hIntervals.last().endTime = QDateTime::currentSecsSinceEpoch();
        }
        emit dataChanged(); // 触发 UI 刷新，关掉绿灯
    }
}

void MonitorDataManager::loadHistoryToState(QString licId, QDate date) {
    if (!m_licenseMap.contains(licId)) {
        LicenseRuntimeState newState;
        newState.licenseId = licId;

        QList<ClientInfo> allHistory = DatabaseManager::instance()->getAllHistoryClients();
        for (const auto& client : allHistory) {
            if (client.id == licId) {
                newState.displayName = client.name;
                break;
            }
        }
        if (newState.displayName.isEmpty()) newState.displayName = licId;
        m_licenseMap[licId] = newState;
    }

    LicenseRuntimeState &state = m_licenseMap[licId];
    // 清空旧状态
    state.current2hOps.clear();
    state.current2hIntervals.clear();
    for(int i=0; i<48; ++i) state.daySlots30m[i] = false;

    QList<RawLogs> logs = DatabaseManager::instance()->getLogsByDate(licId, date);
    if (logs.isEmpty()) return;
    //获取关键操作图表A和图表B
    for (const auto& log : logs) {
        if (log.type == 1) { // 关键操作
            state.current2hOps.append({log.time, log.opName});
            
            // 同步内存中的 30min 格子，确保图表 B 绘制准确
            QDateTime dt = QDateTime::fromSecsSinceEpoch(log.time);
            int slot = (dt.time().hour() * 60 + dt.time().minute()) / 30;
            if (slot >= 0 && slot < 48) state.daySlots30m[slot] = true;
        }
    }
    
    // 恢复在线信息
    OnlineInterval currentInterval;
    currentInterval.startTime = logs.first().time;
    currentInterval.endTime = logs.first().time + 1;
    for (int i = 1; i < logs.size(); i++) {
        if (logs[i].time - currentInterval.endTime <= 60) {
            currentInterval.endTime = logs[i].time;
        } else {
            state.current2hIntervals.append(currentInterval);
            currentInterval.startTime = logs[i].time;
            currentInterval.endTime = logs[i].time + 1;
        }
    }
    state.current2hIntervals.append(currentInterval);
    
    // 历史模式下关闭在线灯
    if(date!= QDate::currentDate())
    {
        state.isOnline = false;
    }
}

void MonitorDataManager::auditDayConcurrency(QDate date) {
    if (DatabaseManager::instance()->isDateActive(date)) return;

    int N = DatabaseManager::instance()->getPurchasedCount();
    if (N <= 0) return;

    //获取当天所有出现过的 License ID（包括那些因为重启或换机器产生的新 ID）
    QList<ClientInfo> clients = DatabaseManager::instance()->getAllHistoryClients();
    // 如果全天出现过的总人数连 N 都不到，这一天绝对不可能达标
    if (clients.size() < N) return;

    struct TempState {
        QVector<OnlineInterval> intervals;
        QVector<qint64> opTimes;
    };
    QMap<QString, TempState> dayData;

    for (const auto& c : clients) {
        // 获取该 ID 在这一天的所有原始日志
        QList<RawLogs> logs = DatabaseManager::instance()->getLogsByDate(c.id, date);
        if (logs.isEmpty()) continue;

        OnlineInterval curInt = { logs.first().time, logs.first().time + 1  };
		if (logs.first().type == 1) dayData[c.id].opTimes.append(logs.first().time);
        for (int i =  1; i < logs.size(); ++i) {
            if (logs[i].time - curInt.endTime <= 70) {
                curInt.endTime = logs[i].time;
            }
            else {
                dayData[c.id].intervals.append(curInt);
                curInt = { logs[i].time, logs[i].time + 1 };
            }
            // 记录关键操作的时间点
            if (logs[i].type == 1) dayData[c.id].opTimes.append(logs[i].time);
        }
        dayData[c.id].intervals.append(curInt);
    }

    // 以5分钟为步进，滑动 2 小时窗口
    QDateTime startPoint(date, QTime(0, 0));
	qint64 dayEndSecs = QDateTime(date.addDays(1), QTime(0, 0)).toSecsSinceEpoch();
    while (startPoint.addSecs(7200).date() == date) {
        qint64 winStart = startPoint.toSecsSinceEpoch();
        qint64 winEnd = qMin(winStart + 7200, dayEndSecs);

        QStringList qualifiedIds;
        for (auto it = dayData.begin(); it != dayData.end(); ++it) {
            for (qint64 t : it.value().opTimes) {
                if (t >= winStart && t <= winEnd) {
                    qualifiedIds.append(it.key());
                    break;
                }
            }
        }

        if (qualifiedIds.size() < N)    return;

        struct TimeEvent { qint64 time; int type; };
        QVector<TimeEvent> events;
        for (const auto& id : clients) {
            for (const auto& interval : dayData[id.name].intervals) {
                qint64 s = qMax(interval.startTime, winStart);
                qint64 e = qMin(interval.endTime, winEnd);
                if (e > s) {
                    events.append({ s, 1 }); 
                    events.append({ e, -1 });
                }
            }
        }

        // 时间线排序
        std::sort(events.begin(), events.end(), [](const TimeEvent& a, const TimeEvent& b) {
            return a.time != b.time ? a.time < b.time : a.type < b.type;
            });

        qint64 totalOverlapSecs = 0;
        int currentActiveOnline = 0;
        qint64 lastEventTime = events.first().time;
        for (int i = 0; i < events.size(); ++i) {
            qint64 currentTime = events[i].time;
            if (currentActiveOnline >= N && i > 0) {
                totalOverlapSecs += (currentTime - lastEventTime);
            }
            currentActiveOnline += events[i].type;
            lastEventTime = currentTime;
        }

        // 如果在这个 2 小时窗口内，活跃坑位重叠时长达到 10 分钟 (600秒)
        if (totalOverlapSecs >= 600) {
            DatabaseManager::instance()->saveConcurrencyVerdict(date, true);
            emit kpiUpdated(); // 通知 UI 刷新（右上角勋章）
            SystemLogger::instance()->logMessage(
                QStringLiteral("历史审计成功：[%1 %2] 窗口满足 %3 个人同时活跃且并发 10 分钟。")
                .arg(date.toString("yyyy-MM-dd"))
                .arg(startPoint.toString("HH:mm"))
                .arg(N)
            );
            return;
        }

        startPoint = startPoint.addSecs(300); // 步进 5 分钟继续扫描
    }
}