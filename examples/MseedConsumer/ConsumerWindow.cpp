#include "ConsumerWindow.h"
#include "MseedConsumer.h"
#include "NetworkReplyTimeout.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QBrush>
#include <QColor>

namespace {
// 控制台接口都是轻量 JSON 请求，15 秒没有任何响应就判定失败并重试，
// 避免 Qt 5.12 下请求悬挂导致界面一直停在“上一次结果”。
const int kRequestTimeoutMs = 15000;
// 完全相同的网络错误 20 秒内只记一次，防止 5 秒轮询把日志刷满。
const qint64 kErrorRepeatMs = 20000;

// 服务端 last_seen_at 来自 SQLite CURRENT_TIMESTAMP，是 UTC 的
// "yyyy-MM-dd HH:mm:ss" 且不带时区标记，必须按 UTC 解析后再转本地时间。
QDateTime deviceUtcTime(const QString& text)
{
    QDateTime dt = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (dt.isValid())
        dt.setTimeSpec(Qt::UTC);
    return dt;
}

QString relativeAge(const QDateTime& utcTime)
{
    if (!utcTime.isValid())
        return QStringLiteral("无心跳记录");
    const qint64 secs = utcTime.secsTo(QDateTime::currentDateTimeUtc());
    if (secs < 0)
        return QStringLiteral("时间异常");
    if (secs < 60)
        return QStringLiteral("%1 秒前").arg(secs);
    if (secs < 3600)
        return QStringLiteral("%1 分钟前").arg(secs / 60);
    if (secs < 86400)
        return QStringLiteral("%1 小时 %2 分钟前").arg(secs / 3600).arg((secs % 3600) / 60);
    return QStringLiteral("%1 天 %2 小时前").arg(secs / 86400).arg((secs % 86400) / 3600);
}
}

ConsumerWindow::ConsumerWindow(QWidget* parent) : QMainWindow(parent),
    m_configPath(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("config.ini")),
    m_network(new QNetworkAccessManager(this)), m_refreshTimer(new QTimer(this))
{
    setWindowTitle(QStringLiteral("采集控制台")); resize(1050, 720);
    QWidget* central=new QWidget(this); QVBoxLayout* root=new QVBoxLayout(central);
    QGroupBox* serverBox=new QGroupBox(QStringLiteral("服务器"),central); QGridLayout* sf=new QGridLayout(serverBox);
    sf->setHorizontalSpacing(12); sf->setVerticalSpacing(8);
    m_serverEdit=new QLineEdit(serverBox); m_tokenEdit=new QLineEdit(serverBox); m_tokenEdit->setEchoMode(QLineEdit::Password);
    m_stateEdit=new QLineEdit(serverBox); m_intervalSpin=new QSpinBox(serverBox);
    m_intervalSpin->setRange(1,3600); m_intervalSpin->setSuffix(QStringLiteral(" 秒"));
    sf->addWidget(new QLabel(QStringLiteral("地址"),serverBox),0,0); sf->addWidget(m_serverEdit,0,1);
    sf->addWidget(new QLabel(QStringLiteral("下载令牌"),serverBox),0,2); sf->addWidget(m_tokenEdit,0,3);
    sf->addWidget(new QLabel(QStringLiteral("游标根目录"),serverBox),1,0); sf->addWidget(m_stateEdit,1,1);
    sf->addWidget(new QLabel(QStringLiteral("轮询间隔"),serverBox),1,2); sf->addWidget(m_intervalSpin,1,3);
    sf->setColumnStretch(1,1); sf->setColumnStretch(3,1);
    root->addWidget(serverBox);
    QHBoxLayout* commands=new QHBoxLayout; m_saveButton=new QPushButton(QStringLiteral("保存"),central);
    m_startButton=new QPushButton(QStringLiteral("启动获取数据"),central); m_stopButton=new QPushButton(QStringLiteral("停止获取数据"),central);
    QPushButton* refresh=new QPushButton(QStringLiteral("刷新设备"),central);
    m_saveButton->setObjectName("secondaryButton");
    m_stopButton->setObjectName("dangerButton");
    refresh->setObjectName("secondaryButton");
    commands->addWidget(m_saveButton);commands->addWidget(m_startButton);commands->addWidget(m_stopButton);commands->addWidget(refresh);commands->addStretch();root->addLayout(commands);

    QSplitter* split=new QSplitter(central); QWidget* left=new QWidget(split); QVBoxLayout* ll=new QVBoxLayout(left);
    m_deviceList=new QListWidget(left); ll->addWidget(m_deviceList,1); QHBoxLayout* dc=new QHBoxLayout;
    QPushButton* add=new QPushButton(QStringLiteral("新增"),left); QPushButton* remove=new QPushButton(QStringLiteral("停用"),left);
    add->setObjectName("secondaryButton");
    remove->setObjectName("dangerButton");
    dc->addWidget(add);dc->addWidget(remove);ll->addLayout(dc); split->addWidget(left);
    QTabWidget* tabs=new QTabWidget(split);
    QWidget* statusPage=new QWidget(tabs); QVBoxLayout* sl=new QVBoxLayout(statusPage); QFormLayout* df=new QFormLayout;
    m_deviceStatus=new QLabel("-",statusPage);m_deviceIp=new QLabel("-",statusPage);m_programVersion=new QLabel("-",statusPage);m_configVersion=new QLabel("-",statusPage);m_lastSeen=new QLabel("-",statusPage);
    df->addRow(QStringLiteral("设备状态"),m_deviceStatus);df->addRow(QStringLiteral("最后心跳"),m_lastSeen);df->addRow(QStringLiteral("设备IP"),m_deviceIp);df->addRow(QStringLiteral("程序版本"),m_programVersion);df->addRow(QStringLiteral("配置版本"),m_configVersion);sl->addLayout(df);
    m_stationHint=new QLabel(QString(),statusPage);m_stationHint->setWordWrap(true);m_stationHint->setStyleSheet(QStringLiteral("color:#8a5a00;"));sl->addWidget(m_stationHint);
    m_stationTable=new QTableWidget(statusPage);m_stationTable->setColumnCount(6);m_stationTable->setHorizontalHeaderLabels({QStringLiteral("站号"),QStringLiteral("IP:PORT"),QStringLiteral("连接"),QStringLiteral("在线通道"),QStringLiteral("总通道"),QStringLiteral("GPS")});m_stationTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);m_stationTable->setEditTriggers(QAbstractItemView::NoEditTriggers);sl->addWidget(m_stationTable);tabs->addTab(statusPage,QStringLiteral("设备状态"));
    QWidget* parameterPage=new QWidget(tabs);QGridLayout* pf=new QGridLayout(parameterPage);
    pf->setHorizontalSpacing(12);pf->setVerticalSpacing(8);
    const QStringList keys={"tri_on","tri_off","nsta","nlta","detect_ch","qualified_ch","energy_thre","amp_thre","intrach","interch"};
    const QStringList defaults={"5.5","2","50","500","1","1","0.25","0.25","100","250"};
    for(int i=0;i<keys.size();++i){
        const int row=i/2,col=i%2;
        QLabel* label=new QLabel(keys.at(i),parameterPage);
        label->setAlignment(Qt::AlignLeft|Qt::AlignVCenter);
        label->setMinimumWidth(110);
        QLineEdit* edit=new QLineEdit(defaults.at(i),parameterPage);
        m_parameterEdits.insert(keys.at(i),edit);
        pf->addWidget(label,row,col*2);
        pf->addWidget(edit,row,col*2+1);
    }
    const int uploadRow=(keys.size()+1)/2;
    QLabel* uploadLabel=new QLabel(QStringLiteral("上传服务器(设备使用)"),parameterPage);
    uploadLabel->setAlignment(Qt::AlignLeft|Qt::AlignVCenter);
    m_uploadUrlEdit=new QLineEdit(parameterPage);
    pf->addWidget(uploadLabel,uploadRow,0);
    pf->addWidget(m_uploadUrlEdit,uploadRow,1,1,3);
    QPushButton* submit=new QPushButton(QStringLiteral("下发参数"),parameterPage);
    pf->addWidget(submit,uploadRow+1,0,1,4);
    pf->setColumnStretch(1,1);pf->setColumnStretch(3,1);
    tabs->addTab(parameterPage,QStringLiteral("参数设置"));
    QWidget* filesPage=new QWidget(tabs);QFormLayout* ff=new QFormLayout(filesPage);
    m_outputEdit=new QLineEdit(filesPage);m_outputEdit->setReadOnly(true);m_outputEdit->setPlaceholderText(QStringLiteral("请选择设备后浏览保存目录"));m_outputEdit->setToolTip(QStringLiteral("点击右侧“浏览...”选择保存目录"));
    QPushButton* browse=new QPushButton(QStringLiteral("浏览..."),filesPage);browse->setObjectName("secondaryButton");
    QHBoxLayout* outputLay=new QHBoxLayout;outputLay->setContentsMargins(0,0,0,0);outputLay->addWidget(m_outputEdit,1);outputLay->addWidget(browse);
    m_statusLabel=new QLabel(QStringLiteral("未启动"),filesPage);m_cursorLabel=new QLabel("0",filesPage);m_lastFileLabel=new QLabel("-",filesPage);m_lastFileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ff->addRow(QStringLiteral("设备保存目录"),outputLay);
    ff->addRow(QStringLiteral("数据获取状态"),m_statusLabel);ff->addRow(QStringLiteral("已确认游标"),m_cursorLabel);ff->addRow(QStringLiteral("最近文件"),m_lastFileLabel);tabs->addTab(filesPage,QStringLiteral("mseed消费"));
    connect(browse,&QPushButton::clicked,this,[this](){
        const QString id=selectedDeviceId();
        if(id.isEmpty()){
            QMessageBox::information(this,QStringLiteral("设备保存目录"),QStringLiteral("请先选择设备，再设置保存目录"));
            return;
        }
        QString start=m_outputEdit->text().trimmed();
        if(!QFileInfo(start).isDir())start=QCoreApplication::applicationDirPath();
        const QString dir=QFileDialog::getExistingDirectory(this,QStringLiteral("选择mseed保存目录"),start);
        if(dir.isEmpty())return;
        m_outputEdit->setText(QDir::toNativeSeparators(dir));
        QSettings s(m_configPath,QSettings::IniFormat);
        s.setValue("DeviceDirs/"+id,m_outputEdit->text().trimmed());
        s.sync();
        appendLog(QStringLiteral("设备 %1 保存目录已更新：%2（重新启动获取数据后生效）").arg(id,m_outputEdit->text().trimmed()));
    });
    split->addWidget(tabs);split->setStretchFactor(1,1);root->addWidget(split,3);
    m_logEdit=new QPlainTextEdit(central);m_logEdit->setReadOnly(true);m_logEdit->setMaximumBlockCount(500);m_logEdit->setMinimumHeight(180);root->addWidget(m_logEdit,2);setCentralWidget(central);

    connect(m_saveButton,&QPushButton::clicked,this,&ConsumerWindow::saveSettings);connect(m_startButton,&QPushButton::clicked,this,&ConsumerWindow::startConsumer);connect(m_stopButton,&QPushButton::clicked,this,&ConsumerWindow::stopConsumer);connect(refresh,&QPushButton::clicked,this,[this](){m_manualRefreshRequested=true;refreshDevices();});connect(add,&QPushButton::clicked,this,&ConsumerWindow::addDevice);connect(remove,&QPushButton::clicked,this,&ConsumerWindow::removeDevice);connect(m_deviceList,&QListWidget::itemSelectionChanged,this,&ConsumerWindow::onDeviceSelected);connect(submit,&QPushButton::clicked,this,&ConsumerWindow::submitParameters);connect(m_refreshTimer,&QTimer::timeout,this,&ConsumerWindow::refreshDevices);
    createDefaultConfig();loadSettings();setRunning(false);m_refreshTimer->start(5000);appendLog(QStringLiteral("采集控制台已启动，服务器地址：%1，设备轮询 %2 秒").arg(m_serverEdit->text().trimmed().isEmpty()?QStringLiteral("<空>"):m_serverEdit->text().trimmed()).arg(m_refreshTimer->interval()/1000));refreshDevices();
}
ConsumerWindow::~ConsumerWindow()=default;

void ConsumerWindow::createDefaultConfig() const{if(QFile::exists(m_configPath))return;QSettings s(m_configPath,QSettings::IniFormat);s.setValue("Server/url","http://39.96.41.239:9002");s.setValue("Server/downloadToken","");s.remove("Server/projectToken");s.setValue("Consumer/pollSeconds",5);s.setValue("Consumer/outputDir","data/mseed");s.setValue("Consumer/stateFile","data/state");s.sync();}
void ConsumerWindow::loadSettings(){QSettings s(m_configPath,QSettings::IniFormat);m_serverEdit->setText(s.value("Server/url").toString());m_tokenEdit->setText(s.value("Server/downloadToken").toString());m_outputEdit->clear();m_stateEdit->setText(s.value("Consumer/stateFile","data/state").toString());m_intervalSpin->setValue(s.value("Consumer/pollSeconds",5).toInt());m_uploadUrlEdit->setText(m_serverEdit->text());m_outputDirDeviceId.clear();}
void ConsumerWindow::saveSettings(){
    QSettings s(m_configPath,QSettings::IniFormat);
    s.setValue("Server/url",m_serverEdit->text().trimmed());
    s.setValue("Server/downloadToken",m_tokenEdit->text().trimmed());
    s.remove("Server/projectToken");
    s.setValue("Consumer/stateFile",m_stateEdit->text().trimmed());
    s.setValue("Consumer/pollSeconds",m_intervalSpin->value());
    const QString deviceId=selectedDeviceId();
    if(!deviceId.isEmpty()){
        const QString outputDir=m_outputEdit->text().trimmed();
        if(outputDir.isEmpty())s.remove("DeviceDirs/"+deviceId);
        else s.setValue("DeviceDirs/"+deviceId,outputDir);
    }
    s.sync();
    m_projectInfoLoaded=false;
    m_projectName.clear();
    m_formDeviceId.clear();
    m_lastConfigStatusKey.clear();
    m_lastDeviceSnapshot.clear();
    m_deviceListLogged=false;
    m_lastRefreshFailed=false;
    m_manualRefreshRequested=false;
    m_lastSelectedLogId.clear();
    m_statusLogValid=false;
    setWindowTitle(QStringLiteral("采集控制台"));
    m_uploadUrlEdit->setText(m_serverEdit->text().trimmed());
    const QString server=m_serverEdit->text().trimmed();
    appendLog(QStringLiteral("配置已保存：服务器=%1，下载令牌=%2，设备=%3，保存目录=%4")
        .arg(server.isEmpty()?QStringLiteral("<空>"):server,
             m_tokenEdit->text().trimmed().isEmpty()?QStringLiteral("未填写"):QStringLiteral("已填写"),
             deviceId.isEmpty()?QStringLiteral("-"):deviceId,
             m_outputEdit->text().trimmed().isEmpty()?QStringLiteral("默认"):m_outputEdit->text().trimmed()));
}
QString ConsumerWindow::selectedDeviceId()const{return m_deviceList->currentItem()?m_deviceList->currentItem()->data(Qt::UserRole).toString():QString();}
QString ConsumerWindow::apiUrl(const QString& path)const{QString base=m_serverEdit->text().trimmed();while(base.endsWith('/'))base.chop(1);return base+path;}

// 控制面（设备列表、参数、状态）统一使用下载令牌作为项目令牌。
// 服务器端每个项目是独立服务、独立数据库，令牌不匹配直接返回 401，
// 因此控制台不可能看到或控制其它项目的设备。
QByteArray ConsumerWindow::controlToken()const{
    return m_tokenEdit?m_tokenEdit->text().trimmed().toUtf8():QByteArray();
}
QNetworkRequest ConsumerWindow::apiRequest(const QString& path)const{
    QNetworkRequest request(QUrl(apiUrl(path)));
    const QByteArray token=controlToken();
    if(!token.isEmpty())request.setRawHeader("X-Project-Token",token);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute,true);
    return request;
}
void ConsumerWindow::loadProjectInfo(){
    if(m_projectInfoLoaded||m_projectInfoRequestActive)return;
    if(m_serverEdit->text().trimmed().isEmpty())return;
    m_projectInfoRequestActive=true;
    QNetworkReply* r=m_network->get(apiRequest("/coalmine/project"));
    NetTimeout::arm(r,kRequestTimeoutMs);
    connect(r,&QNetworkReply::finished,this,[this,r](){
        m_projectInfoRequestActive=false;
        if(r->error()==QNetworkReply::NoError){
            const QJsonObject o=QJsonDocument::fromJson(r->readAll()).object();
            const QString name=o.value("name").toString();
            if(!name.isEmpty()){
                m_projectInfoLoaded=true;
                m_projectName=name;
                if (name == "project_lgy")
                    setWindowTitle(QStringLiteral("采集控制台 - %1").arg(QStringLiteral("老公营煤矿")));
                else
                    setWindowTitle(QStringLiteral("采集控制台 - %1").arg(name));
                appendLog(QStringLiteral("已连接项目：%1（版本 %2，服务器 %3）")
                    .arg(name, o.value("version").toString(), m_serverEdit->text().trimmed()));
            }
        }else{
            appendLogThrottled(QStringLiteral("获取项目信息失败：%1").arg(NetTimeout::describeFailure(r)));
        }
        r->deleteLater();
    });
}

void ConsumerWindow::refreshDevices(){
    loadProjectInfo();
    if(m_serverEdit->text().trimmed().isEmpty()){
        m_manualRefreshRequested=false;
        appendLogThrottled(QStringLiteral("刷新设备失败：服务器地址为空，请先填写并保存"));
        return;
    }
    QNetworkReply* r=m_network->get(apiRequest("/coalmine/devices"));
    NetTimeout::arm(r,kRequestTimeoutMs);
    connect(r,&QNetworkReply::finished,this,[this,r](){
        if(r->error()!=QNetworkReply::NoError){m_lastRefreshFailed=true;m_manualRefreshRequested=false;handleNetworkError(QStringLiteral("刷新设备"),r);r->deleteLater();return;}
        const QString selected=selectedDeviceId();
        const QJsonArray arr=QJsonDocument::fromJson(r->readAll()).array();
        int deviceCount=0;
        int onlineCount=0;
        QStringList snapshot;
        {
            QSignalBlocker blocker(m_deviceList);
            m_deviceList->clear();
            for(const QJsonValue& v:arr){
                const QJsonObject o=v.toObject();
                if(!o.value("enabled").toBool())continue;
                const QString id=o.value("device_id").toString();
                const bool online=o.value("online").toBool();
                ++deviceCount;
                if(online)++onlineCount;
                snapshot << id + (online?QStringLiteral(":1"):QStringLiteral(":0"));
                QListWidgetItem* item=new QListWidgetItem(QStringLiteral("%1  %2").arg(id,online?QStringLiteral("在线"):QStringLiteral("离线")),m_deviceList);
                item->setData(Qt::UserRole,id);
                if(id==selected)m_deviceList->setCurrentItem(item);
            }
            if(!m_deviceList->currentItem()&&m_deviceList->count())m_deviceList->setCurrentRow(0);
        }
        snapshot.sort();
        const QString state=snapshot.join(',');
        const bool changed=state!=m_lastDeviceSnapshot;
        const bool first=!m_deviceListLogged;
        const bool manual=m_manualRefreshRequested;
        const bool recovered=m_lastRefreshFailed;
        m_lastDeviceSnapshot=state;
        m_manualRefreshRequested=false;
        m_lastRefreshFailed=false;
        if(changed||first||manual||recovered){
            m_deviceListLogged=true;
            if(deviceCount==0){
                appendLog(QStringLiteral("设备列表刷新成功：当前项目暂无设备%1")
                    .arg(m_projectName.isEmpty()?QString():QStringLiteral("（%1）").arg(m_projectName)));
            }else{
                appendLog(QStringLiteral("设备列表刷新成功：共 %1 台，在线 %2 台%3")
                    .arg(deviceCount).arg(onlineCount)
                    .arg(m_projectName.isEmpty()?QString():QStringLiteral("（%1）").arg(m_projectName)));
            }
        }
        onDeviceSelected();
        r->deleteLater();
    });
}
void ConsumerWindow::addDevice(){bool ok=false;QString id=QInputDialog::getText(this,QStringLiteral("新增设备"),QStringLiteral("设备编号"),QLineEdit::Normal,QString(),&ok).trimmed();if(!ok||id.isEmpty())return;QJsonObject body{{"name",QStringLiteral("EG628-%1").arg(id)}};QNetworkRequest req=apiRequest("/coalmine/devices/"+id);req.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");QNetworkReply* r=m_network->put(req,QJsonDocument(body).toJson(QJsonDocument::Compact));NetTimeout::arm(r,kRequestTimeoutMs);connect(r,&QNetworkReply::finished,this,[this,r,id](){if(r->error()!=QNetworkReply::NoError)handleNetworkError(QStringLiteral("新增设备"),r);else{appendLog(QStringLiteral("新增设备 %1 成功").arg(id));refreshDevices();}r->deleteLater();});}
void ConsumerWindow::removeDevice(){QString id=selectedDeviceId();if(id.isEmpty()||QMessageBox::question(this,QStringLiteral("停用设备"),QStringLiteral("确定停用设备 %1？历史文件不会删除。").arg(id))!=QMessageBox::Yes)return;QNetworkReply* r=m_network->deleteResource(apiRequest("/coalmine/devices/"+id));NetTimeout::arm(r,kRequestTimeoutMs);connect(r,&QNetworkReply::finished,this,[this,r,id](){if(r->error()!=QNetworkReply::NoError)handleNetworkError(QStringLiteral("停用设备"),r);else{appendLog(QStringLiteral("设备 %1 已停用，历史文件保留").arg(id));refreshDevices();}r->deleteLater();});}
void ConsumerWindow::onDeviceSelected(){
    const QString id=selectedDeviceId();if(id.isEmpty())return;
    if(id!=m_lastSelectedLogId){
        m_lastSelectedLogId=id;
        appendLog(QStringLiteral("已选择设备 %1，正在获取状态和配置").arg(id));
    }
    if(id!=m_outputDirDeviceId)loadOutputDirForDevice(id);
    QNetworkReply* r=m_network->get(apiRequest("/coalmine/devices/"+id));
    NetTimeout::arm(r,kRequestTimeoutMs);
    connect(r,&QNetworkReply::finished,this,[this,r,id](){if(id!=selectedDeviceId()){r->deleteLater();return;}if(r->error()!=QNetworkReply::NoError)handleNetworkError(QStringLiteral("获取设备状态"),r);else showDevice(QJsonDocument::fromJson(r->readAll()).object());r->deleteLater();});
    // 只有切换设备时才把服务端配置填入表单。5 秒刷新不能覆盖用户刚改的
    // “上传服务器”，否则手动改成 9002 后会被旧配置的 443 立刻改回去。
    const bool loadForm=id!=m_formDeviceId;
    QNetworkReply* cr=m_network->get(apiRequest("/coalmine/devices/"+id+"/config/status"));
    NetTimeout::arm(cr,kRequestTimeoutMs);
    connect(cr,&QNetworkReply::finished,this,[this,cr,id,loadForm](){if(id!=selectedDeviceId()){cr->deleteLater();return;}const int status=cr->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();if(cr->error()==QNetworkReply::NoError&&status==200){QJsonObject cfg=QJsonDocument::fromJson(cr->readAll()).object();if(loadForm){QJsonObject p=cfg.value("parameters").toObject();for(auto it=m_parameterEdits.begin();it!=m_parameterEdits.end();++it)if(p.contains(it.key()))it.value()->setText(QString::number(p.value(it.key()).toDouble(),'g',12));const QString url=cfg.value("upload_server_url").toString().trimmed();m_uploadUrlEdit->setText(url.isEmpty()?m_serverEdit->text().trimmed():url);m_formDeviceId=id;}const QString pulled=cfg.value("pulled_at").toString(),applied=cfg.value("applied_at").toString();const QString summary=QStringLiteral("设备配置：版本 %1，状态 %2，拉取 %3，应用 %4%5").arg(cfg.value("version").toInt()).arg(cfg.value("apply_status").toString().isEmpty()?QStringLiteral("未知"):cfg.value("apply_status").toString()).arg(pulled.isEmpty()?QStringLiteral("未拉取"):pulled).arg(applied.isEmpty()?QStringLiteral("未应用"):applied).arg(cfg.value("apply_message").toString().isEmpty()?QString():QStringLiteral("，说明 ")+cfg.value("apply_message").toString());if(summary!=m_lastConfigStatusKey){m_lastConfigStatusKey=summary;appendLog(summary);}}else if(cr->error()==QNetworkReply::NoError&&status==204){if(loadForm){m_uploadUrlEdit->setText(m_serverEdit->text().trimmed());m_formDeviceId=id;}}else{handleNetworkError(QStringLiteral("获取设备配置状态"),cr);}cr->deleteLater();});
    if(m_consumer&&m_consumingDeviceId!=id){stopConsumer();startConsumer();}}
void ConsumerWindow::showDevice(const QJsonObject& o){
    const bool online=o.value("online").toBool();
    const QDateTime lastSeen=deviceUtcTime(o.value("last_seen_at").toString());
    const QString age=relativeAge(lastSeen);
    const QJsonObject status=o.value("status").toObject();
    m_deviceStatus->setText(online?QStringLiteral("在线"):QStringLiteral("离线"));
    m_lastSeen->setText(lastSeen.isValid()?QStringLiteral("%1（%2）").arg(lastSeen.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"),age):QStringLiteral("无心跳记录"));
    m_deviceIp->setText(o.value("remote_ip").toString()+" / "+status.value("local_ip").toString());
    m_programVersion->setText(status.value("program_version").toString());
    m_configVersion->setText(QStringLiteral("已应用 %1 / 期望 %2%3").arg(o.value("applied_config_version").toInt()).arg(o.value("desired_config_version").toInt()).arg(online?QString():QStringLiteral("（离线，上线后才会拉取）")));
    m_stationHint->setText(online?QString():QStringLiteral("设备离线：下表是最后一次心跳（%1）时的站点快照，不代表当前状态").arg(age));
    const QJsonArray stations=status.value("stations").toArray();
    const QString deviceId=o.value("device_id").toString();
    int connectedStations=0;
    for(const QJsonValue& value:stations)
        if(value.toObject().value("connected").toBool())++connectedStations;
    if(!m_statusLogValid||deviceId!=m_statusLogDeviceId||online!=m_statusLogOnline){
        m_statusLogValid=true;
        m_statusLogDeviceId=deviceId;
        m_statusLogOnline=online;
        appendLog(QStringLiteral("设备 %1 状态：%2；最后心跳：%3；站点：%4 个，已连接 %5 个")
            .arg(deviceId.isEmpty()?QStringLiteral("-"):deviceId,
                 online?QStringLiteral("在线"):QStringLiteral("离线"),
                 lastSeen.isValid()?QStringLiteral("%1（%2）")
                     .arg(lastSeen.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"),age)
                     :QStringLiteral("无心跳记录"))
            .arg(stations.size()).arg(connectedStations));
    }
    m_stationTable->setRowCount(stations.size());
    for(int i=0;i<stations.size();++i){
        const QJsonObject s=stations.at(i).toObject();
        QString connected=s.value("connected").toBool()?QStringLiteral("在线"):QStringLiteral("离线");
        if(!online)connected+=QStringLiteral("（快照）");
        const QString ip=s.value("ip").toString();
        const int port=s.value("port").toInt();
        const QString endpoint=ip.isEmpty()||port<=0?QStringLiteral("-"):QStringLiteral("%1:%2").arg(ip).arg(port);
        const QStringList values={QString::number(s.value("station_number").toInt()),endpoint,connected,QString::number(s.value("online_channels").toInt()),QString::number(s.value("total_channels").toInt()),QString::number(s.value("gps").toInt())};
        for(int c=0;c<values.size();++c){
            QTableWidgetItem* item=new QTableWidgetItem(values.at(c));
            if(!online)item->setForeground(QBrush(QColor(130,130,130)));
            m_stationTable->setItem(i,c,item);
        }
    }
}
void ConsumerWindow::submitParameters(){
    const QString id=selectedDeviceId();
    if(id.isEmpty()){QMessageBox::warning(this,QStringLiteral("参数"),QStringLiteral("请先选择设备"));return;}
    const QString uploadUrl=m_uploadUrlEdit->text().trimmed();
    if(uploadUrl.isEmpty()||(!uploadUrl.startsWith("http://")&&!uploadUrl.startsWith("https://"))){
        QMessageBox::warning(this,QStringLiteral("参数"),QStringLiteral("上传服务器地址无效，请填写 http:// 或 https:// 地址"));
        return;
    }
    QJsonObject p;
    for(auto it=m_parameterEdits.constBegin();it!=m_parameterEdits.constEnd();++it)p.insert(it.key(),it.value()->text().toDouble());
    QJsonObject body{{"parameters",p},{"upload_server_url",uploadUrl}};
    QNetworkRequest req=apiRequest("/coalmine/devices/"+id+"/config");
    req.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
    appendLog(QStringLiteral("下发参数到设备 %1：上传服务器=%2").arg(id,uploadUrl));
    QNetworkReply* r=m_network->put(req,QJsonDocument(body).toJson(QJsonDocument::Compact));
    NetTimeout::arm(r,kRequestTimeoutMs);
    connect(r,&QNetworkReply::finished,this,[this,r,id](){
        if(r->error()!=QNetworkReply::NoError){
            handleNetworkError(QStringLiteral("下发参数"),r);
        }else{
            QJsonObject o=QJsonDocument::fromJson(r->readAll()).object();
            m_formDeviceId=id;
            appendLog(QStringLiteral("设备 %1 参数版本 %2 已提交，等待设备应用（设备在线后 10 秒内拉取）").arg(id).arg(o.value("version").toInt()));
            onDeviceSelected();
        }
        r->deleteLater();
    });
}

void ConsumerWindow::startConsumer(){
    const QString id=selectedDeviceId();
    if(id.isEmpty()){appendLog(QStringLiteral("请先选择设备"));return;}
    if(m_serverEdit->text().trimmed().isEmpty()||m_tokenEdit->text().trimmed().isEmpty()){
        appendLog(QStringLiteral("服务器地址或下载令牌为空，无法启动数据获取"));
        return;
    }
    saveSettings();
    QString outRaw=m_outputEdit->text().trimmed();
    if(outRaw.isEmpty()){
        QSettings s(m_configPath,QSettings::IniFormat);
        const QString root=s.value("Consumer/outputDir","data/mseed").toString().trimmed();
        outRaw=QDir(root.isEmpty()?QStringLiteral("data/mseed"):root).filePath(id);
    }
    const QString out=absoluteConfigPath(outRaw);
    QString stateBase=absoluteConfigPath(m_stateEdit->text().trimmed());
    if(QFileInfo(stateBase).suffix().compare("txt",Qt::CaseInsensitive)==0)
        stateBase=QFileInfo(stateBase).absolutePath();
    const QString stateDir=QDir(stateBase).absoluteFilePath(id);
    const QString stateFile=QDir(stateDir).absoluteFilePath("cursor.txt");
    appendLog(QStringLiteral("启动设备 %1 数据获取：输出目录=%2，游标文件=%3").arg(id,out,stateFile));
    m_consumer.reset(new MseedConsumer(QUrl(m_serverEdit->text().trimmed()),m_tokenEdit->text().trimmed().toUtf8(),id,out,stateFile,m_intervalSpin->value(),this));
    connect(m_consumer.get(),&MseedConsumer::statusChanged,m_statusLabel,&QLabel::setText);
    connect(m_consumer.get(),&MseedConsumer::cursorChanged,this,[this](qint64 v){m_cursorLabel->setText(QString::number(v));});
    connect(m_consumer.get(),&MseedConsumer::fileDownloaded,this,[this](qint64,const QString&,const QString& path){m_lastFileLabel->setText(path);});
    connect(m_consumer.get(),&MseedConsumer::logMessage,this,&ConsumerWindow::appendLog);
    if(!m_consumer->start()){
        appendLog(QStringLiteral("设备 %1 数据获取启动失败，请检查游标文件和目录权限").arg(id));
        m_consumer.reset();
        return;
    }
    m_consumingDeviceId=id;
    setRunning(true);
}
void ConsumerWindow::stopConsumer(){
    if(m_consumer){
        m_consumer->stop();
    }
    m_consumer.reset();
    m_consumingDeviceId.clear();
    setRunning(false);
}
void ConsumerWindow::appendLog(const QString& m){m_logEdit->appendPlainText(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"),m));}
QString ConsumerWindow::absoluteConfigPath(const QString& v)const{QFileInfo i(v);return i.isAbsolute()?i.absoluteFilePath():QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(v);}
void ConsumerWindow::loadOutputDirForDevice(const QString& id){
    if(!m_outputEdit)return;
    QSettings s(m_configPath,QSettings::IniFormat);
    QString path=s.value("DeviceDirs/"+id).toString().trimmed();
    if(path.isEmpty()){
        const QString root=s.value("Consumer/outputDir","data/mseed").toString().trimmed();
        path=QDir(absoluteConfigPath(root.isEmpty()?QStringLiteral("data/mseed"):root)).absoluteFilePath(id);
    }else{
        path=absoluteConfigPath(path);
    }
    m_outputEdit->setText(QDir::toNativeSeparators(path));
    m_outputDirDeviceId=id;
}
void ConsumerWindow::setRunning(bool running){m_startButton->setEnabled(!running);m_stopButton->setEnabled(running);}
void ConsumerWindow::handleNetworkError(const QString& op,QNetworkReply* r){
    const int http=r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString hint;
    if(http==401)hint=QStringLiteral("（下载令牌无效或与本项目不匹配）");
    else if(http==403)hint=QStringLiteral("（无权限）");
    else if(http==404)hint=QStringLiteral("（接口或设备不存在）");
    else if(http==0)hint=NetTimeout::timedOut(r)?QStringLiteral("（请求超时，检查网络和服务器）"):QStringLiteral("（无法连接服务器，检查地址和端口）");
    else if(http>=500)hint=QStringLiteral("（服务器内部错误）");
    appendLogThrottled(QStringLiteral("%1失败：%2%3").arg(op,NetTimeout::describeFailure(r),hint));
}
void ConsumerWindow::appendLogThrottled(const QString& message){
    const qint64 now=QDateTime::currentMSecsSinceEpoch();
    if(message==m_lastErrorKey&&now-m_lastErrorMs<kErrorRepeatMs)return;
    m_lastErrorKey=message;m_lastErrorMs=now;
    appendLog(message);
}
