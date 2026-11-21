#ifndef CONCURRENCYCHART_H
#define CONCURRENCYCHART_H

#include <QtWidgets/QWidget>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QTimer>
#include <QDateTime>
#include <QtWidgets/QToolTip>
#include <Qt3DInput/QMouseEvent>

struct OnlineTimeInterval {
    int startTime; // 相对采样开始的秒数
    int endTime;   // 相对采样开始的秒数
};

struct LicenseState {
    QList<OnlineTimeInterval> intervals; // 所有的在线片段
    QList<int> keyOpTimes;           // 所有的操作时间点
    bool isCurrentlyOnline;          // 当前是否在线
};

class ConcurrencyChart : public QWidget
{
    Q_OBJECT
public:
    explicit ConcurrencyChart(QWidget *parent = nullptr);
    ~ConcurrencyChart()
    {

    };

public:
    void setRowMapping(const QMap<QString, int>* mapping) { m_rowMapping = mapping; }
    void setIsActiveDay(bool active);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    QTimer *m_refreshTimer;              // 驱动界面重绘和时间增长
    const QMap<QString, int>* m_rowMapping = nullptr;
    bool m_isActiveDay = false;

private:
    int timeToX(qint64 timestamp, int chartWidth);


signals:
};

#endif // CONCURRENCYCHART_H
