#include "ConsumerWindow.h"
#include "MseedConsumer.h"
#include "NetworkReplyTimeout.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
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
    QGroupBox* serverBox=new QGroupBox(QStringLiteral("服务器"),central); QFormLayout* sf=new QFormLayout(serverBox);
    m_serverEdit=new QLineEdit(serverBox); m_tokenEdit=new QLineEdit(serverBox); m_tokenEdit->setEchoMode(QLineEdit::Password);
    m_projectTokenEdit=new QLineEdit(serverBox); m_projectTokenEdit->setEchoMode(QLineEdit::Password);
    m_projectTokenEdit->setPlaceholderText(QStringLiteral("留空则使用下载令牌"));
    m_outputEdit=new QLineEdit(serverBox); m_stateEdit=new QLineEdit(serverBox); m_intervalSpin=new QSpinBox(serverBox);
    m_intervalSpin->setRange(1,3600); m_intervalSpin->setSuffix(QStringLiteral(" 秒"));
    sf->addRow(QStringLiteral("地址"),m_serverEdit); sf->addRow(QStringLiteral("下载令牌"),m_tokenEdit);
    sf->addRow(QStringLiteral("项目令牌"),m_projectTokenEdit);
    sf->addRow(QStringLiteral("文件根目录"),m_outputEdit); sf->addRow(QStringLiteral("游标根目录"),m_stateEdit);
    sf->addRow(QStringLiteral("轮询间隔"),m_intervalSpin); root->addWidget(serverBox);
    QHBoxLayout* commands=new QHBoxLayout; m_saveButton=new QPushButton(QStringLiteral("保存"),central);
    m_startButton=new QPushButton(QStringLiteral("启动消费"),central); m_stopButton=new QPushButton(QStringLiteral("停止消费"),central);
    QPushButton* refresh=new QPushButton(QStringLiteral("刷新设备"),central);
    commands->addWidget(m_saveButton);commands->addWidget(m_startButton);commands->addWidget(m_stopButton);commands->addWidget(refresh);commands->addStretch();root->addLayout(commands);

    QSplitter* split=new QSplitter(central); QWidget* left=new QWidget(split); QVBoxLayout* ll=new QVBoxLayout(left);
    m_deviceList=new QListWidget(left); ll->addWidget(m_deviceList,1); QHBoxLayout* dc=new QHBoxLayout;
    QPushButton* add=new QPushButton(QStringLiteral("新增"),left); QPushButton* remove=new QPushButton(QStringLiteral("停用"),left);
    dc->addWidget(add);dc->addWidget(remove);ll->addLayout(dc); split->addWidget(left);
    QTabWidget* tabs=new QTabWidget(split);
    QWidget* statusPage=new QWidget(tabs); QVBoxLayout* sl=new QVBoxLayout(statusPage); QFormLayout* df=new QFormLayout;
    m_deviceStatus=new QLabel("-",statusPage);m_deviceIp=new QLabel("-",statusPage);m_programVersion=new QLabel("-",statusPage);m_configVersion=new QLabel("-",statusPage);m_lastSeen=new QLabel("-",statusPage);
    df->addRow(QStringLiteral("设备状态"),m_deviceStatus);df->addRow(QStringLiteral("最后心跳"),m_lastSeen);df->addRow(QStringLiteral("设备IP"),m_deviceIp);df->addRow(QStringLiteral("程序版本"),m_programVersion);df->addRow(QStringLiteral("配置版本"),m_configVersion);sl->addLayout(df);
    m_stationHint=new QLabel(QString(),statusPage);m_stationHint->setWordWrap(true);m_stationHint->setStyleSheet(QStringLiteral("color:#8a5a00;"));sl->addWidget(m_stationHint);
    m_stationTable=new QTableWidget(statusPage);m_stationTable->setColumnCount(6);m_stationTable->setHorizontalHeaderLabels({QStringLiteral("站号"),QStringLiteral("IP:PORT"),QStringLiteral("连接"),QStringLiteral("在线通道"),QStringLiteral("总通道"),QStringLiteral("GPS")});m_stationTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);m_stationTable->setEditTriggers(QAbstractItemView::NoEditTriggers);sl->addWidget(m_stationTable);tabs->addTab(statusPage,QStringLiteral("设备状态"));
    QWidget* parameterPage=new QWidget(tabs);QFormLayout* pf=new QFormLayout(parameterPage);
    const QStringList keys={"tri_on","tri_off","nsta","nlta","detect_ch","qualified_ch","energy_thre","amp_thre","intrach","interch"};
    const QStringList defaults={"5.5","2","50","500","1","1","0.25","0.25","100","250"};
    for(int i=0;i<keys.size();++i){QLineEdit* edit=new QLineEdit(defaults.at(i),parameterPage);m_parameterEdits.insert(keys.at(i),edit);pf->addRow(keys.at(i),edit);}
    m_uploadUrlEdit=new QLineEdit(parameterPage);pf->addRow(QStringLiteral("上传服务器(设备使用)"),m_uploadUrlEdit);QPushButton* submit=new QPushButton(QStringLiteral("下发参数"),parameterPage);pf->addRow(QString(),submit);tabs->addTab(parameterPage,QStringLiteral("参数设置"));
    QWidget* filesPage=new QWidget(tabs);QFormLayout* ff=new QFormLayout(filesPage);m_statusLabel=new QLabel(QStringLiteral("未启动"),filesPage);m_cursorLabel=new QLabel("0",filesPage);m_lastFileLabel=new QLabel("-",filesPage);m_lastFileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);ff->addRow(QStringLiteral("消费状态"),m_statusLabel);ff->addRow(QStringLiteral("已确认游标"),m_cursorLabel);ff->addRow(QStringLiteral("最近文件"),m_lastFileLabel);tabs->addTab(filesPage,QStringLiteral("mseed消费"));
    split->addWidget(tabs);split->setStretchFactor(1,1);root->addWidget(split,2);
    m_logEdit=new QPlainTextEdit(central);m_logEdit->setReadOnly(true);m_logEdit->setMaximumBlockCount(500);root->addWidget(m_logEdit,1);setCentralWidget(central);

    connect(m_saveButton,&QPushButton::clicked,this,&ConsumerWindow::saveSettings);connect(m_startButton,&QPushButton::clicked,this,&ConsumerWindow::startConsumer);connect(m_stopButton,&QPushButton::clicked,this,&ConsumerWindow::stopConsumer);connect(refresh,&QPushButton::clicked,this,&ConsumerWindow::refreshDevices);connect(add,&QPushButton::clicked,this,&ConsumerWindow::addDevice);connect(remove,&QPushButton::clicked,this,&ConsumerWindow::removeDevice);connect(m_deviceList,&QListWidget::itemSelectionChanged,this,&ConsumerWindow::onDeviceSelected);connect(submit,&QPushButton::clicked,this,&ConsumerWindow::submitParameters);connect(m_refreshTimer,&QTimer::timeout,this,&ConsumerWindow::refreshDevices);
    createDefaultConfig();loadSettings();setRunning(false);m_refreshTimer->start(5000);appendLog(QStringLiteral("采集控制台已启动，服务器地址：%1，设备轮询 %2 秒").arg(m_serverEdit->text().trimmed().isEmpty()?QStringLiteral("<空>"):m_serverEdit->text().trimmed()).arg(m_refreshTimer->interval()/1000));refreshDevices();
}
ConsumerWindow::~ConsumerWindow()=default;

void ConsumerWindow::createDefaultConfig() const{if(QFile::exists(m_configPath))return;QSettings s(m_configPath,QSettings::IniFormat);s.setValue("Server/url","http://39.96.41.239:9002");s.setValue("Server/downloadToken","");s.setValue("Server/projectToken","");s.setValue("Consumer/pollSeconds",5);s.setValue("Consumer/outputDir","data/mseed");s.setValue("Consumer/stateFile","data/state");s.sync();}
void ConsumerWindow::loadSettings(){QSettings s(m_configPath,QSettings::IniFormat);m_serverEdit->setText(s.value("Server/url").toString());m_tokenEdit->setText(s.value("Server/downloadToken").toString());m_projectTokenEdit->setText(s.value("Server/projectToken").toString());m_outputEdit->setText(s.value("Consumer/outputDir","data/mseed").toString());m_stateEdit->setText(s.value("Consumer/stateFile","data/state").toString());m_intervalSpin->setValue(s.value("Consumer/pollSeconds",5).toInt());m_uploadUrlEdit->setText(m_serverEdit->text());}
void ConsumerWindow::saveSettings(){QSettings s(m_configPath,QSettings::IniFormat);s.setValue("Server/url",m_serverEdit->text().trimmed());s.setValue("Server/downloadToken",m_tokenEdit->text().trimmed());s.setValue("Server/projectToken",m_projectTokenEdit->text().trimmed());s.setValue("Consumer/outputDir",m_outputEdit->text().trimmed());s.setValue("Consumer/stateFile",m_stateEdit->text().trimmed());s.setValue("Consumer/pollSeconds",m_intervalSpin->value());s.sync();m_projectInfoLoaded=false;m_formDeviceId.clear();m_lastConfigStatusKey.clear();setWindowTitle(QStringLiteral("采集控制台"));m_uploadUrlEdit->setText(m_serverEdit->text().trimmed());appendLog(QStringLiteral("配置已保存，服务器地址：%1").arg(m_serverEdit->text().trimmed().isEmpty()?QStringLiteral("<空>"):m_serverEdit->text().trimmed()));}
QString ConsumerWindow::selectedDeviceId()const{return m_deviceList->currentItem()?m_deviceList->currentItem()->data(Qt::UserRole).toString():QString();}
QString ConsumerWindow::apiUrl(const QString& path)const{QString base=m_serverEdit->text().trimmed();while(base.endsWith('/'))base.chop(1);return base+path;}

// 控制面（设备列表、参数、状态）用项目令牌；留空时沿用下载令牌。
// 服务器端每个项目是独立服务、独立数据库，令牌不匹配直接返回 401，
// 因此控制台不可能看到或控制其它项目的设备。
QByteArray ConsumerWindow::controlToken()const{
    QString token=m_projectTokenEdit?m_projectTokenEdit->text().trimmed():QString();
    if(token.isEmpty()&&m_tokenEdit)token=m_tokenEdit->text().trimmed();
    return token.toUtf8();
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
                setWindowTitle(QStringLiteral("采集控制台 - %1").arg(name));
                appendLog(QStringLiteral("已连接项目：%1（版本 %2）").arg(name,o.value("version").toString()));
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
        appendLogThrottled(QStringLiteral("刷新设备失败：服务器地址为空，请先填写并保存"));
        return;
    }
    QNetworkReply* r=m_network->get(apiRequest("/coalmine/devices"));
    NetTimeout::arm(r,kRequestTimeoutMs);
    connect(r,&QNetworkReply::finished,this,[this,r](){if(r->error()!=QNetworkReply::NoError){handleNetworkError(QStringLiteral("刷新设备"),r);r->deleteLater();return;}QString selected=selectedDeviceId();QJsonArray arr=QJsonDocument::fromJson(r->readAll()).array();{QSignalBlocker blocker(m_deviceList);m_deviceList->clear();for(const QJsonValue& v:arr){QJsonObject o=v.toObject();if(!o.value("enabled").toBool())continue;QString id=o.value("device_id").toString();QListWidgetItem* item=new QListWidgetItem(QStringLiteral("%1  %2").arg(id,o.value("online").toBool()?QStringLiteral("在线"):QStringLiteral("离线")),m_deviceList);item->setData(Qt::UserRole,id);if(id==selected)m_deviceList->setCurrentItem(item);}if(!m_deviceList->currentItem()&&m_deviceList->count())m_deviceList->setCurrentRow(0);}onDeviceSelected();r->deleteLater();});}
void ConsumerWindow::addDevice(){bool ok=false;QString id=QInputDialog::getText(this,QStringLiteral("新增设备"),QStringLiteral("设备编号"),QLineEdit::Normal,QString(),&ok).trimmed();if(!ok||id.isEmpty())return;QJsonObject body{{"name",QStringLiteral("EG628-%1").arg(id)}};QNetworkRequest req=apiRequest("/coalmine/devices/"+id);req.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");QNetworkReply* r=m_network->put(req,QJsonDocument(body).toJson(QJsonDocument::Compact));NetTimeout::arm(r,kRequestTimeoutMs);connect(r,&QNetworkReply::finished,this,[this,r](){if(r->error()!=QNetworkReply::NoError)handleNetworkError(QStringLiteral("新增设备"),r);else refreshDevices();r->deleteLater();});}
void ConsumerWindow::removeDevice(){QString id=selectedDeviceId();if(id.isEmpty()||QMessageBox::question(this,QStringLiteral("停用设备"),QStringLiteral("确定停用设备 %1？历史文件不会删除。").arg(id))!=QMessageBox::Yes)return;QNetworkReply* r=m_network->deleteResource(apiRequest("/coalmine/devices/"+id));NetTimeout::arm(r,kRequestTimeoutMs);connect(r,&QNetworkReply::finished,this,[this,r](){if(r->error()!=QNetworkReply::NoError)handleNetworkError(QStringLiteral("停用设备"),r);else refreshDevices();r->deleteLater();});}
void ConsumerWindow::onDeviceSelected(){
    const QString id=selectedDeviceId();if(id.isEmpty())return;
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

void ConsumerWindow::startConsumer(){QString id=selectedDeviceId();if(id.isEmpty()){appendLog(QStringLiteral("请先选择设备"));return;}if(m_serverEdit->text().trimmed().isEmpty()||m_tokenEdit->text().trimmed().isEmpty()){appendLog(QStringLiteral("服务器地址或下载令牌为空"));return;}saveSettings();QString out=QDir(absoluteConfigPath(m_outputEdit->text().trimmed())).absoluteFilePath(id);QString stateBase=absoluteConfigPath(m_stateEdit->text().trimmed());if(QFileInfo(stateBase).suffix().compare("txt",Qt::CaseInsensitive)==0)stateBase=QFileInfo(stateBase).absolutePath();QString stateDir=QDir(stateBase).absoluteFilePath(id);m_consumer.reset(new MseedConsumer(QUrl(m_serverEdit->text().trimmed()),m_tokenEdit->text().trimmed().toUtf8(),id,out,QDir(stateDir).absoluteFilePath("cursor.txt"),m_intervalSpin->value(),this));connect(m_consumer.get(),&MseedConsumer::statusChanged,m_statusLabel,&QLabel::setText);connect(m_consumer.get(),&MseedConsumer::cursorChanged,this,[this](qint64 v){m_cursorLabel->setText(QString::number(v));});connect(m_consumer.get(),&MseedConsumer::fileDownloaded,this,[this](qint64,const QString&,const QString& path){m_lastFileLabel->setText(path);});connect(m_consumer.get(),&MseedConsumer::logMessage,this,&ConsumerWindow::appendLog);if(!m_consumer->start()){m_consumer.reset();return;}m_consumingDeviceId=id;setRunning(true);}
void ConsumerWindow::stopConsumer(){if(m_consumer)m_consumer->stop();m_consumer.reset();m_consumingDeviceId.clear();setRunning(false);}
void ConsumerWindow::appendLog(const QString& m){m_logEdit->appendPlainText(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"),m));}
QString ConsumerWindow::absoluteConfigPath(const QString& v)const{QFileInfo i(v);return i.isAbsolute()?i.absoluteFilePath():QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(v);}
void ConsumerWindow::setRunning(bool running){m_startButton->setEnabled(!running);m_stopButton->setEnabled(running);}
void ConsumerWindow::handleNetworkError(const QString& op,QNetworkReply* r){
    appendLogThrottled(QStringLiteral("%1失败：%2").arg(op,NetTimeout::describeFailure(r)));
}
void ConsumerWindow::appendLogThrottled(const QString& message){
    const qint64 now=QDateTime::currentMSecsSinceEpoch();
    if(message==m_lastErrorKey&&now-m_lastErrorMs<kErrorRepeatMs)return;
    m_lastErrorKey=message;m_lastErrorMs=now;
    appendLog(message);
}
