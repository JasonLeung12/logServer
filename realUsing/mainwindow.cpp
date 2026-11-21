#include "mainwindow.h"
//#include "ui_mainwindow.h"
#include <QFileDialog>
#include <QtNetwork/QUdpSocket>
#include "systemLogger.h"
#include <QSplitter>
#include <QSettings>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QMessageBox>



MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{

    setAutoStart(true);
    resize(1500,800);


    if(!DatabaseManager::instance()->initDb()){
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("无法初始化数据库，程序将退出"));
        return;
    }
    QString iconPath = QCoreApplication::applicationDirPath() + "/HsCAEHX.ico";
    //setWindowIcon(QIcon(iconPath));

    DatabaseManager::instance()->syncWhitelistWithFile();
    setWindowTitle(QStringLiteral("License使用实时统计系统  V%1").arg(DatabaseManager::instance()->getVersionNum()));
    stack = new QStackedWidget(this);
    initUi();

    tcpServer = new LicenseMonitorServer(this);
    initSignalsSlot();

    QList<ClientInfo> allClients = DatabaseManager::instance()->getAllHistoryClients();
    
    MonitorDataManager::instance()->initLicenses(allClients);

    durationTable->setRowCount(0);
    m_idToRowMap.clear();

    for(const auto& client : allClients){
        QString cid = client.id.trimmed();
        
        if(!m_idToRowMap.contains(cid)) {
            int row = durationTable->rowCount();
            durationTable->insertRow(row);
            
            // 设置侧边栏显示白名单里的【计算机名】
            durationTable->setVerticalHeaderItem(row, new QTableWidgetItem(client.name));
            
            // 初始化单元格
            for(int col = 0; col < 48; ++col) {
                durationTable->setItem(row, col, new QTableWidgetItem());
            }
            
            // 建立 IP 到行号的映射
            m_idToRowMap[cid] = row; 
            
            //从数据库恢复该 ID 今天的历史状态（图表A的绿条和图表B的蓝格子）
            MonitorDataManager::instance()->loadHistoryToState(cid, QDate::currentDate());
        }
    }

    // 加载当前日期（今天）的视图
    this->loadHistoryDataToUI(QDate::currentDate());
    quint16 listenPort = DatabaseManager::instance()->getServerPort();
    int purchasedCount = DatabaseManager::instance()->getPurchasedCount();
    // 启动 TCP 监听
    if (purchasedCount <= 0) {
        QMessageBox::critical(this, QStringLiteral("配置错误"),
            QStringLiteral("授权数(LicenseNum)不能为0，请检查配置文件 whitelist.txt。"));
    }
    else {
        if (!tcpServer->startServer(listenPort)) {
            QMessageBox::warning(this, QStringLiteral("启动失败"),
                QStringLiteral("服务器端口 %1 可能被占用，监控功能无法正常工作。").arg(listenPort));
        }
    }

    //托盘图标设置（保持不变）
    m_trayIcon = new QSystemTrayIcon(this);
    //QString iconPath = QCoreApplication::applicationDirPath() + "/HsCAEHX.ico";
    QIcon trayIcon(iconPath);
    if (trayIcon.isNull() || trayIcon.availableSizes().isEmpty()) {
        m_trayIcon->setIcon(style()->standardIcon(QStyle::SP_MessageBoxInformation));
    } else {
        m_trayIcon->setIcon(trayIcon);
    }
    m_trayIcon->setToolTip(QStringLiteral("License 监控系统 - 运行中"));

    m_trayMenu = new QMenu(this);
    QAction *restoreAction = m_trayMenu->addAction(QStringLiteral("显示主界面"));
    QAction *quitAction = m_trayMenu->addAction(QStringLiteral("彻底退出程序"));
    connect(restoreAction, &QAction::triggered, this, &MainWindow::showNormal);
    connect(quitAction, &QAction::triggered, this, &MainWindow::forceQuit);
    m_trayIcon->setContextMenu(m_trayMenu);
    m_trayIcon->show(); 
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::onTrayIconActivated);

    MonitorDataManager::instance()->auditDayConcurrency(QDate::currentDate());
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::initUi()
{
    QWidget* centralWidget = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);

    //左侧日历
    QVBoxLayout* leftLayout = new QVBoxLayout();
    buttonRealTime = new QPushButton(QStringLiteral("实时数据"), this);
    buttonHistory = new QPushButton(QStringLiteral("历史数据"), this);
    buttonExport = new QPushButton(QStringLiteral("导出数据"), this);
    QHBoxLayout* v0 = new QHBoxLayout();
    QLabel* label0 = new QLabel();
    label0->setText(QString(QStringLiteral("许可证数量")));
    QLineEdit* lineEdit0 = new QLineEdit();
    lineEdit0->setText(QString::number(DatabaseManager::instance()->getPurchasedCount()));
    lineEdit0->setReadOnly(true);
    v0->addWidget(label0);
    v0->addWidget(lineEdit0);
    v0->addStretch();
    calendar = new QCalendarWidget();
    calendar->setMaximumHeight(calendar->sizeHint().height() + 50);
    calendar->setMinimumHeight(calendar->sizeHint().height() + 50);
    calendar->setFixedWidth(300);
    leftLayout->addWidget(new QLabel(QStringLiteral("历史记录溯源(选定日期)")));
    leftLayout->addWidget(calendar);
    leftLayout->addWidget(buttonRealTime);
    leftLayout->addWidget(buttonHistory);
    leftLayout->addWidget(buttonExport);
    leftLayout->addLayout(v0);
    leftLayout->addStretch();

    QWidget* leftWidget = new QWidget();
    leftWidget->setLayout(leftLayout);

    //右侧图表区
    QSplitter* chartSplitter = new QSplitter(Qt::Vertical);
    concurrencyBox = new QGroupBox(QStringLiteral("图表A:今日活跃(2小时采样周期-该License用户有关键操作) 注：红点表示用户有关键操作，绿色背景表示该用户在线，白色表示不在线"));
    QVBoxLayout* v1 = new QVBoxLayout(concurrencyBox);
    concurrencyChart = new ConcurrencyChart(); // 必须给成员变量赋值
    v1->setContentsMargins(0,50,0,0);
    v1->addWidget(concurrencyChart);
    concurrencyChart->setRowMapping(&m_idToRowMap);

    //
    //有效使用时长表
    durationBox = new QGroupBox(QStringLiteral("图表B:今日有效使用时长(30min采样片段)-License在该时间段内有关键操作即计入有效使用时长 注：深蓝色背景表示用户在该时间段内进行了关键操作"));
    durationTable = new QTableWidget(0,48,this);
    QStringList headers;
    for(int h = 0; h < 24; ++h) {
        headers << QString("%1:00").arg(h, 2, 10, QChar('0'));
        headers << QString("%1:30").arg(h, 2, 10, QChar('0'));
    }
    //禁止编辑
	durationTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    durationTable->setHorizontalHeaderLabels(headers);
    durationTable->horizontalHeader()->setDefaultSectionSize(60); 

    //有效使用时长统计
    todayTotalTime = new QLabel(durationBox);

    //增加可拉伸
    QSplitter* boxSplitter = new QSplitter(Qt::Vertical);
    boxSplitter->addWidget(durationTable);
    boxSplitter->addWidget(todayTotalTime);
    boxSplitter->setStretchFactor(0, 1);
    boxSplitter->setStretchFactor(1, 1);

    QVBoxLayout* v2 = new QVBoxLayout(durationBox);
    v2->addWidget(boxSplitter);
    v2->setContentsMargins(0, 30, 0, 0);
    durationBox->setLayout(v2);
    //
    chartSplitter->addWidget(concurrencyBox);
    chartSplitter->addWidget(durationBox);

    chartSplitter->setStretchFactor(0, 1);
    chartSplitter->setStretchFactor(1, 2);

    QWidget* realTimePage = new QWidget();
    QVBoxLayout* realTimePageLayout = new QVBoxLayout(realTimePage);
    realTimePageLayout->setContentsMargins(0,0,0,0);
    realTimePageLayout->addWidget(chartSplitter);
    realTimePage->setLayout(realTimePageLayout);
    stack->addWidget(realTimePage);

    //页面 B：历史数据页
    QWidget* historyPage = new QWidget();
    QVBoxLayout* historyLayout = new QVBoxLayout(historyPage);
    historyLayout->addWidget(new QLabel(QStringLiteral("历史数据视图：基于选定周期的 License 使用率统计")));

    //筛选条件组合框
    QGroupBox* filter = new QGroupBox(QStringLiteral("统计参数设置"));
    QVBoxLayout* filterMainLayout = new QVBoxLayout(filter); // 唯一的顶级布局
    filterMainLayout->setContentsMargins(10, 30, 10, 10);
    filterMainLayout->setSpacing(15);

    // 第一行：日期选择
    QHBoxLayout* row1 = new QHBoxLayout();
    row1->addWidget(new QLabel(QStringLiteral("起始日期:")));
    startEdit = new QDateEdit(QDate::currentDate().addDays(-30));
    startEdit->setCalendarPopup(true);
    //
    connect(startEdit, &QDateEdit::dateChanged, this, [this](const QDate& date) {
        int days = date.daysTo(endEdit->date());
        if (days % 30 != 0)
        {
            workMonthsEdit->setText(QString::number((days / 30) + 1));
            updateHistoryTable();
            endEdit->setMinimumDate(startEdit->date());
        }
        if (days % 30 == 0)
        {
            workMonthsEdit->setText(QString::number(1));
            updateHistoryTable();
            endEdit->setMinimumDate(startEdit->date());
        }
        });
    row1->addWidget(startEdit);
    row1->addSpacing(20);

    row1->addWidget(new QLabel(QStringLiteral("终止日期:")));
    endEdit = new QDateEdit(QDate::currentDate());

    endEdit->setCalendarPopup(true);
    row1->addWidget(endEdit);
    connect(endEdit, &QDateEdit::dateChanged, this, [this](const QDate& date) {
        int days = startEdit->date().daysTo(date);
        if (days % 30 != 0)
        {
            workMonthsEdit->setText(QString::number((days / 30) + 1));
            updateHistoryTable();
            startEdit->setMaximumDate(endEdit->date());
        }
        if (days % 30 == 0)
        {
            workMonthsEdit->setText(QString::number(1));
            updateHistoryTable();
            startEdit->setMaximumDate(endEdit->date());
        }
        });
    row1->addStretch();
    filterMainLayout->addLayout(row1); // 添加到主布局

    startEdit->setMaximumDate(endEdit->date());
    endEdit->setMinimumDate(startEdit->date());

    // 第二行：参数输入
    QHBoxLayout* row2 = new QHBoxLayout();
    row2->addWidget(new QLabel(QStringLiteral("周期工作日总数:")));
    workDaysEdit = new QLineEdit("21.75");
    workDaysEdit->setFixedWidth(60);
    row2->addWidget(workDaysEdit);

    row2->addSpacing(20);

    row2->addWidget(new QLabel(QStringLiteral("本周期总月数:")));
    workMonthsEdit = new QLineEdit();
    workMonthsEdit->setFixedWidth(60);
    row2->addWidget(workMonthsEdit);
    row2->addStretch();
    filterMainLayout->addLayout(row2); // 添加到主布局
    // 计算初始的月份数
    int days = endEdit->date().daysTo(startEdit->date());
    auto aaa = days % 30;
    if (days % 30 != 0)
    {
        workMonthsEdit->setText(QString::number((days / 30) + 1));
    }
    if (days % 30 == 0)
    {
        workMonthsEdit->setText(QString::number(1));
    }

    connect(workMonthsEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        updateHistoryTable();
        });

    historyLayout->addWidget(filter);

    // 历史统计表格
    activeTimeTable = new QTableWidget(0, 6); // 必须指定行列数
    activeTimeTable->setHorizontalHeaderLabels({ QStringLiteral("活跃日天数"), QStringLiteral("月活率=总活跃日/总工作日(%)"), QStringLiteral("平均有效使用时长(h)"), QStringLiteral("有效使用总时长(h)"), QStringLiteral("月活率是否达标(>=60%)"),QStringLiteral("有效使用时间是否达标(>=60h)")});
    activeTimeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); // 自动拉伸

    activeTimeTable->setEditTriggers(QAbstractItemView::NoEditTriggers); // 禁止触发编辑
    activeTimeTable->setFocusPolicy(Qt::NoFocus);                        // 禁止获取焦点（去掉虚线框）
    activeTimeTable->setSelectionMode(QAbstractItemView::NoSelection);

    m_infoText = new QTextEdit;
    m_infoText->setReadOnly(true);
    QString filePath = QCoreApplication::applicationDirPath() + "/calculationRules.txt";
    QFile info(filePath);
    if (!info.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("无法打开验收文件"));
        return;
    }
    QTextStream in(&info);
    in.setCodec("UTF-8");

    QString content = in.readAll();
    m_infoText->setPlainText(content);

    historyLayout->addWidget(activeTimeTable);
    historyLayout->addWidget(m_infoText);
    historyLayout->addStretch(); // 底部推起
    historyLayout->setStretchFactor(activeTimeTable, 1);
    historyLayout->setStretchFactor(m_infoText, 2);

    stack->addWidget(historyPage);

    QSplitter* mainSplitter = new QSplitter(Qt::Horizontal);
    mainSplitter->addWidget(leftWidget);
    mainSplitter->addWidget(stack);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    //底部日志栏
    logMsg = new QPlainTextEdit(this);
    logMsg->setReadOnly(true);
    //logMsg->setMaximumHeight(100);
    logMsg->setStyleSheet("background-color:#F5F5F5;border:1px solid #CCC;");

    QSplitter* verticalSplitter = new QSplitter(Qt::Vertical);
    verticalSplitter->addWidget(mainSplitter);
    verticalSplitter->addWidget(logMsg);
    verticalSplitter->setStretchFactor(0, 8); // 主内容区占大头
    verticalSplitter->setStretchFactor(1, 1); // 日志栏占小头

    mainLayout->addWidget(verticalSplitter);

    connect(SystemLogger::instance(), &SystemLogger::logMessageReceived, this, [this](const QString& msg){
        logMsg->appendPlainText(msg);//显示日志信息
    });

    // 在 initSignalsSlot 中增加信号连接
    // 发现新客户端时，动态给 durationTable 增加一行
    connect(MonitorDataManager::instance(), &MonitorDataManager::clientAdded, this, [this](QString licId, QString name) {
        if (!m_idToRowMap.contains(licId)) {
            int row = durationTable->rowCount();
            durationTable->insertRow(row);
            // 设置侧边栏显示名字（IP/计算机名）
            durationTable->setVerticalHeaderItem(row, new QTableWidgetItem(name));
            // 初始化该行的单元格（如果不 new，后面 item(row, col) 会返回空指针导致崩溃）
            for (int col = 0; col < 48; ++col) {
                QTableWidgetItem* item = new QTableWidgetItem();
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable); // 禁止编辑
                durationTable->setItem(row, col, item);
            }

            // 记录 ID 对应的行号 
            m_idToRowMap[licId] = row;
            this->updateDurationTableFromMemory();
            concurrencyChart->update();
            qDebug() << "UI: Dynamic row added for" << name << "at row" << row;
        }
        });

    setCentralWidget(centralWidget);

    this->setStyleSheet("QMainWindow{background-color:#F5F5F5;}"
                        "QGroupBox{font-weight:bold;border:1px solid #CCC;margin-top:10px;background:white;}"
                        "QTable{gridline-color:#EEE;}");
}

void MainWindow::initSignalsSlot()
{
    // 通信层 -> 逻辑处理层 (数据输入) ---
    connect(tcpServer, &LicenseMonitorServer::heartBeatReceived,
            MonitorDataManager::instance(), &MonitorDataManager::handleHeartbeat);
    connect(tcpServer, &LicenseMonitorServer::operationReceived,
            MonitorDataManager::instance(), &MonitorDataManager::handleOperation);
    connect(buttonExport, &QPushButton::clicked, this, &MainWindow::on_buttonExport_clicked);
    // 连接服务器的断开信号到逻辑层的下线处理
    connect(tcpServer, &LicenseMonitorServer::clientStatusChanged, 
            MonitorDataManager::instance(), [this](QString id, bool online){
        if(!online) {
            MonitorDataManager::instance()->handleClientOffline(id);
        }
    });
    connect(MonitorDataManager::instance(), &MonitorDataManager::kpiUpdated, this, [this]() {
        // 只有当今日达标时，这个信号才会触发
        if (calendar->selectedDate() == QDate::currentDate()) {
            concurrencyChart->setIsActiveDay(true); // 直接设为 true，因为信号触发了肯定就是达标了
            concurrencyChart->update();
        }
        });
    // 情况 A: 内存基础数据变化（绿条生长、蓝格子亮起）
    connect(MonitorDataManager::instance(), &MonitorDataManager::dataChanged, this, [this](){
        // 刷新图表A
        if(concurrencyChart) concurrencyChart->update();
        // 如果当前是实时页面且日历选的是今天，刷新图表 B 格子
        if(stack->currentIndex() == 0 && calendar->selectedDate() == QDate::currentDate()) {
            this->updateDurationTableFromMemory();
        }
    });

    connect(MonitorDataManager::instance(), &MonitorDataManager::systemDateRollover, this, [this](QDate newDate) {
        calendar->setSelectedDate(newDate);

        // 更新图表标题（从“某年某月某日”变回“今日”）
        concurrencyBox->setTitle(QString(QStringLiteral("图表A:今日活跃(2小时采样周期-该License用户有关键操作) ...")));
        durationBox->setTitle(QString(QStringLiteral("图表B:今日有效使用时长(30min采样片段) ...")));

        // 强制加载新日期的数据视图（此时应为空轨迹）
        this->loadHistoryDataToUI(newDate);

        SystemLogger::instance()->logMessage(QStringLiteral("UI 视图已自动切换至今日。"));
        });

    connect(buttonRealTime,&QPushButton::clicked,this, [=](){
        calendar->setSelectedDate(QDate::currentDate());
        stack->setCurrentIndex(0);
        double total = 0;
        auto states = MonitorDataManager::instance()->getAllStates();
        for(auto s : states) {
            // 遍历 48 个格子统计 true 的数量 * 0.5
            for(int j=0; j<48; ++j) if(s.daySlots30m[j]) total += 0.5;
        }
        todayTotalTime->setText(QString(QStringLiteral("今日有效使用时长：%1小时")).arg(total));
        concurrencyBox->setTitle(QString(QStringLiteral("图表A:今日活跃(2小时采样周期-该License用户有关键操作) 注：红点表示用户有关键操作，绿色背景表示该用户在线，白色表示不在线")));
        durationBox->setTitle(QString(QStringLiteral("图表B:今日有效使用时长(30min采样片段)-License在该时间段内有关键操作即计入有效使用时长 注：深蓝色背景表示用户在该时间段内进行了关键操作")));
        this->updateDurationTableFromMemory(); // 切回实时页时刷新一次表格，确保显示最新数据
    });
    connect(calendar, &QCalendarWidget::clicked, this, [=](const QDate &date){
        //根据点击的日期生成前缀（如果是今天显示“今日”，否则显示具体日期）
        QString datePrefix = (date == QDate::currentDate()) ? QStringLiteral("今日") : date.toString(QStringLiteral("yyyy年MM月dd日"));

        //动态设置图表 A 的标题
        concurrencyBox->setTitle(QString(QStringLiteral("图表A:%1活跃(2小时采样周期-该License用户有关键操作) "
                                        "注：红点表示用户有关键操作，绿色背景表示该用户在线，白色表示不在线"))
                                .arg(datePrefix));

        //动态设置图表 B 的标题
        durationBox->setTitle(QString(QStringLiteral("图表B:%1有效使用时长(30min采样片段)-"
                                    "License在该时间段内有关键操作即计入有效使用时长 "
                                    "注：深蓝色背景表示用户在该时间段内进行了关键操作"))
                            .arg(datePrefix));

        //加载数据（这个函数内部会根据 date 刷新表格颜色和统计底部的总时长 Label）
        this->loadHistoryDataToUI(date);
        
        //如果是今天，刷新红点和绿条的显示（确保实时性）
        if(date == QDate::currentDate()) {
            this->updateDurationTableFromMemory();
        }
        concurrencyChart->update();
    });
    connect(buttonHistory, &QPushButton::clicked,this, [=]() {
        stack->setCurrentIndex(1);
        this->updateHistoryTable();
    });

    connect(startEdit, &QDateEdit::dateChanged, this, &MainWindow::updateHistoryTable);
    connect(endEdit, &QDateEdit::dateChanged, this, &MainWindow::updateHistoryTable);
    connect(workDaysEdit,&QLineEdit::textChanged, this, &MainWindow::updateHistoryTable);
    connect(workMonthsEdit, &QLineEdit::textChanged, this, &MainWindow::updateHistoryTable);
}

void MainWindow::updateDurationTableFromMemory() {
    if (calendar->selectedDate() != QDate::currentDate()) {
        return; 
    }
    const auto& states = MonitorDataManager::instance()->getAllStates();

    // 遍历所有已建立映射的客户端
    QMapIterator<QString, int> i(m_idToRowMap);
    while (i.hasNext()) {
        i.next();
        QString licId = i.key();
        int row = i.value();

        if (!states.contains(licId)) continue;
        const auto& state = states[licId];
        QTableWidgetItem* vHeader = durationTable->verticalHeaderItem(row);
        if (vHeader) {
            if (vHeader->text() != state.displayName) {
                vHeader->setText(state.displayName);
            }
        }
        else {
            durationTable->setVerticalHeaderItem(row, new QTableWidgetItem(state.displayName));
        }
        for (int col = 0; col < 48; ++col) {
            QTableWidgetItem *item = durationTable->item(row, col);
            if (!item) continue;

            if (state.daySlots30m[col]) {
                item->setBackground(QColor(0, 102, 204)); // 蓝色活跃
            } else {
                item->setBackground(Qt::white); // 白色空闲
            }
        }
    }
    double total = 0;
    for (auto s : states) {
        for (int j = 0; j < 48; ++j) {
            if (s.daySlots30m[j]) total += 0.5;
        }
    }
    todayTotalTime->setText(QString(QStringLiteral("今日有效使用总时长：%1 小时")).arg(total));
}

void MainWindow::updateHistoryTable() {
    QDate startDate = this->startEdit->date();
    QDate endDate = this->endEdit->date();
    DatabaseManager::instance()->recalculateDailyUsage(startDate, endDate);

    double workMonths = this->workMonthsEdit->text().toDouble();
    if(workMonths <= 0) workMonths = 1.0;
    double workDaysTotal = this->workDaysEdit->text().toDouble();
    if(workDaysTotal <= 0) workDaysTotal = 21.75;

    activeTimeTable->setRowCount(0);
    QList<ClientInfo> allClients = DatabaseManager::instance()->getAllHistoryClients();
    int purchasedCount = DatabaseManager::instance()->getPurchasedCount();
    if (purchasedCount <= 0) return;

    // 获取某个时间段内的的达标天数和总有效时长
    int systemConcurrentDays = DatabaseManager::instance()->getSystemActiveDaysInRange(startDate, endDate);
    double systemTotalHours = DatabaseManager::instance()->getTotalSystemHours(startDate, endDate);

    // 计算系统全局平均月时长
    double globalAvgMonthlyHours = (systemTotalHours / purchasedCount) / workMonths;

    //历史数据表格
    {
        int row = 0;
        activeTimeTable->insertRow(row);
        auto createItem = [](const QString& text) {
            QTableWidgetItem* item = new QTableWidgetItem(text);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setTextAlignment(Qt::AlignCenter); 
            return item;
            };
        //活跃日天数
        activeTimeTable->setItem(row, 0, createItem(QString::number(systemConcurrentDays)));
        //活跃率
        double mar = (systemConcurrentDays / (workDaysTotal * workMonths)) * 100.0;
        activeTimeTable->setItem(row, 1, createItem(QString("%1%").arg(mar, 0, 'f', 2)));
        //平均有效使用时长
        activeTimeTable->setItem(row, 2, createItem(QString("%1").arg(globalAvgMonthlyHours, 0, 'f', 2)));
        //有效使用总时长
        activeTimeTable->setItem(row, 3, createItem(QString("%1").arg(systemTotalHours, 0, 'f', 2)));
        bool isMarOk = (mar >= 60.0);
        QTableWidgetItem* marItem = createItem(isMarOk ? QStringLiteral("是") : QStringLiteral("否"));
        marItem->setForeground(isMarOk ? Qt::darkGreen : Qt::red);
        //月活率是否达标
        activeTimeTable->setItem(row, 4, marItem);

        bool isTimeOk = (globalAvgMonthlyHours >= 60.0); 
        QTableWidgetItem* hourItem = createItem(isTimeOk ? QStringLiteral("是") : QStringLiteral("否"));
        hourItem->setForeground(isTimeOk ? Qt::darkGreen : Qt::red);
        //有效使用时间是否达标
        activeTimeTable->setItem(row, 5, hourItem);

    }
}

void MainWindow::loadHistoryDataToUI(const QDate &date) {
    bool isToday = (date == QDate::currentDate());
    MonitorDataManager::instance()->setLiveMode(date == QDate::currentDate());
    // 清空表格背景色
    for(int r=0; r < durationTable->rowCount(); ++r) {
        for(int c=0; c < 48; ++c) {
            if(durationTable->item(r, c)) durationTable->item(r, c)->setBackground(Qt::white);
        }
    }

    //if (isToday)
    //{
    //    this->updateDurationTableFromMemory();
    //    bool isActive = DatabaseManager::instance()->isDateActive(date);
    //    concurrencyChart->setIsActiveDay(isActive);
    //    concurrencyChart->update();
    //    return;
    //}
     
    double allLicensesTotalDayHours = 0;
    QMapIterator<QString, int> i(m_idToRowMap);
    
    while (i.hasNext()) {
        i.next();
        QString licId = i.key(); 
        int row = i.value(); 

        // 调用修改后的函数，一次性填充内存中的所有状态（A和B）
        MonitorDataManager::instance()->loadHistoryToState(licId, date);
        
        // 直接从内存状态里取数据来画图表 B
        const auto& state = MonitorDataManager::instance()->getState(licId);
        for (int col = 0; col < 48; ++col) {
            if (state.daySlots30m[col]) {
                durationTable->item(row, col)->setBackground(QColor(0, 102, 204));
                allLicensesTotalDayHours += 0.5;
            }
        }
    }

    // 更新统计文字
    todayTotalTime->setText(QString(QStringLiteral("今日有效使用总时长：%1 小时"))
                                .arg(allLicensesTotalDayHours, 0, 'f', 1));

    bool isActive = DatabaseManager::instance()->isDateActive(date);
	concurrencyChart->setIsActiveDay(isActive);
    // 图表A重绘
    concurrencyChart->update(); 
}
void MainWindow::on_buttonExport_clicked() {
    // 弹出文件保存对话框
    QString defaultName = QString(QStringLiteral("License验收报告.csv"));

    QString fileName = QFileDialog::getSaveFileName(this,
        QStringLiteral("导出验收报告"),
        defaultName,
        QStringLiteral("CSV文件 (*.csv);;所有文件 (*)"));

    if (fileName.isEmpty()) return;

    // 准备写入文件
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("无法创建文件，请检查是否有写入权限。"));
        return;
    }

    //使用 QTextStream 写入，处理编码问题（Excel 默认识别 GBK 或带 BOM 的 UTF-8）
    QTextStream out(&file);
    out.setGenerateByteOrderMark(true); // 加上 BOM，确保 Excel 打开不乱码
    out.setCodec("UTF-8");

    // 写入表头
    out << QStringLiteral("日期,许可证ID,当日有效使用时长(h),全员并发是否达标(满足KPI 1的依据)\n");

    QList<QStringList> exportData = DatabaseManager::instance()->getExportData();

    if (exportData.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("数据库中暂无统计数据。"));
        file.close();
        return;
    }

    for (const QStringList &rowData : exportData) {
        out << rowData.join(",") << "\n";
    }

    file.close();

    QMessageBox::information(this, QStringLiteral("导出成功"),
                             QString(QStringLiteral("验收报告已生成至：\n%1\n\n该文件包含所有 License 的真用记录，可直接作为验收凭证。")).arg(fileName));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    //qDebug() << QStringLiteral("检测到窗口关闭请求，当前强制退出状态:") << m_isActuallyQuitting;

    if (!m_isActuallyQuitting) {
        this->hide(); // 隐藏窗口
        
        // 只有托盘图标可见时才发消息
        if (m_trayIcon && m_trayIcon->isVisible()) {
            m_trayIcon->showMessage(QStringLiteral("服务提醒"), QStringLiteral("系统监控正在后台运行..."));
        }
        
        event->ignore(); // 【关键】忽略关闭信号
        //qDebug() << QStringLiteral("窗口已成功拦截，程序继续运行。");
    } else {
        event->accept(); // 允许退出
    }
}

void MainWindow::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        this->showNormal();
        this->activateWindow();
    }
}

void MainWindow::forceQuit()
{
    m_isActuallyQuitting = true;
    QApplication::quit();
}

void MainWindow::setAutoStart(bool enable) {
    QString appName = QCoreApplication::applicationName();
    QString appPath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    
    // 写入注册表启动项
    QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    
    if (enable) {
        settings.setValue(appName, appPath);
    } else {
        settings.remove(appName);
    }
}