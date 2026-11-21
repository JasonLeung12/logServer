#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QtGui/QPainter>
#include <QTableWidget>
#include <QCalendarWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QGroupBox>
#include <QHeaderView>
#include <QPushButton>
#include <QStackedWidget>
#include <QDateEdit>
#include <QLineEdit>
#include <QtNetwork/QTcpServer>
#include <QMessageBox>
#include "databasemanager.h"
#include "monitormanager.h"
#include "licensemonitorserver.h"
#include "concurrencychart.h"
#include <QSystemTrayIcon>


QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
} QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
private:
    void initSignalsSlot();
    void initUi();
    //更新客户端实时数据图表B，包括新增状态以及历史状态
    void updateDurationTableFromMemory();
    //加载历史数据更新图表B
    void loadHistoryDataToUI(const QDate &date);
    //更新历史表格数据
    void updateHistoryTable();
    //开机自动启动
    void setAutoStart(bool enable);

public:
    // 检查服务器是否启动成功
    bool isServerRunning() const { return tcpServer && tcpServer->isListening(); }

protected:
    void closeEvent(QCloseEvent *event) override;

private Q_SLOTS:
    void on_buttonExport_clicked();
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);
    void forceQuit();


private:
    QCalendarWidget* calendar;//日历
    QPushButton* buttonRealTime;//实时数据
    QPushButton* buttonHistory;//历史数据
    QPushButton* buttonExport;//导出数据
    QLabel* todayTotalTime;
    QGroupBox* concurrencyBox;
    QGroupBox* durationBox;
    QStackedWidget* stack;
    //图表A
    ConcurrencyChart* concurrencyChart;
    //图表B
    QTableWidget* durationTable;
    //历史数据表格
    QTableWidget* activeTimeTable;
	//验收指标信息栏
    QTextEdit* m_infoText;
    //起始日期
    QDateEdit* startEdit;
    //终止日期
    QDateEdit* endEdit;
    //周期工作日总数
    QLineEdit* workDaysEdit;
    //选择的周期内的月数
    QLineEdit* workMonthsEdit;
    //信息栏-用于显示操作过程中的信息
    QPlainTextEdit* logMsg;

private:
    Ui::MainWindow *ui;

    //TCP服务器
    LicenseMonitorServer* tcpServer;
    //ip和图标B中该ip的行数
    QMap<QString, int> m_idToRowMap; 
    QSystemTrayIcon *m_trayIcon;
    QMenu *m_trayMenu;
    bool m_isActuallyQuitting = false; // 用于区分“隐藏窗口”和“完全退出”
};

#endif // MAINWINDOW_H
