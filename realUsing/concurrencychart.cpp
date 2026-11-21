#include "concurrencychart.h"
#include "monitormanager.h"
#include <cmath>

ConcurrencyChart::ConcurrencyChart(QWidget *parent)
    : QWidget{parent}
{
    setMinimumHeight(250);

    // 启动定时器，每秒更新一次
    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, [this](){
        // 更新偏移量：当前时间 - 周期开始时间
        qint64 now = QDateTime::currentSecsSinceEpoch();

        update(); // 触发 paintEvent 重绘界面
    });
    m_refreshTimer->start(1000); // 1秒刷新一次

    setMouseTracking(true);
}

void ConcurrencyChart::paintEvent(QPaintEvent* event)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    auto allStates = MonitorDataManager::instance()->getAllStates();
    
    // 如果映射表没传过来或者为空，显示提示
    if (!m_rowMapping || m_rowMapping->isEmpty()) {
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待客户端接入..."));
        return;
    }

    int leftMargin = 180;
    int rowH = 40;
    int startY = 40;
    int chartW = this->width() - leftMargin - 150;

    // 动态调整高度：确保图表高度和表格行数匹配
    int maxRow = 0;
    for(int r : *m_rowMapping) if(r > maxRow) maxRow = r;
    this->setMinimumHeight(startY + (maxRow + 1) * rowH + 40);

    // 绘制背景刻度
    p.setPen(QColor(200, 200, 200));
    for (int h = 0; h <= 24; h += 2) {
        int x = leftMargin + (h * 3600.0 / 86400.0) * chartW;
        p.drawLine(x, startY - 10, x, startY + (maxRow + 1) * rowH);
        p.drawText(x - 15, startY - 15, QString("%1:00").arg(h));
    }

    // 遍历映射表来绘图，确保顺序与表格完全一致
    QMapIterator<QString, int> it(*m_rowMapping);
    while (it.hasNext()) {
        it.next();
        QString licId = it.key();  // 客户端 ID
        int rowIndex = it.value(); // 该客户端在表格中的行号

        if (!allStates.contains(licId)) continue;
        const auto& state = allStates[licId];

        // 关键点：Y 坐标由 rowIndex 决定，不再由 Map 的遍历顺序决定
        int y = startY + rowIndex * rowH;

        // --- 开始绘制这一行 ---
        p.setPen(Qt::black);
        QRect textRect(10, y, leftMargin - 35, rowH); 
        p.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, state.displayName);

        p.setPen(QColor(240, 240, 240));
        p.drawLine(leftMargin, y + rowH, leftMargin + chartW, y + rowH);

        // 绘制绿条
        p.setBrush(QColor(46, 204, 113, 100));
        p.setPen(Qt::NoPen);
        for (const auto& interval : state.current2hIntervals) {
            int xStart = timeToX(interval.startTime,chartW);
            int xEnd = timeToX(interval.endTime,chartW);
            p.drawRect(xStart, y + 10, qMax(xEnd - xStart, 2), 20);
        }

        // 绘制红点
        p.setBrush(Qt::red);
        for (const auto& op : state.current2hOps) {
			int dotX = timeToX(op.time, chartW);
            p.drawEllipse(QPoint(dotX, y + 20), 4, 4);
        }

        // 绘制在线状态灯
        if (state.isOnline) {
            p.setBrush(Qt::green);
            p.drawEllipse(leftMargin - 15, y + 15, 8, 8);
        }
        if (m_isActiveDay) {
            p.save();

            // 1. 定义勋章区域 (位于右侧页边距内)
            QRect badgeRect(width() - 120, 5, 120, 32);

            // 2. 绘制半透明背景
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(46, 204, 113, 220)); // 鲜绿色
            p.drawRoundedRect(badgeRect, 4, 4);

            // 3. 绘制文字
            p.setPen(Qt::white);
            QFont font = p.font();
            font.setBold(true);
            font.setPointSize(11);
            p.setFont(font);

            // 勾选符号 + 文本
            QString text = QChar(0x2714) + QStringLiteral("活跃日");
            p.drawText(badgeRect, Qt::AlignCenter, text);

            p.restore();
        }
    }

    //绘制当前时刻红线
    // 在函数末尾修改绘制红线的部分
    qint64 now = QDateTime::currentSecsSinceEpoch();
    int curX = timeToX(now,chartW);
    if (curX >= leftMargin && curX <= leftMargin + chartW) {
        p.setPen(QPen(Qt::red, 1, Qt::DashLine));
        
        // 计算图表区域的总底部坐标
        int totalBottom = startY + (maxRow + 1) * rowH;
        
        // 将红线终点设为 totalBottom
        p.drawLine(curX, startY - 20, curX, totalBottom);
    }
}

// 在 ConcurrencyChart 类中定义
int ConcurrencyChart::timeToX(qint64 timestamp, int chartWidth) {
    int margin = 180;
    //int chartWidth = this->width() - margin - 50; 
    
    // 1. 获取该时间戳对应当天的具体时间
    QDateTime dateTime = QDateTime::fromSecsSinceEpoch(timestamp);
    QTime time = dateTime.time();
    
    // 2. 计算从今天凌晨 00:00:00 开始经过的总秒数
    int secondsSinceMidnight = time.hour() * 3600 + time.minute() * 60 + time.second();
    
    // 3. 一整天总共有 86400 秒 (24 * 3600)
    int totalSecondsInDay = 86400;
    
    // 4. 计算比例并映射到像素
    double ratio = static_cast<double>(secondsSinceMidnight) / totalSecondsInDay;
    
    return margin + static_cast<int>(ratio * chartWidth);
}

void ConcurrencyChart::mouseMoveEvent(QMouseEvent* event)
{
    // 1. 安全检查
    if (!m_rowMapping || m_rowMapping->isEmpty()) return;

    auto allStates = MonitorDataManager::instance()->getAllStates();

    // --- 核心修改：定义与 paintEvent 完全一致的布局常量 ---
    const int leftMargin = 180;
    const int rowH = 40;
    const int startY = 40;
    // 必须减去 150，确保与绘图区域宽度 100% 对齐
    const int chartW = this->width() - leftMargin - 150;

    bool found = false;
    static qint64 lastHitTime = -1;

    // 2. 遍历映射表
    QMapIterator<QString, int> it(*m_rowMapping);
    while (it.hasNext()) {
        it.next();
        QString licId = it.key();
        int rowIndex = it.value();

        if (!allStates.contains(licId)) continue;
        const auto& state = allStates[licId];

        // 计算这一行红点的中心 Y 坐标
        int centerY = startY + rowIndex * rowH + 20;

        // 快速过滤：如果鼠标 Y 坐标离这一行中心太远，跳过
        if (qAbs(event->pos().y() - centerY) > 15) {
            continue;
        }

        // 3. 检测该行内的每一个红点
        for (const auto& op : state.current2hOps) {
            // --- 核心修改：传递 chartW 参数 ---
            int centerX = timeToX(op.time, chartW);

            // 边界检查：如果该点由于时间原因被裁剪到了绘图区之外，则不触发提示
            if (centerX < leftMargin || centerX > leftMargin + chartW) {
                continue;
            }

            int dx = event->pos().x() - centerX;
            int dy = event->pos().y() - centerY;
            double distance = std::sqrt(static_cast<double>(dx * dx + dy * dy));

            // 命中检测（半径 7 像素内）
            if (distance < 7) {
                if (lastHitTime != op.time) {
                    QString timeStr = QDateTime::fromSecsSinceEpoch(op.time).toString("yyyy-MM-dd HH:mm:ss");
                    QString tip = QStringLiteral("<b>时间:</b> %1<br>"
                        "<b>操作:</b> %2<br>"
                        "<b>客户端:</b> %3")
                        .arg(timeStr)
                        .arg(op.name)
                        .arg(state.displayName);

                    QToolTip::showText(event->globalPos(), tip, this);
                    lastHitTime = op.time;
                }

                setCursor(Qt::PointingHandCursor);
                found = true;
                break;
            }
        }
        if (found) break;
    }

    if (!found) {
        if (lastHitTime != -1) {
            setCursor(Qt::ArrowCursor);
            QToolTip::hideText();
            lastHitTime = -1;
        }
    }
}

void ConcurrencyChart::setIsActiveDay(bool active)
{
    m_isActiveDay = active;
}