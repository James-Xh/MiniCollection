#include "ConsumerWindow.h"
#include "MseedConsumer.h"
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

ConsumerWindow::ConsumerWindow(QWidget* parent) : QMainWindow(parent),
    m_configPath(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("config.ini")),
    m_network(new QNetworkAccessManager(this)), m_refreshTimer(new QTimer(this))
{
    setWindowTitle(QStringLiteral("MiniCollection 控制台")); resize(1050, 720);
    QWidget* central=new QWidget(this); QVBoxLayout* root=new QVBoxLayout(central);
    QGroupBox* serverBox=new QGroupBox(QStringLiteral("服务器"),central); QFormLayout* sf=new QFormLayout(serverBox);
    m_serverEdit=new QLineEdit(serverBox); m_tokenEdit=new QLineEdit(serverBox); m_tokenEdit->setEchoMode(QLineEdit::Password);
    m_outputEdit=new QLineEdit(serverBox); m_stateEdit=new QLineEdit(serverBox); m_intervalSpin=new QSpinBox(serverBox);
    m_intervalSpin->setRange(1,3600); m_intervalSpin->setSuffix(QStringLiteral(" 秒"));
    sf->addRow(QStringLiteral("地址"),m_serverEdit); sf->addRow(QStringLiteral("下载令牌"),m_tokenEdit);
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
    m_deviceStatus=new QLabel("-",statusPage);m_deviceIp=new QLabel("-",statusPage);m_programVersion=new QLabel("-",statusPage);m_configVersion=new QLabel("-",statusPage);
    df->addRow(QStringLiteral("设备状态"),m_deviceStatus);df->addRow(QStringLiteral("设备IP"),m_deviceIp);df->addRow(QStringLiteral("程序版本"),m_programVersion);df->addRow(QStringLiteral("配置版本"),m_configVersion);sl->addLayout(df);
    m_stationTable=new QTableWidget(statusPage);m_stationTable->setColumnCount(5);m_stationTable->setHorizontalHeaderLabels({QStringLiteral("站号"),QStringLiteral("连接"),QStringLiteral("在线通道"),QStringLiteral("总通道"),QStringLiteral("GPS")});m_stationTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);m_stationTable->setEditTriggers(QAbstractItemView::NoEditTriggers);sl->addWidget(m_stationTable);tabs->addTab(statusPage,QStringLiteral("设备状态"));
    QWidget* parameterPage=new QWidget(tabs);QFormLayout* pf=new QFormLayout(parameterPage);
    const QStringList keys={"tri_on","tri_off","nsta","nlta","detect_ch","qualified_ch","energy_thre","amp_thre","intrach","interch"};
    const QStringList defaults={"5.5","2","50","500","1","1","0.25","0.25","100","250"};
    for(int i=0;i<keys.size();++i){QLineEdit* edit=new QLineEdit(defaults.at(i),parameterPage);m_parameterEdits.insert(keys.at(i),edit);pf->addRow(keys.at(i),edit);}
    m_uploadUrlEdit=new QLineEdit(parameterPage);pf->addRow(QStringLiteral("上传服务器"),m_uploadUrlEdit);QPushButton* submit=new QPushButton(QStringLiteral("下发参数"),parameterPage);pf->addRow(QString(),submit);tabs->addTab(parameterPage,QStringLiteral("参数设置"));
    QWidget* filesPage=new QWidget(tabs);QFormLayout* ff=new QFormLayout(filesPage);m_statusLabel=new QLabel(QStringLiteral("未启动"),filesPage);m_cursorLabel=new QLabel("0",filesPage);m_lastFileLabel=new QLabel("-",filesPage);m_lastFileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);ff->addRow(QStringLiteral("消费状态"),m_statusLabel);ff->addRow(QStringLiteral("已确认游标"),m_cursorLabel);ff->addRow(QStringLiteral("最近文件"),m_lastFileLabel);tabs->addTab(filesPage,QStringLiteral("mseed消费"));
    split->addWidget(tabs);split->setStretchFactor(1,1);root->addWidget(split,2);
    m_logEdit=new QPlainTextEdit(central);m_logEdit->setReadOnly(true);m_logEdit->setMaximumBlockCount(500);root->addWidget(m_logEdit,1);setCentralWidget(central);

    connect(m_saveButton,&QPushButton::clicked,this,&ConsumerWindow::saveSettings);connect(m_startButton,&QPushButton::clicked,this,&ConsumerWindow::startConsumer);connect(m_stopButton,&QPushButton::clicked,this,&ConsumerWindow::stopConsumer);connect(refresh,&QPushButton::clicked,this,&ConsumerWindow::refreshDevices);connect(add,&QPushButton::clicked,this,&ConsumerWindow::addDevice);connect(remove,&QPushButton::clicked,this,&ConsumerWindow::removeDevice);connect(m_deviceList,&QListWidget::itemSelectionChanged,this,&ConsumerWindow::onDeviceSelected);connect(submit,&QPushButton::clicked,this,&ConsumerWindow::submitParameters);connect(m_refreshTimer,&QTimer::timeout,this,&ConsumerWindow::refreshDevices);
    createDefaultConfig();loadSettings();setRunning(false);m_refreshTimer->start(5000);refreshDevices();
}
ConsumerWindow::~ConsumerWindow()=default;

void ConsumerWindow::createDefaultConfig() const{if(QFile::exists(m_configPath))return;QSettings s(m_configPath,QSettings::IniFormat);s.setValue("Server/url","http://39.96.41.239:443");s.setValue("Server/downloadToken","");s.setValue("Consumer/pollSeconds",5);s.setValue("Consumer/outputDir","data/mseed");s.setValue("Consumer/stateFile","data/state");s.sync();}
void ConsumerWindow::loadSettings(){QSettings s(m_configPath,QSettings::IniFormat);m_serverEdit->setText(s.value("Server/url").toString());m_tokenEdit->setText(s.value("Server/downloadToken").toString());m_outputEdit->setText(s.value("Consumer/outputDir","data/mseed").toString());m_stateEdit->setText(s.value("Consumer/stateFile","data/state").toString());m_intervalSpin->setValue(s.value("Consumer/pollSeconds",5).toInt());m_uploadUrlEdit->setText(m_serverEdit->text());}
void ConsumerWindow::saveSettings(){QSettings s(m_configPath,QSettings::IniFormat);s.setValue("Server/url",m_serverEdit->text().trimmed());s.setValue("Server/downloadToken",m_tokenEdit->text().trimmed());s.setValue("Consumer/outputDir",m_outputEdit->text().trimmed());s.setValue("Consumer/stateFile",m_stateEdit->text().trimmed());s.setValue("Consumer/pollSeconds",m_intervalSpin->value());s.sync();appendLog(QStringLiteral("配置已保存"));}
QString ConsumerWindow::selectedDeviceId()const{return m_deviceList->currentItem()?m_deviceList->currentItem()->data(Qt::UserRole).toString():QString();}
QString ConsumerWindow::apiUrl(const QString& path)const{QString base=m_serverEdit->text().trimmed();while(base.endsWith('/'))base.chop(1);return base+path;}

void ConsumerWindow::refreshDevices(){QNetworkReply* r=m_network->get(QNetworkRequest(QUrl(apiUrl("/coalmine/devices"))));connect(r,&QNetworkReply::finished,this,[this,r](){if(r->error()!=QNetworkReply::NoError){handleNetworkError(QStringLiteral("刷新设备"),r);r->deleteLater();return;}QString selected=selectedDeviceId();QJsonArray arr=QJsonDocument::fromJson(r->readAll()).array();{QSignalBlocker blocker(m_deviceList);m_deviceList->clear();for(const QJsonValue& v:arr){QJsonObject o=v.toObject();if(!o.value("enabled").toBool())continue;QString id=o.value("device_id").toString();QListWidgetItem* item=new QListWidgetItem(QStringLiteral("%1  %2").arg(id,o.value("online").toBool()?QStringLiteral("在线"):QStringLiteral("离线")),m_deviceList);item->setData(Qt::UserRole,id);if(id==selected)m_deviceList->setCurrentItem(item);}if(!m_deviceList->currentItem()&&m_deviceList->count())m_deviceList->setCurrentRow(0);}onDeviceSelected();r->deleteLater();});}
void ConsumerWindow::addDevice(){bool ok=false;QString id=QInputDialog::getText(this,QStringLiteral("新增设备"),QStringLiteral("设备编号"),QLineEdit::Normal,QString(),&ok).trimmed();if(!ok||id.isEmpty())return;QJsonObject body{{"name",QStringLiteral("EG628-%1").arg(id)}};QNetworkRequest req(QUrl(apiUrl("/coalmine/devices/"+id)));req.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");QNetworkReply* r=m_network->put(req,QJsonDocument(body).toJson(QJsonDocument::Compact));connect(r,&QNetworkReply::finished,this,[this,r](){if(r->error()!=QNetworkReply::NoError)handleNetworkError(QStringLiteral("新增设备"),r);else refreshDevices();r->deleteLater();});}
void ConsumerWindow::removeDevice(){QString id=selectedDeviceId();if(id.isEmpty()||QMessageBox::question(this,QStringLiteral("停用设备"),QStringLiteral("确定停用设备 %1？历史文件不会删除。").arg(id))!=QMessageBox::Yes)return;QNetworkRequest req(QUrl(apiUrl("/coalmine/devices/"+id)));QNetworkReply* r=m_network->deleteResource(req);connect(r,&QNetworkReply::finished,this,[this,r](){if(r->error()!=QNetworkReply::NoError)handleNetworkError(QStringLiteral("停用设备"),r);else refreshDevices();r->deleteLater();});}
void ConsumerWindow::onDeviceSelected(){QString id=selectedDeviceId();if(id.isEmpty())return;QNetworkReply* r=m_network->get(QNetworkRequest(QUrl(apiUrl("/coalmine/devices/"+id))));connect(r,&QNetworkReply::finished,this,[this,r](){if(r->error()!=QNetworkReply::NoError)handleNetworkError(QStringLiteral("获取设备状态"),r);else showDevice(QJsonDocument::fromJson(r->readAll()).object());r->deleteLater();});QNetworkReply* cr=m_network->get(QNetworkRequest(QUrl(apiUrl("/coalmine/devices/"+id+"/config/status"))));connect(cr,&QNetworkReply::finished,this,[this,cr](){int status=cr->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();if(status==200){QJsonObject cfg=QJsonDocument::fromJson(cr->readAll()).object();QJsonObject p=cfg.value("parameters").toObject();for(auto it=m_parameterEdits.begin();it!=m_parameterEdits.end();++it)if(p.contains(it.key()))it.value()->setText(QString::number(p.value(it.key()).toDouble(),'g',12));m_uploadUrlEdit->setText(cfg.value("upload_server_url").toString());}cr->deleteLater();});if(m_consumer&&m_consumingDeviceId!=id){stopConsumer();startConsumer();}}
void ConsumerWindow::showDevice(const QJsonObject& o){m_deviceStatus->setText(o.value("online").toBool()?QStringLiteral("在线"):QStringLiteral("离线"));m_deviceIp->setText(o.value("remote_ip").toString()+" / "+o.value("status").toObject().value("local_ip").toString());QJsonObject status=o.value("status").toObject();m_programVersion->setText(status.value("program_version").toString());m_configVersion->setText(QStringLiteral("已应用 %1 / 期望 %2").arg(o.value("applied_config_version").toInt()).arg(o.value("desired_config_version").toInt()));QJsonArray stations=status.value("stations").toArray();m_stationTable->setRowCount(stations.size());for(int i=0;i<stations.size();++i){QJsonObject s=stations.at(i).toObject();QStringList values={QString::number(s.value("station_number").toInt()),s.value("connected").toBool()?QStringLiteral("在线"):QStringLiteral("离线"),QString::number(s.value("online_channels").toInt()),QString::number(s.value("total_channels").toInt()),QString::number(s.value("gps").toInt())};for(int c=0;c<values.size();++c)m_stationTable->setItem(i,c,new QTableWidgetItem(values.at(c)));}}
void ConsumerWindow::submitParameters(){QString id=selectedDeviceId();if(id.isEmpty()){QMessageBox::warning(this,QStringLiteral("参数"),QStringLiteral("请先选择设备"));return;}QJsonObject p;for(auto it=m_parameterEdits.constBegin();it!=m_parameterEdits.constEnd();++it)p.insert(it.key(),it.value()->text().toDouble());QJsonObject body{{"parameters",p},{"upload_server_url",m_uploadUrlEdit->text().trimmed()}};QNetworkRequest req(QUrl(apiUrl("/coalmine/devices/"+id+"/config")));req.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");QNetworkReply* r=m_network->put(req,QJsonDocument(body).toJson(QJsonDocument::Compact));connect(r,&QNetworkReply::finished,this,[this,r,id](){if(r->error()!=QNetworkReply::NoError)handleNetworkError(QStringLiteral("下发参数"),r);else{QJsonObject o=QJsonDocument::fromJson(r->readAll()).object();appendLog(QStringLiteral("设备 %1 参数版本 %2 已提交，等待设备应用").arg(id).arg(o.value("version").toInt()));onDeviceSelected();}r->deleteLater();});}

void ConsumerWindow::startConsumer(){QString id=selectedDeviceId();if(id.isEmpty()){appendLog(QStringLiteral("请先选择设备"));return;}if(m_serverEdit->text().trimmed().isEmpty()||m_tokenEdit->text().trimmed().isEmpty()){appendLog(QStringLiteral("服务器地址或下载令牌为空"));return;}saveSettings();QString out=QDir(absoluteConfigPath(m_outputEdit->text().trimmed())).absoluteFilePath(id);QString stateBase=absoluteConfigPath(m_stateEdit->text().trimmed());if(QFileInfo(stateBase).suffix().compare("txt",Qt::CaseInsensitive)==0)stateBase=QFileInfo(stateBase).absolutePath();QString stateDir=QDir(stateBase).absoluteFilePath(id);m_consumer.reset(new MseedConsumer(QUrl(m_serverEdit->text().trimmed()),m_tokenEdit->text().trimmed().toUtf8(),id,out,QDir(stateDir).absoluteFilePath("cursor.txt"),m_intervalSpin->value(),this));connect(m_consumer.get(),&MseedConsumer::statusChanged,m_statusLabel,&QLabel::setText);connect(m_consumer.get(),&MseedConsumer::cursorChanged,this,[this](qint64 v){m_cursorLabel->setText(QString::number(v));});connect(m_consumer.get(),&MseedConsumer::fileDownloaded,this,[this](qint64,const QString&,const QString& path){m_lastFileLabel->setText(path);});connect(m_consumer.get(),&MseedConsumer::logMessage,this,&ConsumerWindow::appendLog);if(!m_consumer->start()){m_consumer.reset();return;}m_consumingDeviceId=id;setRunning(true);}
void ConsumerWindow::stopConsumer(){if(m_consumer)m_consumer->stop();m_consumer.reset();m_consumingDeviceId.clear();setRunning(false);}
void ConsumerWindow::appendLog(const QString& m){m_logEdit->appendPlainText(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"),m));}
QString ConsumerWindow::absoluteConfigPath(const QString& v)const{QFileInfo i(v);return i.isAbsolute()?i.absoluteFilePath():QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(v);}
void ConsumerWindow::setRunning(bool running){m_startButton->setEnabled(!running);m_stopButton->setEnabled(running);}
void ConsumerWindow::handleNetworkError(const QString& op,QNetworkReply* r){appendLog(QStringLiteral("%1失败：HTTP=%2 %3").arg(op).arg(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()).arg(r->errorString()));}
